/**************************************************************************/
/*  horde_flow_field.cpp                                                   */
/**************************************************************************/

#include "horde_flow_field.h"

#include "core/object/class_db.h"
#include "core/os/os.h"

#include <string.h>

// Planar neighbor offsets in octant order (see OCTANT_* in the header).
static const int32_t OCT_DX[8] = { 1, 1, 0, -1, -1, -1, 0, 1 };
static const int32_t OCT_DY[8] = { 0, 1, 1, 1, 0, -1, -1, -1 };
static const bool OCT_DIAG[8] = { false, true, false, true, false, true, false, true };
static const bool OCT_ALL_OK[8] = { true, true, true, true, true, true, true, true };

// Per-move cost multipliers (integer, deterministic). Diagonal ~= sqrt(2).
static constexpr uint32_t STEP_ORTHO = 10;
static constexpr uint32_t STEP_DIAG = 14;

// Effective per-cell traversal cost is clamped to this range. The upper bound
// keeps the integer integration field well clear of uint32 overflow for any
// realistic map/path length while leaving ample headroom for "costly shortcut"
// weighting.
static constexpr uint32_t MAX_CELL_COST = 4095;

// Dial's bucket-queue window. Power of two so the circular index is a mask,
// not a hardware division; must cover the maximum edge weight so the active
// key window never aliases.
static constexpr uint32_t BUCKET_COUNT = 65536;
static_assert((BUCKET_COUNT & (BUCKET_COUNT - 1)) == 0, "BUCKET_COUNT must be a power of two.");
static_assert(BUCKET_COUNT >= MAX_CELL_COST * STEP_DIAG + 1, "Bucket window must cover the maximum edge weight.");

// Dial's bucket queue over flat arrays (circular bucket heads + append-only
// node pool): O(1) push/pop, no comparison-heap sifts, no per-cell heap
// objects. Determinism: strictly sequential, LIFO buckets, and strict
// improvement in relax() make the visit order -- and therefore the output
// field -- a pure function of the input.
struct DialQueue {
	uint32_t *integ = nullptr;
	uint16_t *gid = nullptr;
	int32_t *bhead = nullptr;
	uint64_t *nkey = nullptr; // Packed (cost << 32) | cell.
	int32_t *nnext = nullptr;
	uint32_t node_count = 0;
	uint32_t pending = 0;

	// Relax the edge into cell `p_v` with candidate integration cost `p_nc`
	// propagated from goal `p_gid`. Shared by goal seeding, planar edges, and
	// inter-floor link edges.
	_FORCE_INLINE_ void relax(int32_t p_v, uint32_t p_nc, uint16_t p_gid) {
		if (p_nc >= integ[p_v]) {
			return;
		}
		integ[p_v] = p_nc;
		gid[p_v] = p_gid;
		const uint32_t b = p_nc & (BUCKET_COUNT - 1);
		nkey[node_count] = ((uint64_t)p_nc << 32) | (uint32_t)p_v;
		nnext[node_count] = bhead[b];
		bhead[b] = (int32_t)node_count;
		node_count++;
		pending++;
	}
};

// Steepest-descent candidate tracker shared by the planar and link neighbor
// scans of the direction pass.
struct DescentPick {
	uint32_t best = 0;
	int32_t oct = -1; // OCTANT_* value; OCTANT_LINK for link edges.
	int32_t link = -1;

	_FORCE_INLINE_ void consider(uint32_t p_integ, int32_t p_oct, int32_t p_link) {
		if (p_integ < best) {
			best = p_integ;
			oct = p_oct;
			link = p_link;
		}
	}
};

HordeFlowField::~HordeFlowField() {
	// Never leave a worker task dangling against a freed field.
	if (in_flight.is_set() && task_id != WorkerThreadPool::INVALID_TASK_ID) {
		WorkerThreadPool::get_singleton()->wait_for_task_completion(task_id);
		in_flight.clear();
	}
}

