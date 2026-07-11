/**************************************************************************/
/*  horde_fsm_config.cpp                                                    */
/**************************************************************************/

#include "horde_fsm_config.h"

#include "core/object/class_db.h"

HordeFSMConfig::MovementMode HordeFSMConfig::default_movement_mode(int p_state) {
	// Indices mirror HordeAgents::State (lockstep asserted in horde_agents.cpp).
	static const MovementMode DEFAULTS[STATE_COUNT] = {
		MOVE_STATIONARY, // Dormant
		MOVE_STATIONARY, // Wake
		MOVE_FLOW, // Advance
		MOVE_SEEK_TARGET, // Chase
		MOVE_STATIONARY, // AttackPlayer
		MOVE_STATIONARY, // Grab
		MOVE_STATIONARY, // AttackObstacle
		MOVE_STATIONARY, // Stagger
		MOVE_STATIONARY, // Scream
		MOVE_FLOW, // Crawl
		MOVE_STATIONARY, // Dead
	};
	ERR_FAIL_INDEX_V(p_state, STATE_COUNT, MOVE_STATIONARY);
	return DEFAULTS[p_state];
}

void HordeFSMConfig::configure(int p_archetype_count) {
	ERR_FAIL_COND(p_archetype_count < 0);
	archetype_count = p_archetype_count;
	rules.clear();
	rules.resize((uint32_t)(archetype_count * STATE_COUNT));
	for (uint32_t i = 0; i < rules.size(); i++) {
		rules[i] = Rule();
		// Seed the state's default mode so a table that only tunes numbers keeps
		// sane movement; archetypes that deviate override per (archetype, state).
		rules[i].movement_mode = (uint8_t)default_movement_mode((int)(i % STATE_COUNT));
	}
}

void HordeFSMConfig::set_movement_mode(int p_archetype, int p_state, MovementMode p_mode) {
	ERR_FAIL_INDEX(p_archetype, archetype_count);
	ERR_FAIL_INDEX(p_state, STATE_COUNT);
	ERR_FAIL_INDEX(p_mode, MOVE_MODE_MAX);
	rules[_rule_index(p_archetype, p_state)].movement_mode = (uint8_t)p_mode;
}

HordeFSMConfig::MovementMode HordeFSMConfig::get_movement_mode(int p_archetype, int p_state) const {
	ERR_FAIL_INDEX_V(p_archetype, archetype_count, MOVE_STATIONARY);
	ERR_FAIL_INDEX_V(p_state, STATE_COUNT, MOVE_STATIONARY);
	return (MovementMode)rules[_rule_index(p_archetype, p_state)].movement_mode;
}

void HordeFSMConfig::set_rule(int p_archetype, int p_state, float p_move_speed, float p_min_time, float p_max_time, float p_exit_range) {
	ERR_FAIL_INDEX(p_archetype, archetype_count);
	ERR_FAIL_INDEX(p_state, STATE_COUNT);
	Rule &r = rules[_rule_index(p_archetype, p_state)];
	r.move_speed = p_move_speed;
	r.min_time = p_min_time;
	r.max_time = p_max_time;
	r.exit_range = p_exit_range;
}

float HordeFSMConfig::get_move_speed(int p_archetype, int p_state) const {
	ERR_FAIL_INDEX_V(p_archetype, archetype_count, 0.0f);
	ERR_FAIL_INDEX_V(p_state, STATE_COUNT, 0.0f);
	return rules[_rule_index(p_archetype, p_state)].move_speed;
}

float HordeFSMConfig::get_min_time(int p_archetype, int p_state) const {
	ERR_FAIL_INDEX_V(p_archetype, archetype_count, 0.0f);
	ERR_FAIL_INDEX_V(p_state, STATE_COUNT, 0.0f);
	return rules[_rule_index(p_archetype, p_state)].min_time;
}

float HordeFSMConfig::get_max_time(int p_archetype, int p_state) const {
	ERR_FAIL_INDEX_V(p_archetype, archetype_count, 0.0f);
	ERR_FAIL_INDEX_V(p_state, STATE_COUNT, 0.0f);
	return rules[_rule_index(p_archetype, p_state)].max_time;
}

float HordeFSMConfig::get_exit_range(int p_archetype, int p_state) const {
	ERR_FAIL_INDEX_V(p_archetype, archetype_count, 0.0f);
	ERR_FAIL_INDEX_V(p_state, STATE_COUNT, 0.0f);
	return rules[_rule_index(p_archetype, p_state)].exit_range;
}

