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

// Effective per-cell traversal cost is clamped to this range. The upper bound
// keeps the integer integration field well clear of uint32 overflow for any
// realistic map/path length while leaving ample headroom for "costly shortcut"
// weighting.
static constexpr uint32_t MAX_CELL_COST = 4095;

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
	// before the first recompute completes.
	for (int b = 0; b < 2; b++) {
		buffers[b].resize((uint32_t)cell_count);
		for (int32_t i = 0; i < cell_count; i++) {
			buffers[b].integration[i] = COST_UNREACHABLE;
			buffers[b].dir_octant[i] = OCTANT_NONE;
			buffers[b].goal_id[i] = GOAL_NONE;
			buffers[b].link_step[i] = -1;
		}
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
	LocalVector<int32_t> cursor;
	cursor.resize((uint32_t)cell_count);
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
	dispatched_version = grid->get_version();
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

	// All four reset patterns are 0xFF bytes (COST_UNREACHABLE, GOAL_NONE,
	// OCTANT_NONE, -1), so plain memsets clear the whole back buffer.
	memset(integ, 0xFF, (size_t)cell_count * sizeof(uint32_t));
	memset(gid, 0xFF, (size_t)cell_count * sizeof(uint16_t));
	memset(dir, 0xFF, (size_t)cell_count * sizeof(uint8_t));
	memset(lstep, 0xFF, (size_t)cell_count * sizeof(int32_t));

	// Per-octant precomputed tables: linear index delta, step multiplier, and
	// (for diagonals) the two orthogonal side deltas for the corner-cut check.
	int32_t oct_off[8];
	uint32_t oct_step[8];
	int32_t oct_side_a[8];
	int32_t oct_side_b[8];
	for (int o = 0; o < 8; o++) {
		oct_off[o] = OCT_DY[o] * w + OCT_DX[o];
		oct_step[o] = OCT_DIAG[o] ? STEP_DIAG : STEP_ORTHO;
		oct_side_a[o] = OCT_DX[o];
		oct_side_b[o] = OCT_DY[o] * w;
	}

	// Dial's bucket queue. Every edge weight is at most MAX_CELL_COST *
	// STEP_DIAG, so at any moment all pending keys live in a window of
	// BUCKET_COUNT consecutive values and a circular bucket array indexed by
	// (key % BUCKET_COUNT) is unambiguous. Push and pop are O(1) -- no
	// comparison-heap sifts in the inner loop -- and the whole structure is
	// three flat arrays (bucket heads + an append-only node pool), reused
	// across recomputes.
	//
	// Determinism: the algorithm is strictly sequential; buckets are LIFO, so
	// the visit order is a pure function of the input. Combined with strict
	// improvement (nc < integ[v]) the output field is bit-identical for
	// identical grid+costs+goals.
	//
	// Each planar edge pushes at most once (pushes require strict improvement
	// and each cell settles exactly once), so 8 * cell_count + links + goals
	// bounds the node pool and it never reallocates mid-loop. Raw pointers
	// matter here: LocalVector's checked operator[] and disabled inlining are
	// a ~2x dev_build penalty.
	constexpr uint32_t BUCKET_COUNT = MAX_CELL_COST * STEP_DIAG + 1;
	if (job.bucket_head.size() < BUCKET_COUNT) {
		job.bucket_head.resize(BUCKET_COUNT);
	}
	const uint32_t node_capacity = (uint32_t)cell_count * 8 + job.links.targets.size() + job.goals.size() + 1;
	if (job.node_key_cell.size() < node_capacity) {
		job.node_key_cell.resize(node_capacity); // Trivial types: uninitialized.
		job.node_next.resize(node_capacity);
	}
	int32_t *bhead = job.bucket_head.ptr();
	uint64_t *nkey = job.node_key_cell.ptr();
	int32_t *nnext = job.node_next.ptr();
	memset(bhead, 0xFF, BUCKET_COUNT * sizeof(int32_t)); // All heads = -1.
	uint32_t node_count = 0;
	uint32_t pending = 0;

	for (uint32_t g = 0; g < job.goals.size(); g++) {
		const int32_t src = job.goals[g];
		if (integ[src] == 0) {
			continue; // Duplicate goal cell.
		}
		integ[src] = 0;
		gid[src] = (uint16_t)g;
		nkey[node_count] = (uint64_t)(uint32_t)src; // Key 0 -> bucket 0.
		nnext[node_count] = bhead[0];
		bhead[0] = (int32_t)node_count;
		node_count++;
		pending++;
	}

	const LinkCSR &csr = job.links;
	const int32_t *link_off = csr.offsets.ptr();
	const int32_t *link_tgt = csr.targets.ptr();
	const uint32_t *link_cost = csr.costs.ptr();
	const bool has_links = csr.targets.size() > 0;

	uint32_t cur_bucket = 0;
	while (pending > 0) {
		int32_t ni = bhead[cur_bucket];
		while (ni < 0) {
			cur_bucket++;
			if (cur_bucket == BUCKET_COUNT) {
				cur_bucket = 0;
			}
			ni = bhead[cur_bucket];
		}
		bhead[cur_bucket] = nnext[ni];
		pending--;

		const uint64_t top = nkey[ni];
		const uint32_t u = (uint32_t)(top & 0xFFFFFFFFu);
		const uint32_t ucost = (uint32_t)(top >> 32);

		if (ucost > integ[u]) {
			continue; // Stale entry (superseded by a cheaper push).
		}

		const int32_t f = (int32_t)u / layer;
		const int32_t rem = (int32_t)u - f * layer;
		const int32_t ux = rem % w;
		const int32_t uy = rem / w;
		const uint16_t ugid = gid[u];

		// Border masks: which octants stay in-bounds from (ux, uy).
		const bool ok_e = ux + 1 < w;
		const bool ok_w2 = ux > 0;
		const bool ok_n = uy + 1 < h;
		const bool ok_s = uy > 0;
		const bool oct_ok[8] = { ok_e, ok_e && ok_n, ok_n, ok_w2 && ok_n, ok_w2, ok_w2 && ok_s, ok_s, ok_e && ok_s };

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
				if (cost[u + oct_side_a[o]] == 0 || cost[u + oct_side_b[o]] == 0) {
					continue;
				}
			}
			const uint32_t nc = ucost + vcost * oct_step[o];
			if (nc < integ[v]) {
				integ[v] = nc;
				gid[v] = ugid;
				const uint32_t b = nc % BUCKET_COUNT;
				nkey[node_count] = ((uint64_t)nc << 32) | (uint32_t)v;
				nnext[node_count] = bhead[b];
				bhead[b] = (int32_t)node_count;
				node_count++;
				pending++;
			}
		}

		// Inter-floor links.
		if (has_links) {
			const int32_t begin = link_off[u];
			const int32_t end = link_off[u + 1];
			for (int32_t k = begin; k < end; k++) {
				const int32_t v = link_tgt[k];
				if (cost[v] == 0) {
					continue;
				}
				const uint32_t nc = ucost + link_cost[k];
				if (nc < integ[v]) {
					integ[v] = nc;
					gid[v] = ugid;
					const uint32_t b = nc % BUCKET_COUNT;
					nkey[node_count] = ((uint64_t)nc << 32) | (uint32_t)v;
					nnext[node_count] = bhead[b];
					bhead[b] = (int32_t)node_count;
					node_count++;
					pending++;
				}
			}
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
					dir[u] = OCTANT_NONE;
					continue;
				}
				if (integ[u] == 0) {
					dir[u] = OCTANT_GOAL;
					continue;
				}
				const bool ok_e = ux + 1 < w;
				const bool ok_w2 = ux > 0;
				const bool oct_ok[8] = { ok_e, ok_e && ok_n, ok_n, ok_w2 && ok_n, ok_w2, ok_w2 && ok_s, ok_s, ok_e && ok_s };

				uint32_t best = integ[u];
				int best_oct = -1;
				int32_t best_link = -1;
				for (int o = 0; o < 8; o++) {
					if (!oct_ok[o]) {
						continue;
					}
					const int32_t v = u + oct_off[o];
					if (cost[v] == 0 || integ[v] >= best) {
						continue;
					}
					if (OCT_DIAG[o]) {
						if (cost[u + oct_side_a[o]] == 0 || cost[u + oct_side_b[o]] == 0) {
							continue;
						}
					}
					best = integ[v];
					best_oct = o;
				}
				if (has_links) {
					const int32_t begin = link_off[u];
					const int32_t end = link_off[u + 1];
					for (int32_t k = begin; k < end; k++) {
						const int32_t v = link_tgt[k];
						if (cost[v] == 0 || integ[v] >= best) {
							continue;
						}
						best = integ[v];
						best_oct = -1;
						best_link = v;
					}
				}

				if (best_link >= 0) {
					dir[u] = OCTANT_LINK;
					lstep[u] = best_link;
				} else if (best_oct >= 0) {
					dir[u] = (uint8_t)best_oct;
				} else {
					dir[u] = OCTANT_NONE; // Local minimum with no descending neighbor.
				}
			}
		}
	}

	last_compute_usec = OS::get_singleton()->get_ticks_usec() - t0;

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