void HordeFlowField::set_grid(const Ref<HordeNavGrid> &p_grid) {
	ERR_FAIL_COND_MSG(in_flight.is_set(), "Cannot change grid while a recompute is in flight.");
	grid = p_grid;
	published = false;
	cell_count = grid.is_valid() ? grid->get_cell_count() : 0;

	// Size both buffers so the front buffer is a valid (empty) field even
	// before the first recompute completes. All four reset patterns are 0xFF
	// bytes (COST_UNREACHABLE, OCTANT_NONE, GOAL_NONE, -1).
	for (int b = 0; b < 2; b++) {
		FieldBuffer &fb = buffers[b];
		fb.resize((uint32_t)cell_count);
		if (cell_count > 0) {
			memset(fb.integration.ptr(), 0xFF, (size_t)cell_count * sizeof(uint32_t));
			memset(fb.dir_octant.ptr(), 0xFF, (size_t)cell_count * sizeof(uint8_t));
			memset(fb.goal_id.ptr(), 0xFF, (size_t)cell_count * sizeof(uint16_t));
			memset(fb.link_step.ptr(), 0xFF, (size_t)cell_count * sizeof(int32_t));
		}
		buffer_seq[b].set(0); // Stable (even); no recompute is in flight here.
	}
	front_index.set(0);
}

void HordeFlowField::clear_goals() {
	goals.clear();
}

void HordeFlowField::add_goal(int32_t p_x, int32_t p_y, int32_t p_floor) {
	ERR_FAIL_COND(grid.is_null());
	ERR_FAIL_COND(!grid->in_bounds(p_x, p_y, p_floor));
	goals.push_back(grid->cell_index(p_x, p_y, p_floor));
}

void HordeFlowField::add_goal_index(int32_t p_index) {
	ERR_FAIL_COND(grid.is_null());
	ERR_FAIL_INDEX(p_index, cell_count);
	goals.push_back(p_index);
}

void HordeFlowField::set_goals(const PackedInt32Array &p_indices) {
	ERR_FAIL_COND(grid.is_null());
	goals.clear();
	for (int i = 0; i < p_indices.size(); i++) {
		const int32_t idx = p_indices[i];
		ERR_CONTINUE_MSG(idx < 0 || idx >= cell_count, "Goal cell index out of range.");
		goals.push_back(idx);
	}
}

void HordeFlowField::_snapshot_grid() {
	// Runs on the main thread: build a self-contained snapshot the worker reads
	// so grid edits after dispatch cannot race the compute.
	job.width = grid->get_width();
	job.height = grid->get_height();
	job.floors = grid->get_floors();
	job.back = 1 - front_index.get();

	job.cost.resize((uint32_t)cell_count);
	for (int32_t i = 0; i < cell_count; i++) {
		if (grid->walkable[i] != 0 && grid->blocked[i] == 0) {
			uint32_t c = (uint32_t)grid->base_cost[i] + (uint32_t)grid->dynamic_cost[i];
			c = CLAMP(c, 1u, MAX_CELL_COST);
			job.cost[i] = c;
		} else {
			job.cost[i] = 0; // Impassable.
		}
	}

	// Only passable goals contribute a source.
	job.goals.clear();
	for (uint32_t g = 0; g < goals.size(); g++) {
		const int32_t gi = goals[g];
		if (gi >= 0 && gi < cell_count && job.cost[gi] != 0) {
			job.goals.push_back(gi);
		}
	}

	// Build the link CSR (source-cell -> outgoing links).
	LinkCSR &csr = job.links;
	csr.offsets.resize((uint32_t)cell_count + 1);
	for (int32_t i = 0; i <= cell_count; i++) {
		csr.offsets[i] = 0;
	}
	const LocalVector<HordeNavGrid::GridLink> &glinks = grid->links;
	for (uint32_t i = 0; i < glinks.size(); i++) {
		csr.offsets[glinks[i].from + 1]++;
	}
	for (int32_t i = 0; i < cell_count; i++) {
		csr.offsets[i + 1] += csr.offsets[i];
	}
	const uint32_t link_total = glinks.size();
	csr.targets.resize(link_total);
	csr.costs.resize(link_total);
	if (job.link_cursor.size() < (uint32_t)cell_count) {
		job.link_cursor.resize((uint32_t)cell_count); // Grow-only scratch.
	}
	int32_t *cursor = job.link_cursor.ptr();
	for (int32_t i = 0; i < cell_count; i++) {
		cursor[i] = csr.offsets[i];
	}
	for (uint32_t i = 0; i < glinks.size(); i++) {
		const int32_t from = glinks[i].from;
		const int32_t slot = cursor[from]++;
		csr.targets[slot] = glinks[i].to;
		// Clamped so every edge weight fits the bucket-queue window (see
		// BUCKET_COUNT in _compute()).
		csr.costs[slot] = MIN((uint32_t)glinks[i].cost, MAX_CELL_COST) * STEP_ORTHO;
	}
}