void HordeFSMConfig::load_defaults() {
	// State indices mirror HordeAgents::State.
	enum {
		S_DORMANT,
		S_WAKE,
		S_ADVANCE,
		S_CHASE,
		S_ATTACK_PLAYER,
		S_GRAB,
		S_ATTACK_OBSTACLE,
		S_STAGGER,
		S_SCREAM,
		S_CRAWL,
		S_DEAD,
	};

	configure(2);

	// --- Archetype 0: Shambler (design section 3). Placeholder tuning. ---
	// Dormant until woken (A3.2); wake beat is a 0.5-1.0 s telegraph (A3.3).
	set_rule(0, S_DORMANT, 0.0f, 0.0f, 0.0f, 8.0f); // Wake when a player closes to 8 m.
	set_rule(0, S_WAKE, 0.0f, 0.6f, 0.9f, 0.0f); // Telegraph: exit after 0.6-0.9 s.
	set_rule(0, S_ADVANCE, 1.2f, 0.0f, 0.0f, 3.0f); // Advance on flow field; chase within 3 m.
	set_rule(0, S_CHASE, 2.4f, 0.0f, 0.0f, 1.2f); // Chase target; grab/attack within 1.2 m.
	set_rule(0, S_ATTACK_PLAYER, 0.0f, 0.0f, 0.8f, 0.0f);
	set_rule(0, S_GRAB, 0.0f, 1.5f, 1.5f, 0.0f); // 1.5 s hold (A3.8).
	set_rule(0, S_ATTACK_OBSTACLE, 0.0f, 0.0f, 1.0f, 0.0f);
	set_rule(0, S_STAGGER, 0.0f, 0.7f, 0.7f, 0.0f); // ~0.7 s stagger (A3.6).
	set_rule(0, S_SCREAM, 0.0f, 0.0f, 0.0f, 0.0f); // Shamblers do not scream.
	set_rule(0, S_CRAWL, 0.6f, 0.0f, 0.0f, 1.0f); // Low, slow crawler (A3.7).
	set_rule(0, S_DEAD, 0.0f, 0.0f, 0.0f, 0.0f);

	// --- Archetype 1: Screamer (design section 4). Placeholder tuning. ---
	set_rule(1, S_DORMANT, 0.0f, 0.0f, 0.0f, 12.0f); // Detects at longer range.
	set_rule(1, S_WAKE, 0.0f, 0.3f, 0.5f, 0.0f);
	set_rule(1, S_ADVANCE, 1.0f, 0.0f, 0.0f, 20.0f); // Wanders/patrols; screams within 20 m.
	// Screamers patrol OFF the flow field -- unpredictability is the point (A4.1).
	set_movement_mode(1, S_ADVANCE, MOVE_PATROL);
	set_rule(1, S_CHASE, 1.4f, 0.0f, 0.0f, 6.0f);
	set_rule(1, S_ATTACK_PLAYER, 0.0f, 0.0f, 0.8f, 0.0f);
	set_rule(1, S_GRAB, 0.0f, 1.5f, 1.5f, 0.0f);
	set_rule(1, S_ATTACK_OBSTACLE, 0.0f, 0.0f, 1.0f, 0.0f);
	set_rule(1, S_STAGGER, 0.0f, 0.7f, 0.7f, 0.0f);
	set_rule(1, S_SCREAM, 0.0f, 2.0f, 2.0f, 0.0f); // 2.0 s wind-up (A4.3).
	set_rule(1, S_CRAWL, 0.6f, 0.0f, 0.0f, 1.0f);
	set_rule(1, S_DEAD, 0.0f, 0.0f, 0.0f, 0.0f);
}

void HordeFSMConfig::_bind_methods() {
	ClassDB::bind_method(D_METHOD("configure", "archetype_count"), &HordeFSMConfig::configure);
	ClassDB::bind_method(D_METHOD("get_archetype_count"), &HordeFSMConfig::get_archetype_count);
	ClassDB::bind_method(D_METHOD("set_rule", "archetype", "state", "move_speed", "min_time", "max_time", "exit_range"), &HordeFSMConfig::set_rule);
	ClassDB::bind_method(D_METHOD("get_move_speed", "archetype", "state"), &HordeFSMConfig::get_move_speed);
	ClassDB::bind_method(D_METHOD("get_min_time", "archetype", "state"), &HordeFSMConfig::get_min_time);
	ClassDB::bind_method(D_METHOD("get_max_time", "archetype", "state"), &HordeFSMConfig::get_max_time);
	ClassDB::bind_method(D_METHOD("get_exit_range", "archetype", "state"), &HordeFSMConfig::get_exit_range);
	ClassDB::bind_method(D_METHOD("set_movement_mode", "archetype", "state", "mode"), &HordeFSMConfig::set_movement_mode);
	ClassDB::bind_method(D_METHOD("get_movement_mode", "archetype", "state"), &HordeFSMConfig::get_movement_mode);
	ClassDB::bind_method(D_METHOD("load_defaults"), &HordeFSMConfig::load_defaults);

	BIND_ENUM_CONSTANT(MOVE_STATIONARY);
	BIND_ENUM_CONSTANT(MOVE_FLOW);
	BIND_ENUM_CONSTANT(MOVE_SEEK_TARGET);
	BIND_ENUM_CONSTANT(MOVE_PATROL);
}
