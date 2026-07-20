/**************************************************************************/
/*  test_horde_agents_multifield.h                                       */
/**************************************************************************/

#pragma once

#include "horde_test_helpers.h"

namespace TestHordeSim {

static Ref<HordeFlowField> make_phase_a_field(const Ref<HordeNavGrid> &p_grid, const Vector3 &p_goal_world) {
	const Vector3i goal = p_grid->world_to_cell(p_goal_world);
	Ref<HordeFlowField> field;
	field.instantiate();
	field->set_grid(p_grid);
	field->add_goal(goal.x, goal.y, goal.z);
	field->recompute_sync();
	return field;
}

static Ref<HordeNavGrid> make_phase_a_domain_grid() {
	Ref<HordeNavGrid> grid = make_grid(48, 24);
	grid->configure(0.5f, Vector2(-6.0f, -6.0f), HordeNavGrid::DEFAULT_FLOOR_HEIGHT);
	return grid;
}

TEST_CASE("[HordeSim][Agents][MultiField] Agents sample their assigned field") {
	Ref<HordeNavGrid> east_grid = make_grid(16, 8);
	Ref<HordeNavGrid> west_grid = make_grid(16, 8);
	const Vector3 start = east_grid->cell_to_world(7, 3, 0);
	Ref<HordeFlowField> east_field = make_phase_a_field(east_grid, east_grid->cell_to_world(14, 3, 0));
	Ref<HordeFlowField> west_field = make_phase_a_field(west_grid, west_grid->cell_to_world(1, 3, 0));

	Ref<HordeAgents> agents = make_agents(2);
	agents->set_flow_field(east_field); // Legacy alias registers field 0.
	agents->set_field(1, west_field);
	CHECK(agents->get_flow_field().ptr() == east_field.ptr());
	CHECK(agents->has_method(StringName("set_field")));
	CHECK(agents->has_method(StringName("clear_fields")));
	CHECK(agents->has_method(StringName("set_field_domain")));
	CHECK(agents->has_method(StringName("get_agent_field_id")));

	const int east_id = agents->spawn(0, start, HordeAgents::STATE_ADVANCE);
	const int west_id = agents->spawn(0, start, HordeAgents::STATE_ADVANCE, -1.0f, 1);
	CHECK(agents->get_agent_field_id(east_id) == 0);
	CHECK(agents->get_agent_field_id(west_id) == 1);

	PackedVector3Array players;
	players.push_back(start); // Both slots run Hot on every tick.
	agents->set_player_positions(players);
	agents->tick(0.25);

	CHECK(agents->get_agent_position(east_id).x > start.x);
	CHECK(agents->get_agent_position(west_id).x < start.x);
}

TEST_CASE("[HordeSim][Agents][MultiField] Apron handoff uses distinct outer-exit and inner-entry edges") {
	Ref<HordeNavGrid> coarse_grid = make_phase_a_domain_grid();
	Ref<HordeNavGrid> fine_grid = make_phase_a_domain_grid();
	Ref<HordeFlowField> coarse_field = make_phase_a_field(coarse_grid, Vector3(6.25f, 0.0f, 0.25f));
	Ref<HordeFlowField> fine_field = make_phase_a_field(fine_grid, Vector3(-4.75f, 0.0f, 0.25f));

	Ref<HordeAgents> agents = make_agents(1);
	agents->set_field(0, coarse_field);
	agents->set_field(1, fine_field);
	// Footprint x=[4,8), outer apron x=[2,10). Fine steers left; coarse
	// steers right, producing a complete out-and-back crossing without a test
	// position setter.
	agents->set_field_domain(1, Rect2(Vector2(4.0f, -1.0f), Vector2(4.0f, 2.0f)), 2.0f);
	const Vector3 start(6.25f, 0.0f, 0.25f);
	const int id = agents->spawn(0, start, HordeAgents::STATE_ADVANCE, -1.0f, 1);
	PackedVector3Array players;
	players.push_back(start);
	agents->set_player_positions(players);

	int previous_field = 1;
	int flip_count = 0;
	float outer_exit_x = 100.0f;
	float inner_entry_x = -100.0f;
	bool saw_fine_in_mid_apron = false;
	bool saw_coarse_in_mid_apron = false;
	for (int tick = 0; tick < 100 && flip_count < 2; tick++) {
		agents->tick(0.125);
		const float x = agents->get_agent_position(id).x;
		const int current_field = agents->get_agent_field_id(id);
		if (flip_count == 0 && x > 2.0f && x < 4.0f) {
			saw_fine_in_mid_apron = true;
			CHECK(current_field == 1);
		}
		if (flip_count == 1 && x > 2.0f && x < 4.0f) {
			saw_coarse_in_mid_apron = true;
			CHECK(current_field == 0);
		}
		if (current_field != previous_field) {
			flip_count++;
			if (current_field == 0) {
				outer_exit_x = x;
			} else {
				inner_entry_x = x;
			}
			previous_field = current_field;
		}
	}

	CHECK(saw_fine_in_mid_apron);
	CHECK(saw_coarse_in_mid_apron);
	CHECK(flip_count == 2);
	CHECK(outer_exit_x < 2.0f);
	CHECK(inner_entry_x >= 4.0f);
	CHECK(inner_entry_x < 8.0f);
}

TEST_CASE("[HordeSim][Agents][MultiField] Field switch clears the old flow sample and resamples next tick") {
	Ref<HordeNavGrid> coarse_grid = make_phase_a_domain_grid();
	Ref<HordeNavGrid> fine_grid = make_phase_a_domain_grid();
	const Vector3 outside(-2.75f, 0.0f, 0.25f);
	Ref<HordeFlowField> coarse_field = make_phase_a_field(coarse_grid, Vector3(2.25f, 0.0f, 0.25f));
	// The fine field reports GOAL at the spawn. Handoff must clear that cached
	// octant or _evaluate_exits() would incorrectly emit REASON_AT_GOAL.
	Ref<HordeFlowField> fine_field = make_phase_a_field(fine_grid, outside);

	Ref<HordeAgents> agents = make_agents(1);
	agents->set_field(0, coarse_field);
	agents->set_field(1, fine_field);
	agents->set_field_domain(1, Rect2(Vector2(0.0f, -1.0f), Vector2(2.0f, 2.0f)), 1.0f);
	const int id = agents->spawn(0, outside, HordeAgents::STATE_ADVANCE, -1.0f, 1);
	PackedVector3Array players;
	players.push_back(outside);
	agents->set_player_positions(players);

	agents->tick(0.125);
	CHECK(agents->get_agent_field_id(id) == 0);
	CHECK(agents->query_transitions().is_empty());
	CHECK(agents->get_agent_position(id) == outside);

	agents->tick(0.125);
	CHECK(agents->get_agent_position(id).x > outside.x);
}

TEST_CASE("[HordeSim][Agents][MultiField] Overlapping domains choose the lowest field id") {
	Ref<HordeNavGrid> grid = make_phase_a_domain_grid();
	Ref<HordeFlowField> field = make_phase_a_field(grid, Vector3(5.25f, 0.0f, 0.25f));
	Ref<HordeAgents> agents = make_agents(1);
	agents->set_field(0, field);
	agents->set_field(7, field); // Register out of order and leave holes.
	agents->set_field(2, field);
	const Rect2 footprint(Vector2(4.0f, -1.0f), Vector2(4.0f, 2.0f));
	agents->set_field_domain(7, footprint);
	agents->set_field_domain(2, footprint);
	const Vector3 start(5.25f, 0.0f, 0.25f);
	const int id = agents->spawn(0, start, HordeAgents::STATE_ADVANCE);
	PackedVector3Array players;
	players.push_back(start);
	agents->set_player_positions(players);

	agents->tick(0.125);
	CHECK(agents->get_agent_field_id(id) == 2);
}

TEST_CASE("[HordeSim][Agents][MultiField] Runtime set_agent_field switches the followed field next tick") {
	Ref<HordeNavGrid> east_grid = make_grid(16, 8);
	Ref<HordeNavGrid> west_grid = make_grid(16, 8);
	const Vector3 start = east_grid->cell_to_world(7, 3, 0);
	Ref<HordeFlowField> east_field = make_phase_a_field(east_grid, east_grid->cell_to_world(14, 3, 0));
	Ref<HordeFlowField> west_field = make_phase_a_field(west_grid, west_grid->cell_to_world(1, 3, 0));

	Ref<HordeAgents> agents = make_agents(2);
	agents->set_field(0, east_field);
	agents->set_field(1, west_field);
	CHECK(agents->has_method(StringName("set_agent_field")));
	CHECK(agents->has_method(StringName("set_agent_fields")));

	const int id = agents->spawn(0, start, HordeAgents::STATE_ADVANCE); // Field 0 by default.
	PackedVector3Array players;
	players.push_back(start);
	agents->set_player_positions(players);

	agents->tick(0.25);
	const float x_after_east = agents->get_agent_position(id).x;
	CHECK(x_after_east > start.x); // Followed field 0 (east goal) -> +x.

	// Reassign to field 1 mid-life; the west goal pulls -x on the next tick.
	CHECK(agents->set_agent_field(id, 1));
	CHECK(agents->get_agent_field_id(id) == 1);
	agents->tick(0.25);
	CHECK(agents->get_agent_position(id).x < x_after_east); // Now steering west.

	// Invalid id and out-of-range field are rejected without mutating state.
	CHECK_FALSE(agents->set_agent_field(0x3FF, 0)); // Never-spawned slot id.
	CHECK_FALSE(agents->set_agent_field(id, -1));
	CHECK_FALSE(agents->set_agent_field(id, HordeAgents::MAX_FIELDS));
	CHECK(agents->get_agent_field_id(id) == 1); // Unchanged by the rejected calls.
}

TEST_CASE("[HordeSim][Agents][MultiField] set_agent_field clears the stale AT_GOAL octant") {
	Ref<HordeNavGrid> grid0 = make_phase_a_domain_grid();
	Ref<HordeNavGrid> grid1 = make_phase_a_domain_grid();
	const Vector3 spot(0.25f, 0.0f, 0.25f);
	// Field 0 steers away (+x). Field 1 reports GOAL exactly at the spawn -- a
	// switch to it must NOT arm REASON_AT_GOAL from the old (field 0) sample, and
	// must resample GOAL for field 1 so the agent settles rather than advancing.
	Ref<HordeFlowField> field0 = make_phase_a_field(grid0, Vector3(4.25f, 0.0f, 0.25f));
	Ref<HordeFlowField> field1 = make_phase_a_field(grid1, spot);

	Ref<HordeAgents> agents = make_agents(1);
	agents->set_field(0, field0);
	agents->set_field(1, field1);
	const int id = agents->spawn(0, spot, HordeAgents::STATE_ADVANCE);
	PackedVector3Array players;
	players.push_back(spot);
	agents->set_player_positions(players);

	agents->tick(0.125);
	CHECK(agents->get_agent_position(id).x > spot.x); // Field 0 advanced it.
	CHECK(agents->query_transitions().is_empty()); // Field 0 goal is far; no AT_GOAL.

	CHECK(agents->set_agent_field(id, 1));
	// The very next tick the cached octant is gone: the agent resamples field 1
	// (GOAL at its cell) and settles onto the cell center instead of arming a
	// stale exit or advancing off the old field.
	agents->tick(0.125);
	const int f1_state = agents->get_agent_state(id);
	CHECK(f1_state == HordeAgents::STATE_ADVANCE); // No spurious transition applied.
}

TEST_CASE("[HordeSim][Agents][MultiField] Batched set_agent_fields reassigns many agents and skips bad pairs") {
	Ref<HordeNavGrid> east_grid = make_grid(16, 8);
	Ref<HordeNavGrid> west_grid = make_grid(16, 8);
	const Vector3 start = east_grid->cell_to_world(7, 3, 0);
	Ref<HordeFlowField> east_field = make_phase_a_field(east_grid, east_grid->cell_to_world(14, 3, 0));
	Ref<HordeFlowField> west_field = make_phase_a_field(west_grid, west_grid->cell_to_world(1, 3, 0));

	Ref<HordeAgents> agents = make_agents(4);
	agents->set_field(0, east_field);
	agents->set_field(1, west_field);
	const int a = agents->spawn(0, start, HordeAgents::STATE_ADVANCE);
	const int b = agents->spawn(0, start, HordeAgents::STATE_ADVANCE);
	const int c = agents->spawn(0, start, HordeAgents::STATE_ADVANCE);

	// Move a and c to field 1; leave b on field 0; include a bad field (skipped)
	// and a stale id (skipped) to prove the batch tolerates junk entries.
	PackedInt32Array pairs;
	pairs.push_back(a);
	pairs.push_back(1);
	pairs.push_back(c);
	pairs.push_back(1);
	pairs.push_back(b);
	pairs.push_back(HordeAgents::MAX_FIELDS + 5); // Out of range -> skipped.
	pairs.push_back(0x3FF); // Never spawned -> skipped.
	pairs.push_back(1);
	agents->set_agent_fields(pairs);

	CHECK(agents->get_agent_field_id(a) == 1);
	CHECK(agents->get_agent_field_id(c) == 1);
	CHECK(agents->get_agent_field_id(b) == 0); // The bad-field pair left it alone.

	PackedVector3Array players;
	players.push_back(start);
	agents->set_player_positions(players);
	agents->tick(0.25);
	CHECK(agents->get_agent_position(a).x < start.x); // West.
	CHECK(agents->get_agent_position(c).x < start.x); // West.
	CHECK(agents->get_agent_position(b).x > start.x); // East.
}

TEST_CASE("[HordeSim][Agents][MultiField] 250-agent eight-domain handoff stays within the dev budget") {
	Ref<HordeNavGrid> grid = make_grid(64, 64);
	Ref<HordeFlowField> field = make_phase_a_field(grid, grid->cell_to_world(63, 63, 0));

	auto make_benchmark = [&](bool p_with_domains) {
		Ref<HordeAgents> agents = make_agents(250);
		agents->set_hot_distance(100.0f);
		agents->set_warm_distance(200.0f);
		agents->set_separation_radius(0.1f);
		agents->set_separation_strength(0.0f);
		for (int field_id = 0; field_id <= 8; field_id++) {
			agents->set_field(field_id, field);
			if (p_with_domains && field_id > 0) {
				// Every coarse agent reaches the circle prefilter for all eight
				// domains, then rejects without an AABB test.
				agents->set_field_domain(field_id,
						Rect2(Vector2(100.0f + 10.0f * field_id, 100.0f), Vector2(4.0f, 4.0f)));
			}
		}
		for (int i = 0; i < 250; i++) {
			const Vector3 position(0.25f + 0.5f * (float)(i % 25), 0.0f, 0.25f + 0.5f * (float)(i / 25));
			agents->spawn(i & 1, position, HordeAgents::STATE_ADVANCE);
		}
		PackedVector3Array players;
		players.push_back(Vector3());
		agents->set_player_positions(players);
		return agents;
	};

	Ref<HordeAgents> baseline = make_benchmark(false);
	Ref<HordeAgents> with_domains = make_benchmark(true);
	for (int i = 0; i < 8; i++) {
		baseline->tick(1.0 / 128.0);
		with_domains->tick(1.0 / 128.0);
	}

	uint64_t baseline_best = UINT64_MAX;
	uint64_t domains_best = UINT64_MAX;
	for (int i = 0; i < 24; i++) {
		// Alternate order to avoid consistently favoring the second hot-cache run.
		if ((i & 1) == 0) {
			baseline->tick(1.0 / 128.0);
			with_domains->tick(1.0 / 128.0);
		} else {
			with_domains->tick(1.0 / 128.0);
			baseline->tick(1.0 / 128.0);
		}
		baseline_best = MIN(baseline_best, baseline->get_tick_time_usec());
		domains_best = MIN(domains_best, with_domains->get_tick_time_usec());
	}
	const uint64_t handoff_usec = domains_best > baseline_best ? domains_best - baseline_best : 0;
	print_line(vformat("[HordeSim] 250-agent / 8-domain handoff: %d us incremental (%d us total, %d us baseline)",
			(int64_t)handoff_usec, (int64_t)domains_best, (int64_t)baseline_best));

	// Debug/dev ceiling intentionally leaves ample room for shared CI hosts.
	CHECK(handoff_usec < 4000);
	CHECK(domains_best < 12000);
}

} // namespace TestHordeSim