void HordeFlowField::_dispatch() {
	ERR_FAIL_COND(grid.is_null());
	_snapshot_grid();
	dirty = false;
	in_flight.set();
	task_id = WorkerThreadPool::get_singleton()->add_native_task(&HordeFlowField::_worker_entry, this, true, "HordeFlowField recompute");
}

void HordeFlowField::_worker_entry(void *p_userdata) {
	static_cast<HordeFlowField *>(p_userdata)->_compute();
}

void HordeFlowField::_compute() {
	const uint64_t t0 = OS::get_singleton()->get_ticks_usec();

	const int32_t w = job.width;
	const int32_t h = job.height;
	const int32_t layer = w * h;
	const uint32_t *cost = job.cost.ptr();

	FieldBuffer &out = buffers[job.back];
	uint32_t *integ = out.integration.ptr();
	uint8_t *dir = out.dir_octant.ptr();
	uint16_t *gid = out.goal_id.ptr();
	int32_t *lstep = out.link_step.ptr();

	// Seqlock write bracket, part 1: mark the back buffer write-in-progress
	// (odd) BEFORE its first byte changes. A lagging reader that snapshotted
	// this buffer while it was still the front sees the sequence change on its
	// recheck and retries instead of returning torn data. The acq_rel RMW keeps
	// the writes below from being reordered above it.
	buffer_seq[job.back].increment();

	// All four reset patterns are 0xFF bytes (COST_UNREACHABLE, GOAL_NONE,
	// OCTANT_NONE, -1), so plain memsets clear the whole back buffer.
	memset(integ, 0xFF, (size_t)cell_count * sizeof(uint32_t));
	memset(gid, 0xFF, (size_t)cell_count * sizeof(uint16_t));
	memset(dir, 0xFF, (size_t)cell_count * sizeof(uint8_t));
	memset(lstep, 0xFF, (size_t)cell_count * sizeof(int32_t));

	// Per-octant precomputed tables: linear index delta, step multiplier, and
	// (for diagonals) the row-axis side delta for the corner-cut check (the
	// column-axis side delta is just OCT_DX).
	int32_t oct_off[8];
	uint32_t oct_step[8];
	int32_t oct_side_row[8];
	for (int o = 0; o < 8; o++) {
		oct_off[o] = OCT_DY[o] * w + OCT_DX[o];
		oct_step[o] = OCT_DIAG[o] ? STEP_DIAG : STEP_ORTHO;
		oct_side_row[o] = OCT_DY[o] * w;
	}

	// Every edge weight is at most MAX_CELL_COST * STEP_DIAG, so at any moment
	// all pending keys live inside one BUCKET_COUNT window and the circular
	// bucket index (key & mask) is unambiguous. Each edge pushes at most once
	// (relax() requires strict improvement and each cell settles exactly once),
	// so 8 * cell_count + links + goals bounds the node pool and it never
	// reallocates mid-loop. Raw pointers matter here: LocalVector's checked
	// operator[] and disabled inlining are a ~2x dev_build penalty.
	if (job.bucket_head.size() < BUCKET_COUNT) {
		job.bucket_head.resize(BUCKET_COUNT);
	}
	const uint32_t node_capacity = (uint32_t)cell_count * 8 + job.links.targets.size() + job.goals.size() + 1;
	if (job.node_key_cell.size() < node_capacity) {
		job.node_key_cell.resize(node_capacity); // Trivial types: uninitialized.
		job.node_next.resize(node_capacity);
	}
	DialQueue q;
	q.integ = integ;
	q.gid = gid;
	q.bhead = job.bucket_head.ptr();
	q.nkey = job.node_key_cell.ptr();
	q.nnext = job.node_next.ptr();
	memset(q.bhead, 0xFF, BUCKET_COUNT * sizeof(int32_t)); // All heads = -1.

	for (uint32_t g = 0; g < job.goals.size(); g++) {
		q.relax(job.goals[g], 0, (uint16_t)g); // Duplicate goal cells no-op.
	}

	const LinkCSR &csr = job.links;
	const int32_t *link_off = csr.offsets.ptr();
	const int32_t *link_tgt = csr.targets.ptr();
	const uint32_t *link_cost = csr.costs.ptr();
	const bool single_floor = job.floors == 1;

	// Local aliases keep the pop path free of struct indirection at /Od.
	int32_t *const bhead = q.bhead;
	const int32_t *const nnext = q.nnext;
	const uint64_t *const nkey = q.nkey;

	uint32_t cur_bucket = 0;
	while (q.pending > 0) {
		int32_t ni = bhead[cur_bucket];
		while (ni < 0) {
			cur_bucket = (cur_bucket + 1) & (BUCKET_COUNT - 1);
			ni = bhead[cur_bucket];
		}
		bhead[cur_bucket] = nnext[ni];
		q.pending--;

		const uint64_t top = nkey[ni];
		const uint32_t u = (uint32_t)(top & 0xFFFFFFFFu);
		const uint32_t ucost = (uint32_t)(top >> 32);

		if (ucost > integ[u]) {
			continue; // Stale entry (superseded by a cheaper push).
		}

		int32_t ux, uy;
		if (single_floor) {
			ux = (int32_t)u % w;
			uy = (int32_t)u / w;
		} else {
			const int32_t rem = (int32_t)u % layer;
			ux = rem % w;
			uy = rem / w;
		}
		const uint16_t ugid = gid[u];

		// Octant in-bounds masks. Interior cells -- the overwhelming majority --
		// share a static all-true row instead of computing masks per pop.
		const bool *oct_ok = OCT_ALL_OK;
		bool border_ok[8];
		if (unlikely(ux == 0 || ux + 1 == w || uy == 0 || uy + 1 == h)) {
			const bool ok_e = ux + 1 < w;
			const bool ok_w2 = ux > 0;
			const bool ok_n = uy + 1 < h;
			const bool ok_s = uy > 0;
			border_ok[0] = ok_e;
			border_ok[1] = ok_e && ok_n;
			border_ok[2] = ok_n;
			border_ok[3] = ok_w2 && ok_n;
			border_ok[4] = ok_w2;
			border_ok[5] = ok_w2 && ok_s;
			border_ok[6] = ok_s;
			border_ok[7] = ok_e && ok_s;
			oct_ok = border_ok;
		}

		// Planar 8-connected relaxation (no corner cutting).
		for (int o = 0; o < 8; o++) {
			if (!oct_ok[o]) {
				continue;
			}
			const int32_t v = (int32_t)u + oct_off[o];
			const uint32_t vcost = cost[v];
			if (vcost == 0) {
				continue;
			}
			if (OCT_DIAG[o]) {
				// Both orthogonal cells must be passable.
				if (cost[u + OCT_DX[o]] == 0 || cost[u + oct_side_row[o]] == 0) {
					continue;
				}
			}
			q.relax(v, ucost + vcost * oct_step[o], ugid);
		}

		// Inter-floor link edges relax through the same queue as planar edges.
		// With no links every offset span is empty and this no-ops.
		const int32_t link_end = link_off[u + 1];
		for (int32_t k = link_off[u]; k < link_end; k++) {
			const int32_t v = link_tgt[k];
			if (cost[v] == 0) {
				continue;
			}
			q.relax(v, ucost + link_cost[k], ugid);
		}
	}

	// Direction pass: steepest descent toward the lowest-integration neighbor.
	// Iterated as nested floor/y/x loops so the cell index is incremental (no
	// div/mod) and border checks hoist out of the octant loop.
	int32_t u = 0;
	for (int32_t f = 0; f < job.floors; f++) {
		for (int32_t uy = 0; uy < h; uy++) {
			const bool ok_n = uy + 1 < h;
			const bool ok_s = uy > 0;
			for (int32_t ux = 0; ux < w; ux++, u++) {
				if (cost[u] == 0 || integ[u] == COST_UNREACHABLE) {
					dir[u] = (uint8_t)OCTANT_NONE;
					continue;
				}
				if (integ[u] == 0) {
					dir[u] = (uint8_t)OCTANT_GOAL;
					continue;
				}
				const bool *oct_ok = OCT_ALL_OK;
				bool border_ok[8];
				if (unlikely(ux == 0 || ux + 1 == w || !ok_n || !ok_s)) {
					const bool ok_e = ux + 1 < w;
					const bool ok_w2 = ux > 0;
					border_ok[0] = ok_e;
					border_ok[1] = ok_e && ok_n;
					border_ok[2] = ok_n;
					border_ok[3] = ok_w2 && ok_n;
					border_ok[4] = ok_w2;
					border_ok[5] = ok_w2 && ok_s;
					border_ok[6] = ok_s;
					border_ok[7] = ok_e && ok_s;
					oct_ok = border_ok;
				}

				DescentPick pick;
				pick.best = integ[u];
				for (int o = 0; o < 8; o++) {
					if (!oct_ok[o]) {
						continue;
					}
					const int32_t v = u + oct_off[o];
					if (cost[v] == 0) {
						continue;
					}
					if (OCT_DIAG[o]) {
						if (cost[u + OCT_DX[o]] == 0 || cost[u + oct_side_row[o]] == 0) {
							continue;
						}
					}
					pick.consider(integ[v], o, -1);
				}
				const int32_t link_end = link_off[u + 1];
				for (int32_t k = link_off[u]; k < link_end; k++) {
					const int32_t v = link_tgt[k];
					if (cost[v] == 0) {
						continue;
					}
					pick.consider(integ[v], OCTANT_LINK, v);
				}

				if (pick.oct < 0) {
					dir[u] = (uint8_t)OCTANT_NONE; // Local minimum with no descending neighbor.
				} else {
					dir[u] = (uint8_t)pick.oct;
					if (pick.oct == OCTANT_LINK) {
						lstep[u] = pick.link;
					}
				}
			}
		}
	}

	last_compute_usec = OS::get_singleton()->get_ticks_usec() - t0;

	// Seqlock write bracket, part 2: back buffer stable again (even). The
	// acq_rel RMW keeps the writes above from being reordered below it.
	buffer_seq[job.back].increment();

	// Publish: single atomic release-store. Readers acquire-load front_index
	// before reading, so they never observe this half-written buffer.
	front_index.set(job.back);
}

