/**************************************************************************/
/*  test_horde_sim.h                                                      */
/**************************************************************************/

#pragma once

#include "../horde_agents.h"
#include "../horde_flow_field.h"
#include "../horde_fsm_config.h"
#include "../horde_nav_grid.h"

#include "core/os/os.h"
#include "core/os/thread.h"
#include "core/string/print_string.h"

#include "tests/test_macros.h"

// Full-path includes (see box3d_ragdoll.h): the tests library env does not carry
// the Box3D include dir, but the project root is always on the include path.
#include <thirdparty/box3d/include/box3d/box3d.h>
#include <thirdparty/box3d/include/box3d/collision.h>
#include <thirdparty/box3d/include/box3d/id.h>

#include <atomic>

namespace TestHordeSim {

static Ref<HordeNavGrid> make_grid(int w, int h, int floors = 1) {
	Ref<HordeNavGrid> grid;
	grid.instantiate();
	grid->resize(w, h, floors);
	grid->configure(HordeNavGrid::DEFAULT_CELL_SIZE, Vector2(), HordeNavGrid::DEFAULT_FLOOR_HEIGHT);
	return grid;
}

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

// ---------------------------------------------------------------------------
// Grid transform sanity.
// ---------------------------------------------------------------------------
TEST_CASE("[HordeSim][NavGrid] World<->cell transforms round-trip") {
	Ref<HordeNavGrid> grid = make_grid(10, 10, 3);
	grid->configure(0.5f, Vector2(-2.0f, -3.0f), 3.0f);

	const Vector3 w = grid->cell_to_world(4, 6, 2);
	const Vector3i c = grid->world_to_cell(w);
	CHECK(c == Vector3i(4, 6, 2));

	// Cell center of (0,0,0) sits half a cell in from the origin corner.
	const Vector3 c0 = grid->cell_to_world(0, 0, 0);
	CHECK(c0.x == doctest::Approx(-2.0f + 0.25f));
	CHECK(c0.z == doctest::Approx(-3.0f + 0.25f));
	CHECK(c0.y == doctest::Approx(0.0f));
}

// ===========================================================================
// HordeAgents (P1.B2) — SoA agent sim, FSM framework, LOD, kinematic movement.
// ===========================================================================

// A self-contained Box3D world with static wall boxes, for the native mover
// sweep tests. Owns its hulls (kept alive for the world's lifetime) and world.
struct TestWorld {
	b3WorldId world = b3_nullWorldId;
	LocalVector<b3BoxHull> hulls;

