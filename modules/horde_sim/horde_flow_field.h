/**************************************************************************/
/*  horde_flow_field.h                                                     */
/**************************************************************************/

#pragma once

#include "horde_nav_grid.h"

#include "core/object/ref_counted.h"
#include "core/object/worker_thread_pool.h"
#include "core/templates/local_vector.h"
#include "core/templates/safe_refcount.h"
#include "core/variant/type_info.h"

#include <atomic>

// Multi-goal integration + direction field over a HordeNavGrid (DES G6.3).
//
// Dijkstra from all goals at once (8-connected, cost-aware, no corner cutting)
// produces, per cell:
//   - integration cost to the nearest goal,
//   - a packed octant direction toward the descent neighbor,
//   - the id of the nearest goal (nearest-goal assignment).
// Agent sampling is O(1): direction_at() / cost_at() / goal_at().
//
// THREADING / DOUBLE-BUFFER CONTRACT
// ----------------------------------
// Two field buffers exist. A recompute runs as a WorkerThreadPool task that
// writes ONLY the back buffer (1 - front). On completion the worker publishes
// the result with a single atomic release-store of `front_index`; readers do an
// acquire-load of `front_index` before reading. The release/acquire pair
// guarantees that once a reader observes the new index, every write to that
// buffer is visible -- so a reader never observes a partially-computed field.
//
// At most one recompute is in flight (single writer). The front buffer is never
// mutated while it is the front. request/poll debounce many cost-layer edits in
// a tick into one recompute.
//
// The release/acquire pair alone leaves one window: a reader that loaded
// `front_index` and is still reading that buffer when a SUBSEQUENT recompute
// reclaims it as the back buffer and starts clearing it observes torn data. A
// per-buffer seqlock closes it completely: the worker makes the back buffer's
// sequence odd before its first write and even again before publishing;
// readers snapshot (front_index, sequence), read, and recheck the sequence,
// retrying on any change. A validated sample is proof the buffer was stable
// for the whole read, no matter how slow the reader. Hot-path cost per sample:
// two extra acquire loads + a predictable branch (the fence is free on
// x86/TSO); no RMW, no lock, no allocation. Retries need a full publish DURING
// the nanosecond-scale sample, so they are vanishingly rare and re-converge on
// the freshly published front immediately.
//
// Callers must drive the lifecycle from the sim thread: request_recompute() to
// schedule, poll() each tick to publish a finished field and re-dispatch if the
// grid changed again. recompute_sync() is a blocking convenience for tests.
// The grid must not be edited while a recompute is in flight (poll() must
// report idle first); the worker reads a snapshot of the grid taken at dispatch,
// so edits after dispatch are safe but only apply to the next recompute.
class HordeFlowField : public RefCounted {
	GDCLASS(HordeFlowField, RefCounted);

public:
	// Packed direction octant values stored per cell (fits a uint8_t).
	enum Octant {
		OCTANT_E = 0,
		OCTANT_NE = 1,
		OCTANT_N = 2,
		OCTANT_NW = 3,
		OCTANT_W = 4,
		OCTANT_SW = 5,
		OCTANT_S = 6,
		OCTANT_SE = 7,
		OCTANT_GOAL = 250, // Cell is (or coincides with) a goal.
		OCTANT_LINK = 251, // Next step is an inter-floor link, not planar.
		OCTANT_NONE = 255, // Unreachable / impassable.
	};

	static constexpr uint32_t COST_UNREACHABLE = 0xFFFFFFFFu;
	static constexpr uint16_t GOAL_NONE = 0xFFFFu;

private:
	struct FieldBuffer {
		LocalVector<uint32_t> integration;
		LocalVector<uint8_t> dir_octant;
		LocalVector<uint16_t> goal_id;
		LocalVector<int32_t> link_step; // Target cell when dir_octant == OCTANT_LINK, else -1.

		void resize(uint32_t p_count) {
			integration.resize(p_count);
			dir_octant.resize(p_count);
			goal_id.resize(p_count);
			link_step.resize(p_count);
		}
	};

	// CSR of inter-floor links, keyed by source cell, rebuilt from the grid at
	// dispatch. Keeps the hashmap-free planar path fast; links folded in Dijkstra.
	struct LinkCSR {
		LocalVector<int32_t> offsets; // size cell_count + 1
		LocalVector<int32_t> targets;
		LocalVector<uint32_t> costs;
	};

	// Immutable-per-recompute snapshot the worker reads (decoupled from grid
	// edits after dispatch).
	struct Job {
		int32_t width = 0;
		int32_t height = 0;
		int32_t floors = 0;
		int32_t back = 0; // Buffer index being written.
		LocalVector<uint32_t> cost; // Per-cell traversal cost; 0 == impassable.
		LocalVector<int32_t> goals;
		LinkCSR links;
		LocalVector<int32_t> link_cursor; // CSR-build scratch.
		// Reused scratch for the Dial's bucket queue (flat arrays, no per-cell
		// heap objects): circular bucket heads + append-only node pool.
		LocalVector<int32_t> bucket_head;
		LocalVector<uint64_t> node_key_cell; // Packed (cost << 32) | cell.
		LocalVector<int32_t> node_next;
	};