void HordeFlowField::request_recompute() {
	ERR_FAIL_COND(grid.is_null());
	dirty = true;
	if (!in_flight.is_set()) {
		_dispatch();
	}
}

void HordeFlowField::_finish() {
	WorkerThreadPool::get_singleton()->wait_for_task_completion(task_id);
	in_flight.clear();
	task_id = WorkerThreadPool::INVALID_TASK_ID;
	published = true;
}

bool HordeFlowField::poll() {
	if (!in_flight.is_set()) {
		return false;
	}
	if (!WorkerThreadPool::get_singleton()->is_task_completed(task_id)) {
		return false;
	}
	_finish();

	// Debounced follow-up: the grid changed again while we were computing.
	if (dirty) {
		_dispatch();
	}
	return true;
}

void HordeFlowField::recompute_sync() {
	ERR_FAIL_COND(grid.is_null());
	dirty = true;
	while (dirty || in_flight.is_set()) {
		if (!in_flight.is_set()) {
			_dispatch();
		}
		_finish();
	}
}

Vector2 HordeFlowField::octant_to_vector(int32_t p_octant) {
	static const Vector2 DIR_LUT[8] = {
		Vector2(1, 0),
		Vector2(Math::SQRT12, Math::SQRT12),
		Vector2(0, 1),
		Vector2(-Math::SQRT12, Math::SQRT12),
		Vector2(-1, 0),
		Vector2(-Math::SQRT12, -Math::SQRT12),
		Vector2(0, -1),
		Vector2(Math::SQRT12, -Math::SQRT12),
	};
	if (p_octant < OCTANT_E || p_octant > OCTANT_SE) {
		return Vector2(0, 0); // OCTANT_GOAL / OCTANT_LINK / OCTANT_NONE.
	}
	return DIR_LUT[p_octant];
}

