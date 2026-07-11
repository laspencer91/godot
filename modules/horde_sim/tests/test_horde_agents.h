/**************************************************************************/
/*  test_horde_agents.h                                                  */
/**************************************************************************/

#pragma once

#include "horde_test_helpers.h"

// ===========================================================================
// HordeAgents (P1.B2) — SoA agent sim, FSM framework, LOD, kinematic movement.
// ===========================================================================

namespace TestHordeSim {

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

// ---------------------------------------------------------------------------
// 18. Obstacle-contact signal reaches script through the batched FSM API
// (P2.1 seam): a Hot agent whose mover sweep is clamped by a wall arms
// REASON_BLOCKED, and it round-trips through query_transitions() /
// apply_transitions() exactly like any other reason.
// ---------------------------------------------------------------------------
TEST_CASE("[HordeSim][Agents] Agent walking into the test wall arms REASON_BLOCKED") {
	TestWorld tw;
	// Same wall geometry as the mover-sweep test above.
	tw.add_wall(2.0f, 1.0f, 0.0f, 0.2f, 2.0f, 5.0f);
	tw.finalize();

	Ref<HordeAgents> a = make_agents(4);
	a->set_collision_world_packed(tw.packed());
	// A Hot chaser walking +X into the wall. No FSM config assigned, so
	// min_time/max_time/exit_range all default to 0 -- the wall contact is
	// the only thing that can arm a transition, an unambiguous signal.
	const int id = a->spawn(0, Vector3(0, 0, 0), HordeAgents::STATE_CHASE);
	a->set_agent_target(id, Vector3(6, 0, 0));
	PackedVector3Array players;
	players.push_back(Vector3(0, 0, 0)); // Keeps the agent Hot (wall queries run).
	a->set_player_positions(players);

	PackedInt32Array pending;
	for (int t = 0; t < 400 && pending.is_empty(); t++) {
		a->tick(1.0 / 128.0);
		pending = a->query_transitions();
	}

	REQUIRE(pending.size() == 4); // One agent: [id, archetype, state, reason].
	CHECK(pending[0] == id);
	CHECK(pending[1] == 0);
	CHECK(pending[2] == HordeAgents::STATE_CHASE);
	CHECK(pending[3] == HordeAgents::REASON_BLOCKED);

	// The batched API applies a REASON_BLOCKED transition like any other.
	PackedInt32Array apply;
	apply.push_back(id);
	apply.push_back(HordeAgents::STATE_ATTACK_OBSTACLE);
	a->apply_transitions(apply);
	CHECK(a->get_agent_state(id) == HordeAgents::STATE_ATTACK_OBSTACLE);
	// ATTACK_OBSTACLE has zero move speed (stationary board-attack), so the
	// next tick sweeps nothing and nothing re-arms.
	a->tick(1.0 / 128.0);
	CHECK(a->query_transitions().is_empty());
}

} // namespace TestHordeSim
