/**************************************************************************/
/*  test_horde_agents.h                                                  */
/**************************************************************************/

#pragma once

#include "horde_test_helpers.h"

#include "../horde_wire.h"

// ===========================================================================
// HordeAgents (P1.B2) — SoA agent sim, FSM framework, LOD, kinematic movement.
// Combat ingress (P2.4) — weapon ray, damage/stagger/death, blunt knockback.
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

// Deterministic 40-agent scene (64x64 flow field toward one corner + default
// FSM config) shared by the run-twice determinism cases (16 and 32).
static Ref<HordeAgents> make_determinism_scene() {
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
}

// ---------------------------------------------------------------------------
// 16. Determinism: same seed/inputs -> identical agent state after N ticks.
// ---------------------------------------------------------------------------
TEST_CASE("[HordeSim][Agents] Identical inputs produce bit-identical agent state after N ticks") {
	Ref<HordeAgents> a = make_determinism_scene();
	Ref<HordeAgents> b = make_determinism_scene();

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

// ---------------------------------------------------------------------------
// 26. Weapon ray vs the agent capsule: authored hit/miss/height_frac (P2.4).
// Default capsule: radius 0.35, height 1.8 -> axis band y in [0.35, 1.45],
// surface spans y in [0, 1.8].
// ---------------------------------------------------------------------------
TEST_CASE("[HordeSim][Combat] raycast_agents hits the capsule with authored precision") {
	Ref<HordeAgents> a = make_agents(8);
	const int id = a->spawn(0, Vector3(10, 0, 0));

	// Body shot: horizontal ray at mid height -> cylinder side at x = 10 - r.
	Dictionary hit = a->raycast_agents(Vector3(0, 0.9f, 0), Vector3(1, 0, 0), 50.0f);
	REQUIRE(!hit.is_empty());
	CHECK((int)hit["id"] == id);
	Vector3 hit_p = hit["hit_pos"];
	CHECK(hit_p.x == doctest::Approx(9.65f));
	CHECK(hit_p.y == doctest::Approx(0.9f));
	CHECK((float)hit["height_frac"] == doctest::Approx(0.5f));

	// Head-height ray clips the top cap sphere (center y 1.45, r 0.35):
	// x = 10 - sqrt(0.35^2 - 0.25^2), height_frac = 1.7 / 1.8.
	hit = a->raycast_agents(Vector3(0, 1.7f, 0), Vector3(1, 0, 0), 50.0f);
	REQUIRE(!hit.is_empty());
	hit_p = hit["hit_pos"];
	CHECK(hit_p.x == doctest::Approx(9.75505f));
	CHECK((float)hit["height_frac"] == doctest::Approx(0.94444f));

	// Straight down onto the crown: hit at y = 1.8, height_frac 1.
	hit = a->raycast_agents(Vector3(10, 5, 0), Vector3(0, -1, 0), 50.0f);
	REQUIRE(!hit.is_empty());
	hit_p = hit["hit_pos"];
	CHECK(hit_p.y == doctest::Approx(1.8f));
	CHECK((float)hit["height_frac"] == doctest::Approx(1.0f));

	// Unnormalized direction is normalized internally (same body shot).
	hit = a->raycast_agents(Vector3(0, 0.9f, 0), Vector3(3, 0, 0), 50.0f);
	REQUIRE(!hit.is_empty());
	CHECK(((Vector3)hit["hit_pos"]).x == doctest::Approx(9.65f));

	// Misses: over the crown, wide of the radius, out of range.
	CHECK(a->raycast_agents(Vector3(0, 1.9f, 0), Vector3(1, 0, 0), 50.0f).is_empty());
	CHECK(a->raycast_agents(Vector3(0, 0.9f, 0.4f), Vector3(1, 0, 0), 50.0f).is_empty());
	CHECK(a->raycast_agents(Vector3(0, 0.9f, 0), Vector3(1, 0, 0), 9.0f).is_empty());

	// Nearest-of-two: an agent in front shadows the one behind.
	const int near_id = a->spawn(0, Vector3(5, 0, 0));
	hit = a->raycast_agents(Vector3(0, 0.9f, 0), Vector3(1, 0, 0), 50.0f);
	REQUIRE(!hit.is_empty());
	CHECK((int)hit["id"] == near_id);
	CHECK(((Vector3)hit["hit_pos"]).x == doctest::Approx(4.65f));
}

// ---------------------------------------------------------------------------
// 27. Damage ingress rejects stale ids; corpses are excluded from the ray.
// ---------------------------------------------------------------------------
TEST_CASE("[HordeSim][Combat] Stale ids are rejected; corpses never raycast") {
	Ref<HordeAgents> a = make_agents(4);
	// No config assigned: DEFAULT_COMBAT applies (100 HP base).
	const int id = a->spawn(0, Vector3(5, 0, 0));
	CHECK(a->get_agent_hp(id) == 100.0f);
	CHECK(a->apply_damage(id, 10.0f, Vector3(1, 0, 0), 0) == HordeAgents::STATE_DORMANT);
	CHECK(a->get_agent_hp(id) == doctest::Approx(90.0f));

	// Free the slot, respawn into it: the old id must fail everywhere.
	a->despawn(id);
	CHECK(a->apply_damage(id, 50.0f, Vector3(1, 0, 0), 0) == -1);
	CHECK(a->get_death_info(id).is_empty());
	const int reused = a->spawn(1, Vector3(5, 0, 0));
	CHECK((reused & HordeAgents::ID_SLOT_MASK) == (id & HordeAgents::ID_SLOT_MASK)); // Same slot.
	CHECK(reused != id); // Epoch flipped.
	CHECK(a->apply_damage(id, 50.0f, Vector3(1, 0, 0), 0) == -1); // Stale id still dead.
	CHECK(a->get_agent_hp(reused) == 100.0f); // The new tenant is untouched.

	// The ray reports the CURRENT epoch's id.
	Dictionary hit = a->raycast_agents(Vector3(0, 0.9f, 0), Vector3(1, 0, 0), 50.0f);
	REQUIRE(!hit.is_empty());
	CHECK((int)hit["id"] == reused);

	// Kill it: the corpse stops raycasting the same call sequence that hit it.
	CHECK(a->apply_damage(reused, 200.0f, Vector3(1, 0, 0), 1) == HordeAgents::STATE_DEAD);
	CHECK(a->raycast_agents(Vector3(0, 0.9f, 0), Vector3(1, 0, 0), 50.0f).is_empty());
}

// ---------------------------------------------------------------------------
// 28. Damage -> stagger -> recover: the config threshold gates the stagger,
// the agent halts for stagger_duration_ticks, then the prior movement mode
// resumes natively (P2.4, COMBAT_FEEL section 3 tier B / A3.6).
// ---------------------------------------------------------------------------
TEST_CASE("[HordeSim][Combat] Threshold hit staggers, halts, then resumes the prior state") {
	Ref<HordeFSMConfig> cfg;
	cfg.instantiate();
	cfg->load_defaults(); // Combat row: 100 HP, frac 0.35, 90 ticks.

	Ref<HordeAgents> a = make_agents(8);
	a->set_fsm_config(cfg);
	const int id = a->spawn(0, Vector3(0, 0, 0), HordeAgents::STATE_CHASE);
	a->set_agent_target(id, Vector3(0, 0, 40));
	PackedVector3Array players;
	players.push_back(Vector3(10, 0, 0)); // Hot tier (full-rate cadence), out of exit_range.
	a->set_player_positions(players);

	// Chasing: the agent advances toward its target.
	for (int t = 0; t < 10; t++) {
		a->tick(1.0 / 128.0);
	}
	CHECK(a->get_agent_position(id).z > 0.1f);

	// Sub-threshold hit (pistol-class 34 < 35): HP drops, no stagger.
	CHECK(a->apply_damage(id, 34.0f, Vector3(0, 0, -1), 0) == HordeAgents::STATE_CHASE);
	CHECK(a->get_agent_hp(id) == doctest::Approx(66.0f));

	// Threshold hit (rifle-class 40 >= 35): STAGGER, and the agent halts.
	CHECK(a->apply_damage(id, 40.0f, Vector3(0, 0, -1), 1) == HordeAgents::STATE_STAGGER);
	const Vector3 held = a->get_agent_position(id);
	for (int t = 0; t < 89; t++) {
		a->tick(1.0 / 128.0);
	}
	CHECK(a->get_agent_state(id) == HordeAgents::STATE_STAGGER);
	CHECK(a->get_agent_position(id) == held); // Bit-identical: fully halted.
	CHECK(a->query_transitions().is_empty()); // Native stagger arms no exits.

	// Tick 90 expires the window: prior state resumes and movement restarts.
	a->tick(1.0 / 128.0);
	CHECK(a->get_agent_state(id) == HordeAgents::STATE_CHASE);
	a->tick(1.0 / 128.0);
	CHECK(a->get_agent_position(id).z > held.z);

	// Config plumbing: a retuned row (frac 0.10, 30 ticks) staggers a 12-damage
	// hit, and a re-stagger mid-window refreshes it but still resumes CHASE.
	cfg->set_combat_rule(0, 100.0f, 0.10f, 30, 0.50f, 0.6f, 16);
	CHECK(a->apply_damage(id, 12.0f, Vector3(0, 0, -1), 0) == HordeAgents::STATE_STAGGER);
	for (int t = 0; t < 10; t++) {
		a->tick(1.0 / 128.0);
	}
	CHECK(a->apply_damage(id, 12.0f, Vector3(0, 0, -1), 0) == HordeAgents::STATE_STAGGER); // Refresh.
	for (int t = 0; t < 29; t++) {
		a->tick(1.0 / 128.0);
	}
	CHECK(a->get_agent_state(id) == HordeAgents::STATE_STAGGER);
	a->tick(1.0 / 128.0);
	CHECK(a->get_agent_state(id) == HordeAgents::STATE_CHASE); // Original prior state, not STAGGER.
}

TEST_CASE("[HordeSim][Combat] Heavy surviving hit enters timed knockdown and get-up states") {
	Ref<HordeFSMConfig> cfg;
	cfg.instantiate();
	cfg->load_defaults();
	CHECK(cfg->get_knockdown_damage_frac(0) == doctest::Approx(0.50f));

	Ref<HordeAgents> a = make_agents(8);
	a->set_fsm_config(cfg);
	const int id = a->spawn(0, Vector3(), HordeAgents::STATE_ATTACK_PLAYER, 100.0f);
	PackedVector3Array players;
	players.push_back(Vector3());
	a->set_player_positions(players); // Keep the timer test at Hot-tier cadence.

	CHECK(a->apply_damage(id, 55.0f, Vector3(0, 0, -1), 0) == HordeAgents::STATE_KNOCKDOWN);
	CHECK(a->get_agent_state(id) == HordeAgents::STATE_KNOCKDOWN);
	for (int t = 0; t < 10; t++) {
		a->tick(0.1);
	}
	// Small follow-up damage while down neither pops to STAGGER nor restarts the fall timer.
	CHECK(a->apply_damage(id, 5.0f, Vector3(0, 0, -1), 0) == HordeAgents::STATE_KNOCKDOWN);
	for (int t = 0; t < 7; t++) {
		a->tick(0.1);
	}
	CHECK(a->query_transitions().is_empty());
	a->tick(0.1);
	PackedInt32Array down_exit = a->query_transitions();
	REQUIRE(down_exit.size() == 4);
	CHECK(down_exit[2] == HordeAgents::STATE_KNOCKDOWN);
	CHECK(down_exit[3] == HordeAgents::REASON_TIMER);
	CHECK(a->apply_transition(id, HordeAgents::STATE_GET_UP));
	CHECK(a->apply_damage(id, 35.0f, Vector3(0, 0, -1), 0) == HordeAgents::STATE_KNOCKDOWN);
	CHECK(a->get_agent_state(id) == HordeAgents::STATE_KNOCKDOWN); // Qualifying rise interrupt.
	CHECK(a->apply_transition(id, HordeAgents::STATE_GET_UP));

	for (int t = 0; t < 15; t++) {
		a->tick(0.1);
	}
	CHECK(a->query_transitions().is_empty());
	a->tick(0.1);
	PackedInt32Array rise_exit = a->query_transitions();
	REQUIRE(rise_exit.size() == 4);
	CHECK(rise_exit[2] == HordeAgents::STATE_GET_UP);
	CHECK(rise_exit[3] == HordeAgents::REASON_TIMER);
}

TEST_CASE("[HordeSim][Combat] Per-hit stagger duration overrides the window but not the damage gate") {
	Ref<HordeFSMConfig> cfg;
	cfg.instantiate();
	cfg->load_defaults(); // 100 HP, 0.35 threshold, archetype default 90 ticks.

	Ref<HordeAgents> a = make_agents(8);
	a->set_fsm_config(cfg);
	const int id = a->spawn(0, Vector3(), HordeAgents::STATE_CHASE);

	// An override cannot turn a sub-threshold hit into a stagger.
	CHECK(a->apply_damage(id, 10.0f, Vector3(0, 0, -1), 0, 0.0f, 128) == HordeAgents::STATE_CHASE);
	// A qualifying bat-class hit uses its exact one-second window at the project's 128 Hz tick rate.
	CHECK(a->apply_damage(id, 40.0f, Vector3(0, 0, -1), 0, 0.0f, 128) == HordeAgents::STATE_STAGGER);
	for (int t = 0; t < 127; t++) {
		a->tick(1.0 / 128.0);
	}
	CHECK(a->get_agent_state(id) == HordeAgents::STATE_STAGGER);
	a->tick(1.0 / 128.0);
	CHECK(a->get_agent_state(id) == HordeAgents::STATE_CHASE);
}

// ---------------------------------------------------------------------------
// 29. Damage -> death: the native DEAD transition emits a REASON_DIED quad,
// and killer_hint/impulse_dir carry through to the R3.9 death event payload.
// ---------------------------------------------------------------------------
TEST_CASE("[HordeSim][Combat] Lethal damage emits the death quad and carries the killing hit") {
	Ref<HordeFSMConfig> cfg;
	cfg.instantiate();
	cfg->load_defaults();

	Ref<HordeAgents> a = make_agents(8);
	a->set_fsm_config(cfg);
	const int id = a->spawn(0, Vector3(5, 0, 0), HordeAgents::STATE_CHASE);

	// First hit knocks down (55 >= 50); the second kills through the knockdown.
	CHECK(a->apply_damage(id, 55.0f, Vector3(1, 0, 0), 7) == HordeAgents::STATE_KNOCKDOWN);
	CHECK(a->apply_damage(id, 60.0f, Vector3(0.6f, 0.2f, 0.3f), 2) == HordeAgents::STATE_DEAD);
	CHECK(a->get_agent_state(id) == HordeAgents::STATE_DEAD);
	CHECK(a->get_agent_hp(id) == 0.0f); // Clamped, never negative.
	CHECK(a->is_alive(id)); // Active (and id-valid) until script despawns it.

	// The standard quad, reason REASON_DIED, state already DEAD.
	PackedInt32Array q = a->query_transitions();
	REQUIRE(q.size() == 4);
	CHECK(q[0] == id);
	CHECK(q[1] == 0);
	CHECK(q[2] == HordeAgents::STATE_DEAD);
	CHECK(q[3] == HordeAgents::REASON_DIED);

	// Unacked, it persists across ticks (reliable at-least-once).
	a->tick(1.0 / 128.0);
	CHECK(a->query_transitions() == q);

	// The killing hit feeds the R3.9 death event payload, end to end.
	Dictionary info = a->get_death_info(id);
	REQUIRE(!info.is_empty());
	CHECK((int)info["killer_hint"] == 2);
	CHECK((Vector3)info["impulse_dir"] == Vector3(0.6f, 0.2f, 0.3f));
	Ref<HordeWireCodec> codec;
	codec.instantiate();
	Dictionary evt = codec->decode_death(codec->encode_death(q[0], (int)info["killer_hint"], (Vector3)info["impulse_dir"]));
	CHECK((int)evt["id"] == id);
	CHECK((int)evt["killer_hint"] == 2);
	CHECK((Vector3)evt["impulse_dir"] == Vector3(0.6f, 0.2f, 0.3f));

	// Script acks like any transition; nothing re-arms.
	CHECK(a->apply_transition(id, HordeAgents::STATE_DEAD));
	a->tick(1.0 / 128.0);
	CHECK(a->query_transitions().is_empty());

	// Corpses absorb nothing and keep the killing hit's info.
	CHECK(a->apply_damage(id, 50.0f, Vector3(1, 0, 0), 3) == HordeAgents::STATE_DEAD);
	CHECK((int)((Dictionary)a->get_death_info(id))["killer_hint"] == 2);
	CHECK(a->get_agent_hp(id) == 0.0f);

	// The corpse no longer shadows agents behind it on the weapon ray.
	const int behind = a->spawn(0, Vector3(10, 0, 0));
	Dictionary hit = a->raycast_agents(Vector3(0, 0.9f, 0), Vector3(1, 0, 0), 50.0f);
	REQUIRE(!hit.is_empty());
	CHECK((int)hit["id"] == behind);
}

// ---------------------------------------------------------------------------
// 30. Blunt knockback: config-capped displacement decaying over the config
// window, moved through the CastMover + skin path -- never through walls.
// ---------------------------------------------------------------------------
TEST_CASE("[HordeSim][Combat] Knockback is capped, decays to rest, and stops at walls") {
	TestWorld tw;
	// Same wall geometry as the mover-sweep test: face plane at x = 1.8.
	tw.add_wall(2.0f, 1.0f, 0.0f, 0.2f, 2.0f, 5.0f);
	tw.finalize();

	Ref<HordeAgents> a = make_agents(4);
	a->set_collision_world_packed(tw.packed());
	PackedVector3Array players;
	players.push_back(Vector3(0, 0, 0)); // Hot: the wall sweep runs.
	a->set_player_positions(players);

	// Open field along +Z: a 5 m request is capped to the 0.6 m default, the
	// stumble decays inside the 16-tick window, then the agent is fully at rest.
	const int id = a->spawn(0, Vector3(0, 0, 0)); // DORMANT: no locomotion of its own.
	CHECK(a->apply_damage(id, 10.0f, Vector3(0, 0, 1), 0, 5.0f) == HordeAgents::STATE_DORMANT);
	for (int t = 0; t < 24; t++) {
		a->tick(1.0 / 128.0);
	}
	const Vector3 rest = a->get_agent_position(id);
	CHECK(rest.z == doctest::Approx(0.6f).epsilon(0.01));
	CHECK(rest.x == doctest::Approx(0.0f));
	CHECK(a->get_agent_yaw(id) == 0.0f); // A pure shove never turns the agent.
	for (int t = 0; t < 24; t++) {
		a->tick(1.0 / 128.0);
	}
	CHECK(a->get_agent_position(id) == rest); // Bit-identical: knockback exhausted.

	// Into the wall: the same 0.6 m shove stops at the face (1.8 - radius 0.35,
	// less the 2 cm skin), never inside or beyond it.
	const int id2 = a->spawn(0, Vector3(1.2f, 0, 0));
	CHECK(a->apply_damage(id2, 10.0f, Vector3(1, 0, 0), 0, 0.6f) == HordeAgents::STATE_DORMANT);
	for (int t = 0; t < 24; t++) {
		a->tick(1.0 / 128.0);
	}
	const float x = a->get_agent_position(id2).x;
	CHECK(x > 1.35f); // It did move...
	CHECK(x < 1.46f); // ...but the wall clamped it (unblocked it would reach 1.8).
}

// ---------------------------------------------------------------------------
// 31. Budget: raycast_agents against 250 live agents (measured + printed;
// runs per shot, not per tick, but melee arcs may probe a few times).
// ---------------------------------------------------------------------------
TEST_CASE("[HordeSim][Combat] 250-agent raycast stays within the per-shot budget") {
	Ref<HordeAgents> agents = make_agents(250);
	// The wire-budget spread: a full map's worth of candidates along the ray.
	for (int i = 0; i < 250; i++) {
		const float wx = (float)((i * 13) % 640) + 1.0f;
		const float wz = (float)((i * 29) % 640) + 1.0f;
		agents->spawn(i & 1, Vector3(wx, 0, wz), HordeAgents::STATE_ADVANCE);
	}
	CHECK(agents->get_active_count() == 250);

	// A long diagonal shot across the crowd (aimed at agent i=137's slot).
	const Vector3 from(0, 0.9f, 0);
	const Vector3 dir = Vector3(502, 0, 134).normalized();
	Dictionary hit = agents->raycast_agents(from, dir, 900.0f);
	REQUIRE(!hit.is_empty()); // The budget ray does real work.

	// Same call twice -> identical result (const query, deterministic).
	Dictionary again = agents->raycast_agents(from, dir, 900.0f);
	CHECK((int)again["id"] == (int)hit["id"]);
	CHECK((Vector3)again["hit_pos"] == (Vector3)hit["hit_pos"]);

	// Warm up, then min-of-N batches (32 calls each): per-call cost without
	// test-box scheduling noise (D-016 discipline).
	for (int w = 0; w < 8; w++) {
		agents->raycast_agents(from, dir, 900.0f);
	}
	uint64_t best_batch = UINT64_MAX;
	for (int trial = 0; trial < 16; trial++) {
		const uint64_t t0 = OS::get_singleton()->get_ticks_usec();
		for (int k = 0; k < 32; k++) {
			agents->raycast_agents(from, dir, 900.0f);
		}
		best_batch = MIN(best_batch, OS::get_singleton()->get_ticks_usec() - t0);
	}
	const double per_call = (double)best_batch / 32.0;
	print_line(vformat("[HordeSim] raycast vs 250 agents: %.2f us/call (min-of-16 x32 batch)", per_call));

	// Dev-build (/Od-class) gate for the <= 50 us per-shot target.
	CHECK(per_call < 50.0);
}

// ---------------------------------------------------------------------------
// 32. Determinism: an identical tick + damage + knockback + despawn sequence
// produces bit-identical state, run twice (L6 -- no RNG anywhere in ingress).
// ---------------------------------------------------------------------------
TEST_CASE("[HordeSim][Combat] Identical damage sequences produce bit-identical state, run twice") {
	Ref<HordeAgents> a = make_determinism_scene();
	Ref<HordeAgents> b = make_determinism_scene();
	const PackedInt32Array ids = a->get_active_ids(); // Identical for b (deterministic spawn).

	// One fixed script for both instances: scripted damage (mixed lethal /
	// stagger / knockback hits, some landing on stale ids after despawns --
	// no-ops on both sides), plus the ring transition policy from case 16 with
	// deaths despawned. Raycast runs against `a` only: a const query must not
	// perturb state, or the two instances diverge.
	for (int t = 0; t < 150; t++) {
		PackedVector3Array players;
		players.push_back(Vector3(10.0f + 0.05f * t, 0, 12.0f));
		a->set_player_positions(players);
		b->set_player_positions(players);
		a->tick(1.0 / 128.0);
		b->tick(1.0 / 128.0);

		// Cycle fire across the first 8 spawns so most take the ~3 hits that
		// kill (later hits land on despawned stale ids: identical no-ops).
		if (t % 5 == 2) {
			const int target = ids[(t * 3) % 8];
			const float amount = 40.0f + (float)(t % 7);
			const Vector3 imp(0.6f, 0.1f, -0.8f);
			const float kb = (t % 2 == 0) ? 0.4f : 0.0f;
			a->apply_damage(target, amount, imp, t % 4, kb);
			b->apply_damage(target, amount, imp, t % 4, kb);
		}
		a->raycast_agents(Vector3(0, 0.9f, 0), Vector3(1, 0, 0.4f), 100.0f);

		// Ring policy from case 16, plus deaths despawn (the R3.9 path's shape).
		auto step_policy = [](const Ref<HordeAgents> &h) {
			const PackedInt32Array pending = h->query_transitions();
			PackedInt32Array apply;
			for (int k = 0; k + 3 < pending.size(); k += 4) {
				if (pending[k + 3] == HordeAgents::REASON_DIED) {
					h->despawn(pending[k]);
				} else {
					apply.push_back(pending[k]);
					apply.push_back((pending[k + 2] + 1) % HordeAgents::STATE_MAX);
				}
			}
			h->apply_transitions(apply);
		};
		step_policy(a);
		step_policy(b);
	}

	// Bit-identical survivors: ids, positions, states, yaws, HP.
	const PackedInt32Array ids_a = a->get_active_ids();
	const PackedInt32Array ids_b = b->get_active_ids();
	REQUIRE(ids_a == ids_b);
	CHECK(ids_a.size() < ids.size()); // The lethal hits actually culled some.
	const PackedVector3Array pos_a = a->get_positions();
	const PackedVector3Array pos_b = b->get_positions();
	bool identical = a->get_states() == b->get_states() && a->get_yaws() == b->get_yaws();
	for (int i = 0; i < pos_a.size() && identical; i++) {
		if (pos_a[i].x != pos_b[i].x || pos_a[i].y != pos_b[i].y || pos_a[i].z != pos_b[i].z) {
			identical = false;
		}
	}
	for (int i = 0; i < ids_a.size() && identical; i++) {
		if (a->get_agent_hp(ids_a[i]) != b->get_agent_hp(ids_a[i])) {
			identical = false;
		}
	}
	CHECK(identical);

	// And the weapon ray agrees between the twins, exactly.
	Dictionary ra = a->raycast_agents(Vector3(0, 0.9f, 0), Vector3(1, 0, 0.4f), 100.0f);
	Dictionary rb = b->raycast_agents(Vector3(0, 0.9f, 0), Vector3(1, 0, 0.4f), 100.0f);
	REQUIRE(ra.is_empty() == rb.is_empty());
	if (!ra.is_empty()) {
		CHECK((int)ra["id"] == (int)rb["id"]);
		CHECK((Vector3)ra["hit_pos"] == (Vector3)rb["hit_pos"]);
		CHECK((float)ra["height_frac"] == (float)rb["height_frac"]);
	}
}

// ---------------------------------------------------------------------------
// 33. Dense-clump yaw stability: facing slews toward the desired (pre-separation)
//     heading at a capped rate, so a crowd never snaps its heading tick-to-tick.
//     Regression for the flicker where yaw = atan2(disp) rode the tick-to-tick
//     flipping Reynolds separation push in a tight clump (max_turn_rate fix).
// ---------------------------------------------------------------------------
TEST_CASE("[HordeSim][Agents] Dense clump slews facing at capped turn rate (no flicker)") {
	Ref<HordeAgents> a = make_agents(32);
	// Isolate the separation-vs-target facing guard from the close-range player
	// seek (COMBAT_FEEL, covered by its own case below): the player sits on the
	// clump here to keep every agent Hot, which would otherwise arm close_seek and
	// point facing at the player instead of the shared far target.
	a->set_attack_seek_radius(0.0f);

	// 16 chasers packed inside a single separation radius (0.9 m): a 4x4 grid at
	// 0.12 m spacing (diag ~0.51 m) so every pair separates hard and the push
	// direction churns every tick -- exactly the condition that snapped the raw
	// atan2(disp) heading.
	Vector<int> ids;
	for (int gz = 0; gz < 4; gz++) {
		for (int gx = 0; gx < 4; gx++) {
			const int id = a->spawn(0, Vector3(0.12f * gx, 0, 0.12f * gz), HordeAgents::STATE_CHASE);
			// One shared, distant target -> a stable desired heading (~PI/4) for all.
			a->set_agent_target(id, Vector3(60, 0, 60));
			ids.push_back(id);
		}
	}
	// Player on the clump -> every agent is Hot, so separation is live.
	PackedVector3Array players;
	players.push_back(Vector3(0.18f, 0, 0.18f));
	a->set_player_positions(players);

	const double dt = 1.0 / 128.0;
	const float cap = a->get_max_turn_rate() * (float)dt; // Hot tier: eff_dt == dt.
	const float eps = 1e-3f;

	auto wrap_pi = [](float x) {
		return Math::fposmod(x + (float)Math::PI, (float)Math::TAU) - (float)Math::PI;
	};

	// The per-tick guard the bug violated: shortest-arc |Δyaw| never exceeds the
	// configured turn-rate cap for any agent on any tick.
	float max_step_seen = 0.0f;
	for (int t = 0; t < 120; t++) {
		Vector<float> before;
		for (int k = 0; k < ids.size(); k++) {
			before.push_back(a->get_agent_yaw(ids[k]));
		}
		a->tick(dt);
		for (int k = 0; k < ids.size(); k++) {
			const float dy = Math::abs(wrap_pi(a->get_agent_yaw(ids[k]) - before[k]));
			CHECK(dy <= cap + eps);
			max_step_seen = MAX(max_step_seen, dy);
		}
	}
	// Non-vacuous: agents really did turn (from yaw 0 toward ~PI/4), at the cap.
	CHECK(max_step_seen > 0.0f);
	CHECK(max_step_seen == doctest::Approx(cap).epsilon(0.05));

	// Converged onto the DESIRED heading (toward the shared target), proving the
	// facing tracks the stable steering dir and not the flipping separation push.
	for (int k = 0; k < ids.size(); k++) {
		const Vector3 p = a->get_agent_position(ids[k]);
		const float want = Math::atan2(60.0f - p.x, 60.0f - p.z);
		CHECK(Math::abs(wrap_pi(a->get_agent_yaw(ids[k]) - want)) <= 2.0f * cap);
	}
}

// ---------------------------------------------------------------------------
// 34. Close-range attack seek (COMBAT_FEEL): once an attacking agent is within
//     attack_seek_radius of a player it steers/faces the player's real position
//     directly, overriding the flow field / seek target. This fixes "attack the
//     air" -- ATTACK_PLAYER has speed 0 and previously hit the disp==0 early
//     return before facing ever updated, so a zombie held whatever heading the
//     separation ring left it with and swung away from its victim.
// ---------------------------------------------------------------------------
TEST_CASE("[HordeSim][Agents] Close-range attack seek faces and closes on the player") {
	auto wrap_pi = [](float x) {
		return Math::fposmod(x + (float)Math::PI, (float)Math::TAU) - (float)Math::PI;
	};
	const double dt = 1.0 / 128.0;
	const float seek_radius = 3.25f;

	// A. ATTACK_PLAYER (speed 0) turns to face its victim even though it never
	// moves. Spawn facing +Z (yaw 0); the player sits behind-left, 1.5 m away
// (inside the 3.25 m default radius). The heading must slew to point at it and
	// the agent must not drift (speed 0 -> position pinned).
	{
		Ref<HordeAgents> a = make_agents(4);
		a->set_attack_seek_radius(seek_radius);
		const int id = a->spawn(0, Vector3(0, 0, 0), HordeAgents::STATE_ATTACK_PLAYER);
		const Vector3 player(-1.2f, 0, -0.9f); // |xz| = 1.5 m.
		PackedVector3Array players;
		players.push_back(player);
		a->set_player_positions(players);
		for (int t = 0; t < 96; t++) {
			a->set_player_positions(players);
			a->tick(dt);
		}
		const float want = Math::atan2(player.x, player.z);
		CHECK(Math::abs(wrap_pi(a->get_agent_yaw(id) - want)) <= 0.02f);
		CHECK(a->get_agent_position(id) == Vector3(0, 0, 0)); // Never moved.
	}

	// B. A CHASE agent inside the radius bee-lines at the PLAYER, overriding a
	// seek target pointed the other way (proving close_seek pre-empts the normal
	// movement mode, not merely augments it).
	{
		Ref<HordeAgents> a = make_agents(4);
		a->set_attack_seek_radius(seek_radius);
		const int id = a->spawn(0, Vector3(1.5f, 0, 0), HordeAgents::STATE_CHASE);
		a->set_agent_target(id, Vector3(1.5f, 0, 100.0f)); // Far +Z: the "wrong" way.
		PackedVector3Array players;
		players.push_back(Vector3(0, 0, 0)); // 1.5 m away, inside the radius.
		for (int t = 0; t < 30; t++) {
			a->set_player_positions(players);
			a->tick(dt);
		}
		const Vector3 p = a->get_agent_position(id);
		CHECK(p.x < 1.5f); // Closed toward the player at the origin (-X)...
		CHECK(p.x > 0.5f); // ...without overshooting through it.
		CHECK(Math::abs(p.z) < 0.05f); // Ignored the +Z seek target entirely.
		CHECK(Math::abs(wrap_pi(a->get_agent_yaw(id) - Math::atan2(-p.x, 0.0f))) <= 0.1f);
	}

	// C. Radius gate: the SAME setup at 5 m (outside the 3.25 m radius) leaves the
	// agent on its normal seek target (+Z), never veering toward the player.
	{
		Ref<HordeAgents> a = make_agents(4);
		a->set_attack_seek_radius(seek_radius);
		const int id = a->spawn(0, Vector3(5.0f, 0, 0), HordeAgents::STATE_CHASE);
		a->set_agent_target(id, Vector3(5.0f, 0, 100.0f));
		PackedVector3Array players;
		players.push_back(Vector3(0, 0, 0)); // 5 m away: close_seek must NOT arm.
		for (int t = 0; t < 30; t++) {
			a->set_player_positions(players);
			a->tick(dt);
		}
		const Vector3 p = a->get_agent_position(id);
		CHECK(p.z > 0.1f); // Followed the +Z target...
		CHECK(p.x == doctest::Approx(5.0f).epsilon(0.01)); // ...not pulled toward the origin.
	}

	// D. WAKE is stationary but visibly turns toward the player.
	{
		Ref<HordeAgents> a = make_agents(4);
		a->set_attack_seek_radius(seek_radius);
		const int id = a->spawn(0, Vector3(40.0f, 0, 20.0f), HordeAgents::STATE_WAKE);
		PackedVector3Array players;
		players.push_back(Vector3(42.0f, 0, 20.0f));
		for (int t = 0; t < 64; t++) {
			a->set_player_positions(players);
			a->tick(dt);
		}
		CHECK(a->get_agent_position(id) == Vector3(40.0f, 0, 20.0f));
		CHECK(Math::abs(wrap_pi(a->get_agent_yaw(id) - (float)Math::PI / 2.0f)) <= 0.02f);
	}

	// E. The chase-entry boundary closes on a moving player far from origin.
	{
		Ref<HordeAgents> a = make_agents(4);
		a->set_attack_seek_radius(seek_radius);
		const int id = a->spawn(0, Vector3(100.0f, 0, -70.0f), HordeAgents::STATE_CHASE);
		a->set_agent_target(id, Vector3(0, 0, 0));
		Vector3 player(102.95f, 0, -70.0f);
		for (int t = 0; t < 64; t++) {
			player.x += 0.002f;
			PackedVector3Array players;
			players.push_back(player);
			a->set_player_positions(players);
			a->tick(dt);
		}
		const Vector3 p = a->get_agent_position(id);
		CHECK(p.x > 100.5f);
		CHECK(p.distance_to(player) < 2.95f);
		CHECK(Math::abs(p.z + 70.0f) < 0.05f);
	}

	// F. Zero radius disables the override entirely.
	{
		Ref<HordeAgents> a = make_agents(4);
		a->set_attack_seek_radius(0.0f);
		const int id = a->spawn(0, Vector3(1.5f, 0, 0), HordeAgents::STATE_CHASE);
		a->set_agent_target(id, Vector3(1.5f, 0, 100.0f));
		PackedVector3Array players;
		players.push_back(Vector3(0, 0, 0));
		for (int t = 0; t < 30; t++) {
			a->set_player_positions(players);
			a->tick(dt);
		}
		CHECK(a->get_agent_position(id).z > 0.1f);
	}

	// G. A moving ATTACK_PLAYER commits its lunge but stops at the authored
	// center-distance standoff instead of crossing through the victim. A second
	// step proves the clamp holds for the remainder of the attack window.
	{
		Ref<HordeFSMConfig> cfg;
		cfg.instantiate();
		cfg->load_defaults();
		cfg->set_rule(0, HordeAgents::STATE_ATTACK_PLAYER, 4.2f, 0.0f, 0.5f, 0.0f);
		cfg->set_movement_mode(0, HordeAgents::STATE_ATTACK_PLAYER, HordeFSMConfig::MOVE_SEEK_TARGET);

		Ref<HordeAgents> a = make_agents(4);
		a->set_fsm_config(cfg);
		a->set_attack_seek_radius(seek_radius);
		a->set_attack_standoff_distance(0.8f);
		const int id = a->spawn(0, Vector3(0, 0, 0), HordeAgents::STATE_ATTACK_PLAYER);
		const Vector3 player(0, 0, 2.0f);
		PackedVector3Array players;
		players.push_back(player);
		a->set_player_positions(players);
		a->tick(0.5);
		CHECK(a->get_agent_position(id).z == doctest::Approx(1.2f).epsilon(0.001));
		a->set_player_positions(players);
		a->tick(0.25);
		CHECK(a->get_agent_position(id).z == doctest::Approx(1.2f).epsilon(0.001));
		CHECK(a->get_agent_yaw(id) == doctest::Approx(0.0f).epsilon(0.001));
	}
}

// ---------------------------------------------------------------------------
// 35. Authored facing writer validates ids/input, wraps yaw, and dormant
//     agents retain the authored direction across native ticks.
// ---------------------------------------------------------------------------
TEST_CASE("[HordeSim][Agents] Authored yaw validates, normalizes, and persists") {
	Ref<HordeAgents> a = make_agents(2);
	const int id = a->spawn(0, Vector3(), HordeAgents::STATE_DORMANT);
	CHECK(a->set_agent_yaw(id, (float)Math::TAU + 0.75f));
	CHECK(a->get_agent_yaw(id) == doctest::Approx(0.75f));
	a->tick(1.0 / 128.0);
	CHECK(a->get_agent_yaw(id) == doctest::Approx(0.75f));

	CHECK_FALSE(a->set_agent_yaw(id, Math::INF));
	CHECK(a->get_agent_yaw(id) == doctest::Approx(0.75f));
	CHECK(a->despawn(id));
	CHECK_FALSE(a->set_agent_yaw(id, 0.0f));
}

// ---------------------------------------------------------------------------
// 36. Melee broadphase is an exact query sphere versus the same capsule used
//     by movement/raycast, with deterministic contact ordering.
// ---------------------------------------------------------------------------
TEST_CASE("[HordeSim][Combat] overlap_agents returns exact sorted capsule contacts") {
	Ref<HordeAgents> a = make_agents(8);
	const int eye_id = a->spawn(0, Vector3(2.0f, 0, 0));
	Array hits = a->overlap_agents(Vector3(0, 1.5f, 0), 1.7f, 8);
	REQUIRE(hits.size() == 1);
	Dictionary eye = hits[0];
	CHECK((int)eye["id"] == eye_id);
	CHECK(((Vector3)eye["hit_pos"]).x == doctest::Approx(1.65011f).epsilon(0.0001));

	// Top, middle, and bottom use the capsule surface contact, not feet distance.
	CHECK(a->despawn(eye_id));
	const int id = a->spawn(0, Vector3());
	hits = a->overlap_agents(Vector3(0, 1.9f, 0), 0.11f, 8);
	REQUIRE(hits.size() == 1);
	CHECK((float)((Dictionary)hits[0])["height_frac"] == doctest::Approx(1.0f));
	hits = a->overlap_agents(Vector3(0.5f, 0.9f, 0), 0.16f, 8);
	REQUIRE(hits.size() == 1);
	CHECK((float)((Dictionary)hits[0])["height_frac"] == doctest::Approx(0.5f));
	hits = a->overlap_agents(Vector3(0, -0.1f, 0), 0.11f, 8);
	REQUIRE(hits.size() == 1);
	CHECK((float)((Dictionary)hits[0])["height_frac"] == doctest::Approx(0.0f));
	CHECK(a->despawn(id));

	// Nearest sorting precedes the cap; equal contact distances tie by packed id.
	const int first = a->spawn(0, Vector3(1.0f, 0, 0));
	const int second = a->spawn(0, Vector3(-1.0f, 0, 0));
	const int far = a->spawn(0, Vector3(1.5f, 0, 0));
	hits = a->overlap_agents(Vector3(0, 0.9f, 0), 2.0f, 2);
	REQUIRE(hits.size() == 2);
	CHECK((int)((Dictionary)hits[0])["id"] == MIN(first, second));
	CHECK((int)((Dictionary)hits[1])["id"] == MAX(first, second));

	Array repeated = a->overlap_agents(Vector3(0, 0.9f, 0), 2.0f, 2);
	REQUIRE(repeated.size() == hits.size());
	for (int i = 0; i < hits.size(); i++) {
		const Dictionary lhs = hits[i];
		const Dictionary rhs = repeated[i];
		CHECK((int)lhs["id"] == (int)rhs["id"]);
		CHECK((Vector3)lhs["hit_pos"] == (Vector3)rhs["hit_pos"]);
		CHECK((float)lhs["height_frac"] == (float)rhs["height_frac"]);
	}

	CHECK(a->apply_damage(first, 1000.0f, Vector3(), 0) == HordeAgents::STATE_DEAD);
	hits = a->overlap_agents(Vector3(0, 0.9f, 0), 2.0f, 8);
	for (int i = 0; i < hits.size(); i++) {
		CHECK((int)((Dictionary)hits[i])["id"] != first);
	}
	CHECK(a->despawn(second));
	const int reused = a->spawn(0, Vector3(-1.0f, 0, 0));
	CHECK(reused != second);
	hits = a->overlap_agents(Vector3(0, 0.9f, 0), 2.0f, 8);
	for (int i = 0; i < hits.size(); i++) {
		CHECK((int)((Dictionary)hits[i])["id"] != second);
	}
	CHECK(a->is_alive(far));
}

TEST_CASE("[HordeSim][Combat] overlap_agent_ids matches rich overlap membership and order") {
	Ref<HordeAgents> a = make_agents(12);
	const int first = a->spawn(0, Vector3(0.5f, 0, 0));
	const int second = a->spawn(0, Vector3(-0.5f, 0, 0));
	const int third = a->spawn(0, Vector3(1.5f, 0, 0));
	const int dead = a->spawn(0, Vector3(0, 0, 0.5f));
	CHECK(a->apply_damage(dead, 1000.0f, Vector3(), 0) == HordeAgents::STATE_DEAD);

	for (int cap = 1; cap <= 6; cap++) {
		const Array rich = a->overlap_agents(Vector3(0, 0.9f, 0), 3.0f, cap);
		const PackedInt32Array ids = a->overlap_agent_ids(Vector3(0, 0.9f, 0), 3.0f, cap);
		REQUIRE(ids.size() == rich.size());
		for (int i = 0; i < ids.size(); i++) {
			CHECK(ids[i] == (int)((Dictionary)rich[i])["id"]);
		}
	}

	const PackedInt32Array capped = a->overlap_agent_ids(Vector3(0, 0.9f, 0), 3.0f, 2);
	REQUIRE(capped.size() == 2);
	CHECK(capped[0] == MIN(first, second));
	CHECK(capped[1] == MAX(first, second));
	CHECK(capped.find(third) == -1);
	CHECK(capped.find(dead) == -1);

	CHECK(a->despawn(second));
	const int reused = a->spawn(0, Vector3(-0.5f, 0, 0));
	CHECK(reused != second);
	const PackedInt32Array after_reuse = a->overlap_agent_ids(Vector3(0, 0.9f, 0), 3.0f, 12);
	CHECK(after_reuse.find(second) == -1);
	CHECK(after_reuse.find(reused) >= 0);

	CHECK(a->overlap_agent_ids(Vector3(Math::INF, 0, 0), 3.0f, 12).is_empty());
	CHECK(a->overlap_agent_ids(Vector3(), -0.1f, 12).is_empty());
	CHECK(a->overlap_agent_ids(Vector3(), 3.0f, 0).is_empty());
}

TEST_CASE("[HordeSim][Combat] 250-agent overlap stays within the per-swing budget") {
	Ref<HordeAgents> a = make_agents(250);
	for (int i = 0; i < 250; i++) {
		a->spawn(i & 1, Vector3((float)(i % 25) * 0.6f, 0, (float)(i / 25) * 0.6f), HordeAgents::STATE_ADVANCE);
	}
	for (int warm = 0; warm < 8; warm++) {
		a->overlap_agents(Vector3(7.2f, 0.9f, 2.7f), 10.0f, 250);
		a->overlap_agent_ids(Vector3(7.2f, 0.9f, 2.7f), 10.0f, 250);
	}
	uint64_t best_batch = UINT64_MAX;
	uint64_t best_id_batch = UINT64_MAX;
	for (int trial = 0; trial < 16; trial++) {
		const uint64_t t0 = OS::get_singleton()->get_ticks_usec();
		for (int k = 0; k < 32; k++) {
			a->overlap_agents(Vector3(7.2f, 0.9f, 2.7f), 10.0f, 250);
		}
		best_batch = MIN(best_batch, OS::get_singleton()->get_ticks_usec() - t0);
		const uint64_t id_t0 = OS::get_singleton()->get_ticks_usec();
		for (int k = 0; k < 32; k++) {
			a->overlap_agent_ids(Vector3(7.2f, 0.9f, 2.7f), 10.0f, 250);
		}
		best_id_batch = MIN(best_id_batch, OS::get_singleton()->get_ticks_usec() - id_t0);
	}
	const double per_call = (double)best_batch / 32.0;
	const double id_per_call = (double)best_id_batch / 32.0;
	print_line(vformat("[HordeSim] overlap vs 250 agents: %.2f us/call (min-of-16 x32 batch)", per_call));
	print_line(vformat("[HordeSim] packed-id overlap vs 250 agents: %.2f us/call (min-of-16 x32 batch)", id_per_call));
	// Dev-build pathological ceiling: all 250 capsules overlap the swing and
	// therefore materialize result dictionaries. Paid once per melee impact,
	// never per tick; ordinary daytime encounters return 1-3 candidates.
	CHECK(per_call < 1000.0);
	// The frequent stimulus path marshals one packed buffer, never 250
	// Dictionaries. This deliberately generous dev-build ceiling is tightened
	// only from measured headroom, not release-build intuition.
	CHECK(id_per_call < 500.0);
}

// ---------------------------------------------------------------------------
// Per-agent speed jitter (PLAN_horde_two_field_nav N2): spawn-fixed locomotion
// identity; jitter 0 is byte-identical to today; deterministic across sims.
// ---------------------------------------------------------------------------
TEST_CASE("[HordeSim][Agents] Speed jitter 0 leaves every scale exactly 1.0") {
	Ref<HordeAgents> a = make_agents(64);
	CHECK(a->has_method(StringName("set_speed_jitter")));
	CHECK(a->has_method(StringName("get_agent_speed_scale")));
	CHECK(a->get_speed_jitter() == 0.0f); // Default.

	Vector<int> ids;
	for (int i = 0; i < 32; i++) {
		ids.push_back(a->spawn(i & 1, Vector3(0.5f * i, 0, 0), HordeAgents::STATE_ADVANCE));
	}
	for (int i = 0; i < ids.size(); i++) {
		// Exact 1.0, not approximate: this is the byte-identical guarantee.
		CHECK(a->get_agent_speed_scale(ids[i]) == 1.0f);
	}

	// Setting jitter back to 0 after it was nonzero must restore exact 1.0.
	a->set_speed_jitter(0.2f);
	a->set_speed_jitter(0.0f);
	for (int i = 0; i < ids.size(); i++) {
		CHECK(a->get_agent_speed_scale(ids[i]) == 1.0f);
	}
}

TEST_CASE("[HordeSim][Agents] Speed jitter rolls in range, varies, and is deterministic across sims") {
	auto build = [](float p_jitter) {
		Ref<HordeAgents> a = make_agents(128);
		a->set_speed_jitter(p_jitter);
		for (int i = 0; i < 64; i++) {
			a->spawn(i & 1, Vector3(0.5f * i, 0, 0), HordeAgents::STATE_ADVANCE);
		}
		return a;
	};

	Ref<HordeAgents> a = build(0.2f);
	Ref<HordeAgents> b = build(0.2f);
	PackedInt32Array ids = a->get_active_ids();
	REQUIRE(ids.size() == 64);

	bool all_equal = true;
	const float first = a->get_agent_speed_scale(ids[0]);
	for (int i = 0; i < ids.size(); i++) {
		const float sa = a->get_agent_speed_scale(ids[i]);
		const float sb = b->get_agent_speed_scale(ids[i]);
		// In [1 - jitter, 1 + jitter].
		CHECK(sa >= 0.8f - 1e-6f);
		CHECK(sa <= 1.2f + 1e-6f);
		// Two identically-seeded sims roll bit-identical scales.
		CHECK(sa == sb);
		if (sa != first) {
			all_equal = false;
		}
	}
	CHECK_FALSE(all_equal); // The jitter actually varies agents apart.
}

TEST_CASE("[HordeSim][Agents] Speed jitter re-rolls a recycled slot via the epoch bump") {
	Ref<HordeAgents> a = make_agents(4);
	a->set_speed_jitter(0.3f);
	// Fill every slot, record scales, then despawn and respawn one slot: the
	// epoch bump must change that slot's rolled scale (with overwhelming odds),
	// proving recycle re-rolls the identity for free.
	int first_id = a->spawn(0, Vector3(0, 0, 0), HordeAgents::STATE_ADVANCE);
	const float first_scale = a->get_agent_speed_scale(first_id);
	CHECK(a->despawn(first_id));
	int reused_id = a->spawn(0, Vector3(0, 0, 0), HordeAgents::STATE_ADVANCE);
	CHECK((reused_id & HordeAgents::ID_SLOT_MASK) == (first_id & HordeAgents::ID_SLOT_MASK)); // Same slot.
	CHECK(reused_id != first_id); // Epoch flipped.
	// Different epoch feeds the hash -> a different (still in-range) scale.
	const float reused_scale = a->get_agent_speed_scale(reused_id);
	CHECK(reused_scale >= 0.7f - 1e-6f);
	CHECK(reused_scale <= 1.3f + 1e-6f);
	CHECK(reused_scale != first_scale);
}

TEST_CASE("[HordeSim][Agents] Jitter makes a fast agent outrun a slow one on the same field") {
	// Two agents, same flow field and start row, different rolled speeds -> the
	// faster one leads. Proves the scale reaches locomotion, not just the getter.
	Ref<HordeNavGrid> grid = make_grid(64, 8);
	const Vector3 goal = grid->cell_to_world(60, 3, 0);
	Ref<HordeFlowField> field;
	field.instantiate();
	field->set_grid(grid);
	const Vector3i g = grid->world_to_cell(goal);
	field->add_goal(g.x, g.y, g.z);
	field->recompute_sync();

	Ref<HordeAgents> a = make_agents(256);
	a->set_speed_jitter(0.3f);
	a->set_field(0, field);
	// Scan slots for one clearly-fast and one clearly-slow rolled scale.
	int fast_slot = -1;
	int slow_slot = -1;
	for (int probe = 0; probe < 200; probe++) {
		const Vector3 start = grid->cell_to_world(2, 3, 0);
		const int id = a->spawn(0, start, HordeAgents::STATE_ADVANCE);
		const float s = a->get_agent_speed_scale(id);
		if (s > 1.15f && fast_slot < 0) {
			fast_slot = id;
		} else if (s < 0.85f && slow_slot < 0) {
			slow_slot = id;
		} else {
			a->despawn(id);
		}
		if (fast_slot >= 0 && slow_slot >= 0) {
			break;
		}
	}
	REQUIRE(fast_slot >= 0);
	REQUIRE(slow_slot >= 0);
	PackedVector3Array players;
	players.push_back(grid->cell_to_world(2, 3, 0)); // Keep both Hot every tick.
	a->set_player_positions(players);
	for (int t = 0; t < 40; t++) {
		players.set(0, a->get_agent_position(fast_slot));
		a->set_player_positions(players);
		a->tick(1.0 / 32.0);
	}
	CHECK(a->get_agent_position(fast_slot).x > a->get_agent_position(slow_slot).x);
}

} // namespace TestHordeSim