// Empty interleave hook for the production sampling paths (inlines away).
static _FORCE_INLINE_ void _no_hook() {}

Vector2 HordeFlowField::direction_at_index(int32_t p_index) const {
	if (!published || p_index < 0 || p_index >= cell_count) {
		return Vector2(0, 0);
	}
	return octant_to_vector(_coherent_sample([p_index](const FieldBuffer &b) { return b.dir_octant[p_index]; }, _no_hook));
}

Vector2 HordeFlowField::direction_at(int32_t p_x, int32_t p_y, int32_t p_floor) const {
	ERR_FAIL_COND_V(grid.is_null(), Vector2());
	if (!grid->in_bounds(p_x, p_y, p_floor)) {
		return Vector2(0, 0);
	}
	return direction_at_index(grid->cell_index(p_x, p_y, p_floor));
}

int32_t HordeFlowField::octant_at_index(int32_t p_index) const {
	if (!published || p_index < 0 || p_index >= cell_count) {
		return OCTANT_NONE;
	}
	return _coherent_sample([p_index](const FieldBuffer &b) { return (int32_t)b.dir_octant[p_index]; }, _no_hook);
}

int32_t HordeFlowField::cost_at_index(int32_t p_index) const {
	if (!published || p_index < 0 || p_index >= cell_count) {
		return -1;
	}
	const uint32_t c = _coherent_sample([p_index](const FieldBuffer &b) { return b.integration[p_index]; }, _no_hook);
	return (c == COST_UNREACHABLE) ? -1 : (int32_t)c;
}