	TestWorld() {
		b3WorldDef wd = b3DefaultWorldDef();
		world = b3CreateWorld(&wd);
	}
	~TestWorld() {
		if (B3_IS_NON_NULL(world)) {
			b3DestroyWorld(world);
		}
	}
	// Axis-aligned static box centered at (cx,cy,cz) with half extents (hx,hy,hz).
	void add_wall(float cx, float cy, float cz, float hx, float hy, float hz) {
		b3BodyDef bd = b3DefaultBodyDef();
		bd.type = b3_staticBody;
		bd.position = b3Vec3{ cx, cy, cz };
		b3BodyId body = b3CreateBody(world, &bd);
		hulls.push_back(b3MakeBoxHull(hx, hy, hz));
		b3ShapeDef sd = b3DefaultShapeDef();
		b3CreateHullShape(body, &sd, &hulls[hulls.size() - 1].base);
	}
	void finalize() {
		// Populate the broadphase so queries see the static geometry.
		b3World_Step(world, 1.0f / 128.0f, 1);
	}
	uint32_t packed() const { return b3StoreWorldId(world); }
};

static Ref<HordeAgents> make_agents(int capacity = 250) {
	Ref<HordeAgents> a;
	a.instantiate();
	a->set_capacity(capacity);
	return a;
}

// ---------------------------------------------------------------------------
// 10. SoA alloc/free with epoch reuse correctness (NET R3.5).
// ---------------------------------------------------------------------------
TEST_CASE("[HordeSim][Agents] Slot allocation, free, and reuse-epoch id validation") {
	Ref<HordeAgents> agents = make_agents(4);

	const int a = agents->spawn(0, Vector3(1, 0, 1));
	const int b = agents->spawn(0, Vector3(2, 0, 2));
	CHECK(agents->get_active_count() == 2);
	CHECK(agents->is_alive(a));
	CHECK(agents->is_alive(b));
	CHECK(agents->get_agent_position(a) == Vector3(1, 0, 1));

	// Free `a`; its id must now fail validation, `b` still valid.
	CHECK(agents->despawn(a));
	CHECK_FALSE(agents->is_alive(a));
	CHECK(agents->is_alive(b));
	CHECK(agents->get_active_count() == 1);

	// Reusing the freed slot yields a DIFFERENT id (epoch bit flipped), and the
	// old id stays invalid even though it names the same slot.
	const int c = agents->spawn(1, Vector3(3, 0, 3));
	CHECK((c & HordeAgents::ID_SLOT_MASK) == (a & HordeAgents::ID_SLOT_MASK)); // Same slot.
	CHECK(c != a); // Different epoch.
	CHECK(agents->is_alive(c));
	CHECK_FALSE(agents->is_alive(a));
	CHECK(agents->get_agent_archetype(c) == 1);

	// Hard cap: a capacity-4 store yields at most 4 live agents.
	agents->spawn(0, Vector3(4, 0, 4));
	agents->spawn(0, Vector3(5, 0, 5));
	CHECK(agents->get_active_count() == 4);
	CHECK(agents->spawn(0, Vector3(6, 0, 6)) == -1); // Full.
}

// ---------------------------------------------------------------------------
// 11. FSM table load + scripted transition round-trip through the batched API.
// ---------------------------------------------------------------------------
TEST_CASE("[HordeSim][Agents] FSM config drives exit conditions; batched transition round-trips") {
	Ref<HordeFSMConfig> cfg;
	cfg.instantiate();
	cfg->load_defaults();
	CHECK(cfg->get_archetype_count() == 2);
	// Shambler Wake is a timed telegraph beat (A3.3), >= 0.5 s.
	CHECK(cfg->get_max_time(0, HordeAgents::STATE_WAKE) >= 0.5f);

	Ref<HordeAgents> agents = make_agents(8);
	agents->set_fsm_config(cfg);

	// A shambler in WAKE with a player far away: only the timer can fire.
	const int id = agents->spawn(0, Vector3(0, 0, 0), HordeAgents::STATE_WAKE);
	PackedVector3Array players;
	players.push_back(Vector3(1000, 0, 1000));
	agents->set_player_positions(players);

	// Not enough elapsed time yet -> no transition armed.
	agents->tick(0.1);
	CHECK(agents->query_transitions().is_empty());

	// Advance past the wake deadline -> exactly one armed transition, reason TIMER.
	for (int i = 0; i < 20; i++) {
		agents->tick(0.1);
	}
	PackedInt32Array pending = agents->query_transitions();
	REQUIRE(pending.size() == 4); // One agent: [id, archetype, state, reason].
	CHECK(pending[0] == id);
	CHECK(pending[1] == 0); // Shambler archetype rides the quad.
	CHECK(pending[2] == HordeAgents::STATE_WAKE);
	CHECK(pending[3] == HordeAgents::REASON_TIMER);

	// Script decides: WAKE -> ADVANCE. Apply through the batched API.
	PackedInt32Array apply;
	apply.push_back(id);
	apply.push_back(HordeAgents::STATE_ADVANCE);
	agents->apply_transitions(apply);
	CHECK(agents->get_agent_state(id) == HordeAgents::STATE_ADVANCE);
	// Timer reset on transition -> nothing armed the very next tick.
	agents->tick(0.1);
	CHECK(agents->query_transitions().is_empty());
}

// ---------------------------------------------------------------------------
// 12. LOD tier assignment vs authored player positions (R3.3).
// ---------------------------------------------------------------------------
TEST_CASE("[HordeSim][Agents] LOD tiers follow distance to the nearest player") {
	Ref<HordeAgents> agents = make_agents(8);
	agents->set_hot_distance(25.0f);
	agents->set_warm_distance(60.0f);

	const int hot = agents->spawn(0, Vector3(5, 0, 0)); // 5 m
	const int warm = agents->spawn(0, Vector3(40, 0, 0)); // 40 m
	const int cold = agents->spawn(0, Vector3(100, 0, 0)); // 100 m

	PackedVector3Array players;
	players.push_back(Vector3(0, 0, 0));
	agents->set_player_positions(players);
	agents->tick(1.0 / 128.0);

	CHECK(agents->get_agent_tier(hot) == HordeAgents::TIER_HOT);
	CHECK(agents->get_agent_tier(warm) == HordeAgents::TIER_WARM);
	CHECK(agents->get_agent_tier(cold) == HordeAgents::TIER_COLD);
	CHECK(agents->get_tier_count(HordeAgents::TIER_HOT) == 1);
	CHECK(agents->get_tier_count(HordeAgents::TIER_WARM) == 1);
	CHECK(agents->get_tier_count(HordeAgents::TIER_COLD) == 1);

	// Move the player next to the cold agent: tiers re-evaluate.
	players.set(0, Vector3(100, 0, 0));
	agents->set_player_positions(players);
	agents->tick(1.0 / 128.0);
	CHECK(agents->get_agent_tier(cold) == HordeAgents::TIER_HOT);
	CHECK(agents->get_agent_tier(hot) == HordeAgents::TIER_COLD);
}

// ---------------------------------------------------------------------------
// 13. Separation keeps Hot agents apart; Warm/Cold may overlap (R3.3, A2.3).
// ---------------------------------------------------------------------------
TEST_CASE("[HordeSim][Agents] Reynolds separation acts on Hot tier only") {
	auto min_pair_dist = [](HordeAgents *a, const Vector<int> &ids) {
		float best = 1e20f;
		for (int i = 0; i < ids.size(); i++) {
			for (int j = i + 1; j < ids.size(); j++) {
				const Vector3 pi = a->get_agent_position(ids[i]);
				const Vector3 pj = a->get_agent_position(ids[j]);
				best = MIN(best, (float)pi.distance_to(pj));
			}
		}
		return best;
	};

	// A tight cluster of chasers whose targets sit straight ahead (own x), so
	// only separation can move them laterally. The player position sets the tier.
	struct ClusterRun {
		int tier = -1;
		float before = 0.0f;
		float after = 0.0f;
		float separation_radius = 0.0f;
	};
	auto run_cluster = [&min_pair_dist](const Vector3 &p_player) {
		Ref<HordeAgents> a = make_agents(16);
		Vector<int> ids;
		for (int i = 0; i < 6; i++) {
			const int id = a->spawn(0, Vector3(0.05f * i, 0, 0), HordeAgents::STATE_CHASE);
			a->set_agent_target(id, Vector3(0.05f * i, 0, 50));
			ids.push_back(id);
		}
		PackedVector3Array players;
		players.push_back(p_player);
		a->set_player_positions(players);
		ClusterRun r;
		r.before = min_pair_dist(a.ptr(), ids);
		for (int t = 0; t < 200; t++) {
			a->tick(1.0 / 128.0);
		}
		r.after = min_pair_dist(a.ptr(), ids);
		r.tier = a->get_agent_tier(ids[0]);
		r.separation_radius = a->get_separation_radius();
		return r;
	};

	// Hot cluster (player adjacent): separation pushes the agents apart.
	const ClusterRun hot = run_cluster(Vector3(0, 0, 0));
	CHECK(hot.tier == HordeAgents::TIER_HOT);
	CHECK(hot.after > hot.before);
	CHECK(hot.after > hot.separation_radius * 0.5f);

	// Cold cluster (player far): no agent-vs-agent separation, so the clustered
	// agents stay clustered (they all translate identically).
	const ClusterRun cold = run_cluster(Vector3(1000, 0, 0));
	CHECK(cold.tier == HordeAgents::TIER_COLD);
	CHECK(cold.after == doctest::Approx(cold.before)); // Unchanged: overlap allowed.
}

// ---------------------------------------------------------------------------
// 14. Kinematic wall contact via the native Box3D mover sweep (R3.1, G6.5).
// ---------------------------------------------------------------------------
TEST_CASE("[HordeSim][Agents] Box3D mover sweep stops an agent at a static wall") {
	TestWorld tw;
	// Thin tall wall across the path at x = 2, spanning the capsule height.
	tw.add_wall(2.0f, 1.0f, 0.0f, 0.2f, 2.0f, 5.0f);
	tw.finalize();

	// Diagnostic probe: cast a capsule straight at the wall and confirm Box3D
	// reports a blocking fraction < 1 (isolates the query from agent integration).
	{
		b3Capsule cap;
		cap.center1 = b3Vec3{ 0.0f, 0.35f, 0.0f };
		cap.center2 = b3Vec3{ 0.0f, 1.45f, 0.0f };
		cap.radius = 0.35f;
		b3QueryFilter f = b3DefaultQueryFilter();
		f.categoryBits = UINT64_C(1) << 63;
		f.maskBits = 0xFFFFFFFF;
		const float frac = b3World_CastMover(tw.world, b3Vec3{ 1.0f, 0.0f, 0.0f }, &cap, b3Vec3{ 2.0f, 0.0f, 0.0f }, f, nullptr, nullptr);
		print_line(vformat("[HordeSim] wall probe cast fraction: %f", frac));
		CHECK(frac < 1.0f);
	}

	// A Hot chaser walking +X at a target on the far side of the wall; returns
	// its final x. Pass 0 to run without a collision world.
	auto run_to_wall = [](uint32_t p_world_packed) {
		Ref<HordeAgents> a = make_agents(4);
		if (p_world_packed != 0) {
			a->set_collision_world_packed(p_world_packed);
		}
		const int id = a->spawn(0, Vector3(0, 0, 0), HordeAgents::STATE_CHASE);
		a->set_agent_target(id, Vector3(6, 0, 0));
		PackedVector3Array players;
		players.push_back(Vector3(0, 0, 0)); // Keeps the agent Hot (wall queries run).
		a->set_player_positions(players);
		for (int t = 0; t < 400; t++) {
			a->tick(1.0 / 128.0);
		}
		return a->get_agent_position(id).x;
	};

	// Stopped in front of the wall face (x = 2 - half_extent 0.2 - radius).
	CHECK(run_to_wall(tw.packed()) < 1.9f);
	// Control: no collision world -> the agent sails through to its target.
	CHECK(run_to_wall(0) > 2.5f);
}

// ---------------------------------------------------------------------------
// 15. Link traversal between floors (OCTANT_LINK step-through, G6.5).
// ---------------------------------------------------------------------------
TEST_CASE("[HordeSim][Agents] Agent steps up an inter-floor link toward an upstairs goal") {
	Ref<HordeNavGrid> grid = make_grid(4, 4, 2);
	grid->link_cells(Vector3i(0, 0, 0), Vector3i(0, 0, 1), 1); // Ladder at cell (0,0).

	Ref<HordeFlowField> field;
	field.instantiate();
	field->set_grid(grid);
	field->add_goal(0, 0, 1); // Goal upstairs (floor 1).
	field->recompute_sync();
	REQUIRE(field->is_reachable(3, 3, 0));

	Ref<HordeAgents> agents = make_agents(4);
	agents->set_flow_field(field);
	// Spawn at the base of the ladder on floor 0.
	const Vector3 base = grid->cell_to_world(0, 0, 0);
	const int id = agents->spawn(0, base, HordeAgents::STATE_ADVANCE);
	PackedVector3Array players;
	players.push_back(base); // Hot.
	agents->set_player_positions(players);

	CHECK(agents->get_agent_position(id).y == doctest::Approx(0.0f));
	for (int t = 0; t < 200; t++) {
		agents->tick(1.0 / 64.0);
	}
	// The agent stepped through the link and reached the upstairs floor height.
	CHECK(agents->get_agent_position(id).y == doctest::Approx((float)grid->get_floor_height()));
}

// ---------------------------------------------------------------------------
// 16. Determinism: same seed/inputs -> identical agent state after N ticks.
// ---------------------------------------------------------------------------
TEST_CASE("[HordeSim][Agents] Identical inputs produce bit-identical agent state after N ticks") {
	auto build = []() -> Ref<HordeAgents> {
		Ref<HordeNavGrid> grid = make_grid(64, 64);
		Ref<HordeFlowField> field;
		field.instantiate();
		field->set_grid(grid);
		field->add_goal(0, 0, 0);
		field->recompute_sync();

		Ref<HordeFSMConfig> cfg;
		cfg.instantiate();
		cfg->load_defaults();

		Ref<HordeAgents> a = make_agents(64);
		a->set_flow_field(field);
		a->set_fsm_config(cfg);
		// Deterministic spread of agents across the grid.
		for (int i = 0; i < 40; i++) {
			const float fx = 2.0f + (float)((i * 7) % 28);
			const float fz = 2.0f + (float)((i * 5) % 28);
			a->spawn(i & 1, Vector3(fx, 0, fz), HordeAgents::STATE_ADVANCE);
		}
		return a;
	};

	Ref<HordeAgents> a = build();
	Ref<HordeAgents> b = build();

	for (int t = 0; t < 120; t++) {
		PackedVector3Array players;
		players.push_back(Vector3(10.0f + 0.05f * t, 0, 12.0f));
		a->set_player_positions(players);
		b->set_player_positions(players);
		a->tick(1.0 / 128.0);
		b->tick(1.0 / 128.0);

		// A fixed scripted transition policy: any armed agent advances its state
		// ring by one (deterministic, identical for both instances). Quads:
		// [id, archetype, state, reason].
		PackedInt32Array pa = a->query_transitions();
		PackedInt32Array pb = b->query_transitions();
		PackedInt32Array apply_a, apply_b;
		for (int k = 0; k + 3 < pa.size(); k += 4) {
			apply_a.push_back(pa[k]);
			apply_a.push_back((pa[k + 2] + 1) % HordeAgents::STATE_MAX);
		}
		for (int k = 0; k + 3 < pb.size(); k += 4) {
			apply_b.push_back(pb[k]);
			apply_b.push_back((pb[k + 2] + 1) % HordeAgents::STATE_MAX);
		}
		a->apply_transitions(apply_a);
		b->apply_transitions(apply_b);
	}

	PackedVector3Array pos_a = a->get_positions();
	PackedVector3Array pos_b = b->get_positions();
	PackedByteArray st_a = a->get_states();
	PackedByteArray st_b = b->get_states();
	PackedFloat32Array yaw_a = a->get_yaws();
	PackedFloat32Array yaw_b = b->get_yaws();

	REQUIRE(pos_a.size() == pos_b.size());
	bool identical = st_a == st_b && yaw_a == yaw_b;
	for (int i = 0; i < pos_a.size() && identical; i++) {
		// Bit-identical: exact float equality, not approximate.
		if (pos_a[i].x != pos_b[i].x || pos_a[i].y != pos_b[i].y || pos_a[i].z != pos_b[i].z) {
			identical = false;
		}
	}
	CHECK(identical);
}

// ---------------------------------------------------------------------------
// 17. Budget: 250 mixed-tier agents, moving player, everything on.
// ---------------------------------------------------------------------------
TEST_CASE("[HordeSim][Agents] 250-agent tick stays within the dev budget") {
	Ref<HordeNavGrid> grid = make_grid(256, 256);
	// A couple of walls with doorways so the field (and the agents) route.
	for (int y = 0; y < 220; y++) {
		grid->set_walkable(80, y, 0, false);
	}
	grid->set_walkable(80, 110, 0, true);

	Ref<HordeFlowField> field;
	field.instantiate();
	field->set_grid(grid);
	field->add_goal(250, 250, 0); // The base corner.
	field->recompute_sync();

	Ref<HordeFSMConfig> cfg;
	cfg.instantiate();
	cfg->load_defaults();

	// Static walls in world space for the mover sweep (everything on).
	TestWorld tw;
	const float cs = (float)grid->get_cell_size();
	tw.add_wall(80.0f * cs, 1.0f, 55.0f * cs, 0.5f, 2.0f, 55.0f * cs);
	tw.finalize();

	Ref<HordeAgents> agents = make_agents(250);
	agents->set_flow_field(field);
	agents->set_fsm_config(cfg);
	agents->set_collision_world_packed(tw.packed());

	// Spread 250 agents across the grid so the mix spans all three LOD tiers
	// relative to the (moving) player near one corner.
	for (int i = 0; i < 250; i++) {
		const float wx = ((float)((i * 13) % 250) + 1.0f) * cs;
		const float wz = ((float)((i * 29) % 250) + 1.0f) * cs;
		agents->spawn(i & 1, Vector3(wx, 0, wz), HordeAgents::STATE_ADVANCE);
	}
	CHECK(agents->get_active_count() == 250);

	// Warm up, then take the min tick time over a window that spans the LOD
	// stagger phase (the tick where Cold agents also run is the heaviest).
	auto move_player = [&](int t) {
		PackedVector3Array players;
		players.push_back(Vector3(20.0f + 0.1f * t, 0, 20.0f));
		agents->set_player_positions(players);
	};
	for (int t = 0; t < 8; t++) {
		move_player(t);
		agents->tick(1.0 / 128.0);
	}
	uint64_t usec = UINT64_MAX;
	for (int t = 8; t < 16; t++) {
		move_player(t);
		agents->tick(1.0 / 128.0);
		usec = MIN(usec, agents->get_tick_time_usec());
	}
	print_line(vformat("[HordeSim] 250-agent tick: %d us (%.3f ms) | hot=%d warm=%d cold=%d",
			(int64_t)usec, usec / 1000.0,
			agents->get_tier_count(HordeAgents::TIER_HOT),
			agents->get_tier_count(HordeAgents::TIER_WARM),
			agents->get_tier_count(HordeAgents::TIER_COLD)));

	// Dev-build (/Od-class) budget gate; the <= 4 ms release number is Gate 1.
	CHECK(usec < 12000);
}

} // namespace TestHordeSim