bool HordeFlowField::poll() {
	if (!in_flight.is_set()) {
		return false;
	}
	if (!WorkerThreadPool::get_singleton()->is_task_completed(task_id)) {
		return false;
	}
	WorkerThreadPool::get_singleton()->wait_for_task_completion(task_id);
	in_flight.clear();
	task_id = WorkerThreadPool::INVALID_TASK_ID;
	published = true;

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
		WorkerThreadPool::get_singleton()->wait_for_task_completion(task_id);
		in_flight.clear();
		task_id = WorkerThreadPool::INVALID_TASK_ID;
		published = true;
	}
}

Vector2 HordeFlowField::octant_to_vector(int32_t p_octant) {
	static const float INV_SQRT2 = 0.7071067811865476f;
	switch (p_octant) {
		case OCTANT_E:
			return Vector2(1, 0);
		case OCTANT_NE:
			return Vector2(INV_SQRT2, INV_SQRT2);
		case OCTANT_N:
			return Vector2(0, 1);
		case OCTANT_NW:
			return Vector2(-INV_SQRT2, INV_SQRT2);
		case OCTANT_W:
			return Vector2(-1, 0);
		case OCTANT_SW:
			return Vector2(-INV_SQRT2, -INV_SQRT2);
		case OCTANT_S:
			return Vector2(0, -1);
		case OCTANT_SE:
			return Vector2(INV_SQRT2, -INV_SQRT2);
		default:
			return Vector2(0, 0);
	}
}

Vector2 HordeFlowField::direction_at_index(int32_t p_index) const {
	if (!published || p_index < 0 || p_index >= cell_count) {
		return Vector2(0, 0);
	}
	return octant_to_vector(_front().dir_octant[p_index]);
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
	return _front().dir_octant[p_index];
}

int32_t HordeFlowField::cost_at_index(int32_t p_index) const {
	if (!published || p_index < 0 || p_index >= cell_count) {
		return -1;
	}
	const uint32_t c = _front().integration[p_index];
	return (c == COST_UNREACHABLE) ? -1 : (int32_t)c;
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
	const uint16_t g = _front().goal_id[grid->cell_index(p_x, p_y, p_floor)];
	return (g == GOAL_NONE) ? -1 : (int32_t)g;
}

int32_t HordeFlowField::link_target_at_index(int32_t p_index) const {
	if (!published || p_index < 0 || p_index >= cell_count) {
		return -1;
	}
	return _front().link_step[p_index];
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
}