int32_t HordeFlowField::cost_at_index_interleaved(int32_t p_index, void (*p_hook)(void *), void *p_hook_userdata) const {
	if (!published || p_index < 0 || p_index >= cell_count) {
		return -1;
	}
	const uint32_t c = _coherent_sample(
			[p_index](const FieldBuffer &b) { return b.integration[p_index]; },
			[p_hook, p_hook_userdata] { p_hook(p_hook_userdata); });
	return (c == COST_UNREACHABLE) ? -1 : (int32_t)c;
}

int32_t HordeFlowField::octant_at_index_interleaved(int32_t p_index, void (*p_hook)(void *), void *p_hook_userdata) const {
	if (!published || p_index < 0 || p_index >= cell_count) {
		return OCTANT_NONE;
	}
	return _coherent_sample(
			[p_index](const FieldBuffer &b) { return (int32_t)b.dir_octant[p_index]; },
			[p_hook, p_hook_userdata] { p_hook(p_hook_userdata); });
}

int32_t HordeFlowField::cost_at(int32_t p_x, int32_t p_y, int32_t p_floor) const {
	ERR_FAIL_COND_V(grid.is_null(), -1);
	if (!grid->in_bounds(p_x, p_y, p_floor)) {
		return -1;
	}
	return cost_at_index(grid->cell_index(p_x, p_y, p_floor));
}

bool HordeFlowField::is_reachable(int32_t p_x, int32_t p_y, int32_t p_floor) const {
	return cost_at(p_x, p_y, p_floor) >= 0;
}

