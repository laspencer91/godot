/**************************************************************************/
/*  horde_fsm_config.h                                                     */
/**************************************************************************/

#pragma once

#include "core/io/resource.h"
#include "core/templates/local_vector.h"
#include "core/variant/type_info.h"

// Data-driven FSM tuning table for the horde agent sim (DES A2.1-A2.2, G6.11).
//
// One rule per (archetype, state). Rules carry only the parameters the NATIVE
// tick needs to run within-state behavior and to detect that a state-exit
// condition fired this tick: movement speed, and the timer/range thresholds
// that arm a transition. The *decision* of which state to enter next is not
// here -- that is GDScript's job through HordeAgents' batched transition API
// (G6.2). "Chances" (crawler revive, etc.) are likewise transition decisions
// and live in script, not in this table.
//
// Being a Resource, the whole table hot-reloads with D5.9's tuning file: edit
// the .tres, and the next tick reads the new numbers with no recompile.
//
// STATE_COUNT must equal HordeAgents::STATE_MAX; a static_assert in
// horde_agents.cpp binds the two together.
class HordeFSMConfig : public Resource {
	GDCLASS(HordeFSMConfig, Resource);

public:
	// Kept in lockstep with HordeAgents::State (checked in horde_agents.cpp).
	static constexpr int STATE_COUNT = 13;

	// How an agent moves while in a state. Data-driven per (archetype, state) so
	// e.g. the screamer's Advance patrols off the flow field (DES A4.1) while
	// the shambler's Advance rides it -- table data, not a code change.
	enum MovementMode {
		MOVE_STATIONARY, // No positional advance.
		MOVE_FLOW, // Flow-field octant advance (+ link step-through).
		MOVE_SEEK_TARGET, // Steer straight toward the script-set target.
		MOVE_PATROL, // Seeded wander; direction re-rolls when the state timer re-arms.
		MOVE_MODE_MAX,
	};

	struct Rule {
		// Within-state movement speed (m/s). 0 means "stationary in this state".
		float move_speed = 0.0f;
		// Earliest time (s) in the state before an exit condition may arm. The
		// design's telegraph beats (e.g. Wake >= 0.5 s) live here.
		float min_time = 0.0f;
		// Hard state deadline (s); 0 means no timed exit.
		float max_time = 0.0f;
		// Range trigger (m) to nearest player; 0 disables the range exit.
		float exit_range = 0.0f;
		// MovementMode while in this state; configure() seeds per-state defaults.
		uint8_t movement_mode = MOVE_STATIONARY;
	};

	// Per-archetype combat-ingress numbers (P2.4, COMBAT_FEEL sections 3-4).
	// One row per archetype, alongside the per-(archetype, state) rules; the
	// member initializers are the working placeholders (Logan owns the finals,
	// D5.9 hot-reload applies). HordeAgents falls back to DEFAULT_COMBAT when
	// no config Resource is assigned.
	struct CombatRule {
		float max_hp = 100.0f; // Spawn HP when spawn() is not given an override.
		// A single hit at >= this fraction of max_hp staggers a survivor. 0.35
		// encodes "not every hit staggers" (COMBAT_FEEL section 3): the bat (36)
		// staggers a 100 HP shambler while the pistol body hit (34) does not.
		float stagger_damage_frac = 0.35f;
		int stagger_duration_ticks = 90; // ~0.7 s at 128 Hz (A3.6).
		// A heavier surviving hit enters the authored knockdown/get-up ladder.
		// Kept above stagger so pistol body < bat stagger < rifle knockdown.
		float knockdown_damage_frac = 0.50f;
		float knockback_distance_cap = 0.6f; // Blunt-knockback displacement ceiling (m).
		int knockback_duration_ticks = 16; // Knockback decay window (~0.125 s at 128 Hz).
	};

	static const CombatRule DEFAULT_COMBAT;

	// The state's default MovementMode (Advance/Crawl flow, Chase seeks, rest
	// stationary). configure() seeds every archetype row from this, and
	// HordeAgents falls back to it when no config Resource is assigned.
	static MovementMode default_movement_mode(int p_state);

private:
	int archetype_count = 0;
	LocalVector<Rule> rules; // archetype_count * STATE_COUNT, row-major by archetype.
	LocalVector<CombatRule> combat_rules; // One per archetype.

	static void _bind_methods();

	_FORCE_INLINE_ int _rule_index(int p_archetype, int p_state) const {
		return p_archetype * STATE_COUNT + p_state;
	}

public:
	void configure(int p_archetype_count);
	int get_archetype_count() const { return archetype_count; }

	void set_rule(int p_archetype, int p_state, float p_move_speed, float p_min_time, float p_max_time, float p_exit_range);
	void set_movement_mode(int p_archetype, int p_state, MovementMode p_mode);
	void set_combat_rule(int p_archetype, float p_max_hp, float p_stagger_damage_frac, int p_stagger_duration_ticks, float p_knockdown_damage_frac, float p_knockback_distance_cap, int p_knockback_duration_ticks);

	float get_move_speed(int p_archetype, int p_state) const;
	float get_min_time(int p_archetype, int p_state) const;
	float get_max_time(int p_archetype, int p_state) const;
	float get_exit_range(int p_archetype, int p_state) const;
	MovementMode get_movement_mode(int p_archetype, int p_state) const;

	float get_max_hp(int p_archetype) const;
	float get_stagger_damage_frac(int p_archetype) const;
	int get_stagger_duration_ticks(int p_archetype) const;
	float get_knockdown_damage_frac(int p_archetype) const;
	float get_knockback_distance_cap(int p_archetype) const;
	int get_knockback_duration_ticks(int p_archetype) const;

	// Native fast path: rule by reference, no Variant marshaling. Caller must
	// pass in-range indices (HordeAgents clamps before calling).
	_FORCE_INLINE_ const Rule &rule(int p_archetype, int p_state) const {
		return rules[_rule_index(p_archetype, p_state)];
	}
	_FORCE_INLINE_ const CombatRule &combat_rule(int p_archetype) const {
		return combat_rules[p_archetype];
	}

	// Fill placeholder shambler (0) + screamer (1) tables from the design ranges
	// so headless tests and bring-up have a working config. Logan owns the final
	// numbers; these are deliberately coarse (flagged in DECISIONS).
	void load_defaults();

	HordeFSMConfig() {}
};

VARIANT_ENUM_CAST(HordeFSMConfig::MovementMode);
