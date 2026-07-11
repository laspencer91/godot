/**************************************************************************/
/*  test_horde_flow_field.h                                              */
/**************************************************************************/

#pragma once

#include "horde_test_helpers.h"

#include <atomic>

namespace TestHordeSim {

// ---------------------------------------------------------------------------
// 1. Straight-path field correctness.
// ---------------------------------------------------------------------------
TEST_CASE("[HordeSim][FlowField] Straight-line field points at the goal with monotone cost") {
	Ref<HordeNavGrid> grid = make_grid(20, 1);

	Ref<HordeFlowField> field;
	field.instantiate();
	field->set_grid(grid);
	field->add_goal(0, 0, 0);
	field->recompute_sync();

	CHECK(field->has_field());
	CHECK(field->cost_at(0, 0, 0) == 0);

	// Ortho step cost is base_cost(1) * STEP_ORTHO(10) = 10 per cell.
	for (int x = 0; x < 20; x++) {
		CHECK(field->cost_at(x, 0, 0) == x * 10);
	}

	// Every non-goal cell points due West (toward the goal at x = 0).
	for (int x = 1; x < 20; x++) {
		CHECK(field->direction_at(x, 0, 0) == Vector2(-1, 0));
	}
	CHECK(field->goal_at(10, 0, 0) == 0);
}

// ---------------------------------------------------------------------------
// 2. Wall + door detour (door is a dynamic blocked flag in the cost layer).
// ---------------------------------------------------------------------------
TEST_CASE("[HordeSim][FlowField] Wall forces a detour; closing the door severs the path") {
	Ref<HordeNavGrid> grid = make_grid(5, 5);
	// Vertical wall at column x = 2 for rows y = 1..4; row y = 0 is the doorway.
	for (int y = 1; y < 5; y++) {
		grid->set_walkable(2, y, 0, false);
	}

	Ref<HordeFlowField> field;
	field.instantiate();
	field->set_grid(grid);
	field->add_goal(0, 2, 0); // Goal on the far side of the wall.

	SUBCASE("door open -> reachable via detour") {
		field->recompute_sync();
		CHECK(field->is_reachable(4, 2, 0));
		// Detour is strictly longer than the blocked straight line (4 * 10 = 40).
		CHECK(field->cost_at(4, 2, 0) > 40);
	}

	SUBCASE("door closed (blocked) -> unreachable, recompute re-routes") {
		field->recompute_sync();
		CHECK(field->is_reachable(4, 2, 0));

		grid->set_blocked(2, 0, 0, true); // Close the only door.
		field->recompute_sync();
		CHECK_FALSE(field->is_reachable(4, 2, 0));
		CHECK(field->cost_at(4, 2, 0) == -1);
		CHECK(field->octant_at_index(grid->cell_index(4, 2, 0)) == HordeFlowField::OCTANT_NONE);
	}
}

// ---------------------------------------------------------------------------
// 3. Cost-avoidance re-routing (cheap corridor vs costly shortcut).
// ---------------------------------------------------------------------------
TEST_CASE("[HordeSim][FlowField] Field avoids a costly corridor and re-routes when cost clears") {
	Ref<HordeNavGrid> grid = make_grid(7, 3);
	Ref<HordeFlowField> field;
	field.instantiate();
	field->set_grid(grid);
	field->add_goal(0, 1, 0);

	// Baseline: straight shot along the middle row costs 6 * 10 = 60.
	field->recompute_sync();
	CHECK(field->cost_at(6, 1, 0) == 60);

	// Make the middle row expensive; cheap detour rows y=0 / y=2 remain.
	for (int x = 1; x <= 5; x++) {
		grid->set_dynamic_cost(x, 1, 0, 100);
	}
	field->recompute_sync();

	// Cost stays close to the cheap detour, nowhere near the ~5000 straight cost.
	const int detour_cost = field->cost_at(6, 1, 0);
	CHECK(detour_cost > 60);
	CHECK(detour_cost < 200);

	// A cell deep in the expensive row escapes it vertically (non-zero Y step).
	const Vector2 mid_dir = field->direction_at(3, 1, 0);
	CHECK(mid_dir.y != 0.0f);

	// Clearing the cost re-routes back to the straight-line optimum.
	for (int x = 1; x <= 5; x++) {
		grid->set_dynamic_cost(x, 1, 0, 0);
	}
	field->recompute_sync();
	CHECK(field->cost_at(6, 1, 0) == 60);
}

// ---------------------------------------------------------------------------
// 4. Multi-goal nearest-goal assignment.
// ---------------------------------------------------------------------------
TEST_CASE("[HordeSim][FlowField] Multiple goals: each cell routes to its nearest goal") {
	Ref<HordeNavGrid> grid = make_grid(11, 1);
	Ref<HordeFlowField> field;
	field.instantiate();
	field->set_grid(grid);
	field->add_goal(0, 0, 0); // goal id 0
	field->add_goal(10, 0, 0); // goal id 1
	field->recompute_sync();

	CHECK(field->cost_at(0, 0, 0) == 0);
	CHECK(field->cost_at(10, 0, 0) == 0);

	CHECK(field->goal_at(3, 0, 0) == 0); // Closer to left goal.
	CHECK(field->goal_at(7, 0, 0) == 1); // Closer to right goal.
	CHECK(field->cost_at(3, 0, 0) == 30);
	CHECK(field->cost_at(7, 0, 0) == 30);

	// Cell 3 flows West toward goal 0; cell 7 flows East toward goal 1.
	CHECK(field->direction_at(3, 0, 0) == Vector2(-1, 0));
	CHECK(field->direction_at(7, 0, 0) == Vector2(1, 0));
}

// ---------------------------------------------------------------------------
// 5. Unreachable-cell flagging.
// ---------------------------------------------------------------------------
TEST_CASE("[HordeSim][FlowField] Sealed-off cells are flagged unreachable") {
	Ref<HordeNavGrid> grid = make_grid(5, 5);
	// Full wall at x = 2, no door: right side is sealed from the goal.
	for (int y = 0; y < 5; y++) {
		grid->set_walkable(2, y, 0, false);
	}

	Ref<HordeFlowField> field;
	field.instantiate();
	field->set_grid(grid);
	field->add_goal(0, 2, 0);
	field->recompute_sync();

	CHECK(field->is_reachable(1, 2, 0)); // Same side as goal.
	CHECK_FALSE(field->is_reachable(4, 2, 0)); // Sealed side.
	CHECK(field->cost_at(4, 2, 0) == -1);
	CHECK(field->direction_at(4, 2, 0) == Vector2(0, 0));
	CHECK(field->goal_at(4, 2, 0) == -1);
}

// ---------------------------------------------------------------------------
// 6. Determinism: identical inputs -> bit-identical field.
// ---------------------------------------------------------------------------
TEST_CASE("[HordeSim][FlowField] Identical grid+costs+goals produce a bit-identical field") {
	auto build = []() -> Ref<HordeNavGrid> {
		Ref<HordeNavGrid> g = make_grid(48, 48);
		// A repeatable mix of walls and cost bands.
		for (int y = 5; y < 40; y++) {
			g->set_walkable(12, y, 0, false);
			g->set_walkable(30, y, 0, false);
		}
		g->set_walkable(12, 20, 0, true); // doorway
		g->set_walkable(30, 25, 0, true); // doorway
		for (int x = 0; x < 48; x++) {
			g->set_dynamic_cost(x, 24, 0, 40);
		}
		return g;
	};

	Ref<HordeNavGrid> grid_a = build();
	Ref<HordeNavGrid> grid_b = build();

	Ref<HordeFlowField> field_a;
	field_a.instantiate();
	field_a->set_grid(grid_a);
	field_a->add_goal(2, 24, 0);
	field_a->add_goal(45, 3, 0);
	field_a->recompute_sync();

	Ref<HordeFlowField> field_b;
	field_b.instantiate();
	field_b->set_grid(grid_b);
	field_b->add_goal(2, 24, 0);
	field_b->add_goal(45, 3, 0);
	field_b->recompute_sync();

	bool identical = true;
	const int count = grid_a->get_cell_count();
	for (int i = 0; i < count; i++) {
		if (field_a->cost_at_index(i) != field_b->cost_at_index(i) ||
				field_a->octant_at_index(i) != field_b->octant_at_index(i)) {
			identical = false;
			break;
		}
	}
	CHECK(identical);

	// A second recompute of the same field also reproduces the result.
	field_a->recompute_sync();
	bool stable = true;
	for (int i = 0; i < count; i++) {
		if (field_a->cost_at_index(i) != field_b->cost_at_index(i) ||
				field_a->octant_at_index(i) != field_b->octant_at_index(i)) {
			stable = false;
			break;
		}
	}
	CHECK(stable);
}

// ---------------------------------------------------------------------------
// 7. Double-buffer coherence under threaded recompute.
// ---------------------------------------------------------------------------
TEST_CASE("[HordeSim][FlowField] Readers never observe a torn or incomplete field") {
	// Fully-open grid: every cell is reachable in every published field, so a
	// reader that ever samples an unreachable/none cell must have observed a
	// partially-computed back buffer (a torn read) -> failure.
	Ref<HordeNavGrid> grid = make_grid(96, 96);
	const int count = grid->get_cell_count();

	Ref<HordeFlowField> field;
	field.instantiate();
	field->set_grid(grid);
	field->add_goal(0, 0, 0);
	field->recompute_sync(); // Publish an initial complete field before readers start.

	std::atomic<bool> stop{ false };
	std::atomic<bool> torn{ false };
	std::atomic<uint64_t> samples{ 0 };

	struct ReaderCtx {
		HordeFlowField *field = nullptr;
		int count = 0;
		std::atomic<bool> *stop = nullptr;
		std::atomic<bool> *torn = nullptr;
		std::atomic<uint64_t> *samples = nullptr;
	};
	ReaderCtx ctx{ field.ptr(), count, &stop, &torn, &samples };

	auto reader = [](void *p_userdata) {
		const ReaderCtx &c = *static_cast<ReaderCtx *>(p_userdata);
		uint64_t local = 0;
		while (!c.stop->load(std::memory_order_relaxed)) {
			for (int i = 0; i < c.count; i++) {
				const int cost = c.field->cost_at_index(i);
				const int oct = c.field->octant_at_index(i);
				if (cost < 0 || oct == HordeFlowField::OCTANT_NONE) {
					c.torn->store(true, std::memory_order_relaxed);
				}
				local++;
			}
		}
		c.samples->fetch_add(local, std::memory_order_relaxed);
	};

	Thread readers[3];
	for (Thread &r : readers) {
		r.start(reader, &ctx);
	}

	// Hammer recomputes, alternating the goal corner so the back buffer content
	// genuinely changes between generations.
	for (int k = 0; k < 400; k++) {
		field->clear_goals();
		if (k & 1) {
			field->add_goal(95, 95, 0);
		} else {
			field->add_goal(0, 0, 0);
		}
		field->recompute_sync();
	}

	stop.store(true, std::memory_order_relaxed);
	for (Thread &r : readers) {
		r.wait_to_finish();
	}

	CHECK_FALSE(torn.load());
	CHECK(samples.load() > 0);
	print_line(vformat("[HordeSim] coherence reader samples: %d", (int64_t)samples.load()));
}

// ---------------------------------------------------------------------------
// 7b. Deterministic straddle: a sample paused inside the race window while a
// publish lands AND a second recompute rewrites the snapshotted buffer. The
// interleaved test seam runs the same seqlock protocol as the production
// accessors, so this catches the torn-read class deterministically (the
// threaded case above only hits the nanosecond window under load).
// ---------------------------------------------------------------------------
TEST_CASE("[HordeSim][FlowField] A sample straddling two recomputes never tears") {
	// 256x256 keeps a recompute in the multi-ms range so the hooked sample's
	// pending data read reliably lands while the worker is mid-write.
	Ref<HordeNavGrid> grid = make_grid(256, 256);
	Ref<HordeFlowField> field;
	field.instantiate();
	field->set_grid(grid);
	field->add_goal(0, 0, 0);
	field->recompute_sync();

	// Probe the far corner: with goals near the opposite corner it is settled
	// last by Dijkstra and written last by the direction pass, so mid-compute
	// the buffer still holds the 0xFF clear pattern there (COST_UNREACHABLE /
	// OCTANT_NONE) -- exactly the torn value the seqlock must never return.
	const int probe = grid->cell_index(255, 255, 0);

	struct HookCtx {
		HordeFlowField *field = nullptr;
		bool fired = false;
	};
	auto hook = [](void *p_userdata) {
		HookCtx &ctx = *static_cast<HookCtx *>(p_userdata);
		if (ctx.fired) {
			return; // Widen the window once; retries must converge unhindered.
		}
		ctx.fired = true;
		// Publish #1: the caller's snapshotted front becomes the back buffer.
		ctx.field->clear_goals();
		ctx.field->add_goal(10, 10, 0);
		ctx.field->recompute_sync();
		// Recompute #2 in flight: the worker starts clearing/rewriting the
		// snapshotted buffer while the caller's data read is still pending.
		ctx.field->clear_goals();
		ctx.field->add_goal(20, 20, 0);
		ctx.field->request_recompute();
		// Land the pending read mid-write (the clear pattern goes down first;
		// the probe cell's real value arrives near the end of the compute).
		OS::get_singleton()->delay_usec(1500);
	};

	for (int iter = 0; iter < 20; iter++) {
		HookCtx ctx;
		ctx.field = field.ptr();
		if (iter & 1) {
			const int oct = field->octant_at_index_interleaved(probe, hook, &ctx);
			CHECK(oct != HordeFlowField::OCTANT_NONE); // Open grid: never unreachable.
		} else {
			const int cost = field->cost_at_index_interleaved(probe, hook, &ctx);
			CHECK(cost >= 0); // Open grid: every published field reaches every cell.
		}
		CHECK(ctx.fired);
		// Drain the in-flight recompute, then restore the baseline field.
		while (field->is_recomputing()) {
			field->poll();
		}
		field->clear_goals();
		field->add_goal(0, 0, 0);
		field->recompute_sync();
	}
}

// ---------------------------------------------------------------------------
// 8. Recompute budget on a 256x256 authored grid.
// ---------------------------------------------------------------------------
TEST_CASE("[HordeSim][FlowField] 256x256 recompute stays within the dev budget") {
	Ref<HordeNavGrid> grid = make_grid(256, 256);
	// Author some structure so this isn't a trivially-empty field: a few walls
	// with doorways plus a diagonal cost band.
	for (int y = 0; y < 220; y++) {
		grid->set_walkable(80, y, 0, false);
	}
	grid->set_walkable(80, 110, 0, true); // doorway
	for (int y = 40; y < 256; y++) {
		grid->set_walkable(170, y, 0, false);
	}
	grid->set_walkable(170, 130, 0, true); // doorway
	for (int d = 0; d < 256; d++) {
		grid->set_dynamic_cost(d, d, 0, 30);
	}

	Ref<HordeFlowField> field;
	field.instantiate();
	field->set_grid(grid);
	field->add_goal(10, 10, 0);
	field->add_goal(250, 250, 0);

	// Warm once, then take the best of three recomputes: the metric is the cost
	// of the recompute itself, not scheduler noise from a loaded test machine.
	field->recompute_sync();
	uint64_t usec = UINT64_MAX;
	for (int i = 0; i < 3; i++) {
		field->recompute_sync();
		usec = MIN(usec, field->get_last_compute_usec());
	}
	print_line(vformat("[HordeSim] 256x256 flow-field recompute: %d us (%.3f ms)", (int64_t)usec, usec / 1000.0));

	CHECK(field->has_field());
	CHECK(field->is_reachable(128, 128, 0));
	// Dev-build budget gate (real shipping budget is gated at Gate 1 on release).
	CHECK(usec < 10000);
}

// ---------------------------------------------------------------------------
// 9. Inter-floor links (the grid's floor-level feature).
// ---------------------------------------------------------------------------
TEST_CASE("[HordeSim][FlowField] Inter-floor link makes an upstairs goal reachable") {
	Ref<HordeNavGrid> grid = make_grid(4, 4, 2);
	Ref<HordeFlowField> field;
	field.instantiate();
	field->set_grid(grid);
	field->add_goal(0, 0, 1); // Goal is on floor 1.

	SUBCASE("no link -> floor 0 cannot reach floor 1") {
		field->recompute_sync();
		CHECK(field->is_reachable(0, 0, 1));
		CHECK_FALSE(field->is_reachable(3, 3, 0));
	}

	SUBCASE("ladder link -> floor 0 reaches the upstairs goal") {
		grid->link_cells(Vector3i(0, 0, 0), Vector3i(0, 0, 1), 1);
		field->recompute_sync();
		CHECK(field->is_reachable(3, 3, 0));
		// The cell at the base of the ladder steps up through the link.
		CHECK(field->octant_at_index(grid->cell_index(0, 0, 0)) == HordeFlowField::OCTANT_LINK);
		CHECK(field->link_target_at_index(grid->cell_index(0, 0, 0)) == grid->cell_index(0, 0, 1));
	}
}

} // namespace TestHordeSim