	Ref<HordeNavGrid> grid;
	LocalVector<int32_t> goals; // Goal cell indices (pending, main-thread owned).

	FieldBuffer buffers[2];
	SafeNumeric<int32_t> front_index{ 0 };
	// Per-buffer seqlock (see the threading contract above): odd while the
	// worker is writing that buffer, even while it is stable.
	SafeNumeric<uint32_t> buffer_seq[2];
	bool published = false; // A completed field exists in front buffer.

	Job job; // Owns worker-visible snapshot + scratch; reused across recomputes.
	WorkerThreadPool::TaskID task_id = WorkerThreadPool::INVALID_TASK_ID;
	SafeFlag in_flight;
	bool dirty = false; // A recompute is pending (debounced).

	uint64_t last_compute_usec = 0;

	int32_t cell_count = 0;

	static void _bind_methods();

	void _dispatch();
	void _finish(); // Retire the completed worker task and mark the field published.
	void _snapshot_grid(); // Main thread: fill job.cost + job.links from grid.
	static void _worker_entry(void *p_userdata);
	void _compute(); // Runs on the worker: Dijkstra + direction pass into back.

	// Coherent single-cell sample (seqlock reader). `p_sample` receives the
	// snapshotted buffer; `p_hook` runs between the snapshot and the data read
	// so tests can widen the nanosecond race window deterministically --
	// production callers pass an empty lambda, which inlines away.
	template <typename Sample, typename Hook>
	_FORCE_INLINE_ auto _coherent_sample(Sample &&p_sample, Hook &&p_hook) const {
		while (true) {
			const int32_t idx = front_index.get(); // Acquire.
			const uint32_t s1 = buffer_seq[idx].get(); // Acquire.
			if (unlikely((s1 & 1u) != 0u)) {
				continue; // Snapshot raced a writer claiming this buffer; reload.
			}
			p_hook();
			const auto value = p_sample(buffers[idx]);
			// Order the data read before the recheck (no-op fence on x86/TSO).
			std::atomic_thread_fence(std::memory_order_acquire);
			if (likely(buffer_seq[idx].get() == s1)) {
				return value;
			}
			// A recompute published and a second one began rewriting the
			// snapshotted buffer mid-read; retry against the new front.
		}
	}

public:
	void set_grid(const Ref<HordeNavGrid> &p_grid);
	Ref<HordeNavGrid> get_grid() const { return grid; }

	// Goals (nearest-goal assignment across all of them).
	void clear_goals();
	void add_goal(int32_t p_x, int32_t p_y, int32_t p_floor);
	void add_goal_index(int32_t p_index);
	void set_goals(const PackedInt32Array &p_indices);
	int32_t get_goal_count() const { return goals.size(); }

	// Lifecycle.
	void request_recompute(); // Debounced async; dispatches if idle.
	bool poll(); // Publish finished field + re-dispatch if dirty. True if newly published.
	void recompute_sync(); // Dispatch (if needed) and block until published.
	bool is_recomputing() const { return in_flight.is_set(); }
	bool has_field() const { return published; }

	// O(1) sampling (agent access pattern).
	Vector2 direction_at(int32_t p_x, int32_t p_y, int32_t p_floor) const;
	Vector2 direction_at_index(int32_t p_index) const;
	int32_t octant_at_index(int32_t p_index) const;
	int32_t cost_at(int32_t p_x, int32_t p_y, int32_t p_floor) const;
	int32_t cost_at_index(int32_t p_index) const;
	bool is_reachable(int32_t p_x, int32_t p_y, int32_t p_floor) const;
	int32_t goal_at(int32_t p_x, int32_t p_y, int32_t p_floor) const; // Nearest goal id, -1 unreachable.
	int32_t link_target_at_index(int32_t p_index) const;

	// TEST SEAM (native-only, not bound): identical to cost_at_index /
	// octant_at_index but runs p_hook between the coherence snapshot and the
	// data read, letting a test force publishes + a back-buffer rewrite inside
	// the race window. Goes through the same _coherent_sample protocol as the
	// production accessors, so a protocol regression fails the straddle test.
	int32_t cost_at_index_interleaved(int32_t p_index, void (*p_hook)(void *), void *p_hook_userdata) const;
	int32_t octant_at_index_interleaved(int32_t p_index, void (*p_hook)(void *), void *p_hook_userdata) const;

	uint64_t get_last_compute_usec() const { return last_compute_usec; }

	// Direction lookup table (octant -> unit-ish planar vector). Public for tests.
	static Vector2 octant_to_vector(int32_t p_octant);

	HordeFlowField() {}
	~HordeFlowField();
};

VARIANT_ENUM_CAST(HordeFlowField::Octant);