int32_t HordeFlowField::goal_at(int32_t p_x, int32_t p_y, int32_t p_floor) const {
	ERR_FAIL_COND_V(grid.is_null(), -1);
	if (!published || !grid->in_bounds(p_x, p_y, p_floor)) {
		return -1;
	}
	const int32_t idx = grid->cell_index(p_x, p_y, p_floor);
	const uint16_t g = _coherent_sample([idx](const FieldBuffer &b) { return b.goal_id[idx]; }, _no_hook);
	return (g == GOAL_NONE) ? -1 : (int32_t)g;
}

int32_t HordeFlowField::link_target_at_index(int32_t p_index) const {
	if (!published || p_index < 0 || p_index >= cell_count) {
		return -1;
	}
	return _coherent_sample([p_index](const FieldBuffer &b) { return b.link_step[p_index]; }, _no_hook);
}

void HordeFlowField::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_grid", "grid"), &HordeFlowField::set_grid);
	ClassDB::bind_method(D_METHOD("get_grid"), &HordeFlowField::get_grid);

	ClassDB::bind_method(D_METHOD("clear_goals"), &HordeFlowField::clear_goals);
	ClassDB::bind_method(D_METHOD("add_goal", "x", "y", "floor"), &HordeFlowField::add_goal);
	ClassDB::bind_method(D_METHOD("add_goal_index", "index"), &HordeFlowField::add_goal_index);
	ClassDB::bind_method(D_METHOD("set_goals", "cell_indices"), &HordeFlowField::set_goals);
	ClassDB::bind_method(D_METHOD("get_goal_count"), &HordeFlowField::get_goal_count);

	ClassDB::bind_method(D_METHOD("request_recompute"), &HordeFlowField::request_recompute);
	ClassDB::bind_method(D_METHOD("poll"), &HordeFlowField::poll);
	ClassDB::bind_method(D_METHOD("recompute_sync"), &HordeFlowField::recompute_sync);
	ClassDB::bind_method(D_METHOD("is_recomputing"), &HordeFlowField::is_recomputing);
	ClassDB::bind_method(D_METHOD("has_field"), &HordeFlowField::has_field);

	ClassDB::bind_method(D_METHOD("direction_at", "x", "y", "floor"), &HordeFlowField::direction_at);
	ClassDB::bind_method(D_METHOD("direction_at_index", "index"), &HordeFlowField::direction_at_index);
	ClassDB::bind_method(D_METHOD("octant_at_index", "index"), &HordeFlowField::octant_at_index);
	ClassDB::bind_method(D_METHOD("cost_at", "x", "y", "floor"), &HordeFlowField::cost_at);
	ClassDB::bind_method(D_METHOD("cost_at_index", "index"), &HordeFlowField::cost_at_index);
	ClassDB::bind_method(D_METHOD("is_reachable", "x", "y", "floor"), &HordeFlowField::is_reachable);
	ClassDB::bind_method(D_METHOD("goal_at", "x", "y", "floor"), &HordeFlowField::goal_at);
	ClassDB::bind_method(D_METHOD("link_target_at_index", "index"), &HordeFlowField::link_target_at_index);

	ClassDB::bind_method(D_METHOD("get_last_compute_usec"), &HordeFlowField::get_last_compute_usec);

	BIND_ENUM_CONSTANT(OCTANT_E);
	BIND_ENUM_CONSTANT(OCTANT_NE);
	BIND_ENUM_CONSTANT(OCTANT_N);
	BIND_ENUM_CONSTANT(OCTANT_NW);
	BIND_ENUM_CONSTANT(OCTANT_W);
	BIND_ENUM_CONSTANT(OCTANT_SW);
	BIND_ENUM_CONSTANT(OCTANT_S);
	BIND_ENUM_CONSTANT(OCTANT_SE);
	BIND_ENUM_CONSTANT(OCTANT_GOAL);
	BIND_ENUM_CONSTANT(OCTANT_LINK);
	BIND_ENUM_CONSTANT(OCTANT_NONE);
}

