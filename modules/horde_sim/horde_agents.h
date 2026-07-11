/**************************************************************************/
/*  horde_agents.h                                                          */
/**************************************************************************/

#pragma once

#include "horde_flow_field.h"
#include "horde_fsm_config.h"
#include "horde_nav_grid.h"

#include "core/math/aabb.h"
#include "core/object/ref_counted.h"
#include "core/templates/local_vector.h"
#include "core/variant/type_info.h"

// Struct-of-arrays horde agent simulation (DES G6.9, A2.x, R3.1-R3.4).
//
// One flat SoA store, no per-agent heap objects, no virtual dispatch in the
// tick loop. The host calls tick(delta) once per fixed tick; the sim assigns
// LOD tiers by distance to the nearest player, runs within-state behavior
// (movement, timers) natively, and reports agents whose state-exit conditions
// fired so GDScript can drive the actual transition decisions (G6.2).
//
// DETERMINISM (CONVENTIONS L6): tick() is a pure function of (delta, current
// SoA state, player positions, published flow field). No wall-clock, no
// randf(); iteration order is stable (ascending slot). Same seed + same inputs
// => bit-identical SoA after N ticks.
//
// COLLISION QUERIES: wall/static contact uses Box3D's b3World_CastMover -- its
// purpose-built kinematic capsule sweep -- called natively against the b3World
// resolved once from the physics space RID (D-009). See set_physics_space().
class HordeAgents : public RefCounted {
	GDCLASS(HordeAgents, RefCounted);

public:
	// Shared FSM state set (DES A2.2). Kept in lockstep with
	// HordeFSMConfig::STATE_COUNT (static_assert in the .cpp).
	enum State {
		STATE_DORMANT,
		STATE_WAKE,
		STATE_ADVANCE,
		STATE_CHASE,
		STATE_ATTACK_PLAYER,
		STATE_GRAB,
		STATE_ATTACK_OBSTACLE,
		STATE_STAGGER,
		STATE_SCREAM,
		STATE_CRAWL,
		STATE_DEAD,
		STATE_MAX,
	};

	// LOD brain tiers (R3.3, A2.3).
	enum Tier {
		TIER_HOT, // < hot_distance: full rate, separation, attack hooks.
		TIER_WARM, // hot..warm: steering only, 1/2 rate.
		TIER_COLD, // > warm_distance: flow-field advance at 1/4 rate, no agent-vs-agent.
		TIER_MAX,
	};

	// Why an agent's state-exit condition armed this tick (reported to script).
	enum TransitionReason {
		REASON_NONE,
		REASON_TIMER, // Hit the state's max_time deadline.
		REASON_IN_RANGE, // Nearest player entered the state's exit_range.
		REASON_AT_GOAL, // Advancing agent reached its flow-field goal cell.
		// The most recent movement sweep (Hot/Warm only) was clamped by an
		// obstacle contact (fraction < 0.999, see _sweep_fraction/_move_agent).
		// Gated by the state's min_time like REASON_IN_RANGE. Cold agents never
		// arm this: Cold advances without a mover sweep (board-attack contact
		// only matters at Hot/Warm range).
		REASON_BLOCKED,
	};

	// Id space (NET R3.5): 10-bit slot + a reuse-epoch parity bit. A packed id
	// is slot | (epoch_parity << 10). A stale id (freed then slot reused) fails
	// validation on the epoch bit. Wire format is P2.1; only alloc/free is here.
	static constexpr int ID_SLOT_BITS = 10;
	static constexpr int HARD_CAP = 1 << ID_SLOT_BITS; // 1024 concurrent ids.
	static constexpr int ID_SLOT_MASK = HARD_CAP - 1;
	static constexpr int ID_EPOCH_BIT = HARD_CAP;

private:
	// --- Config ---
	int capacity = 250; // Alive cap (R3.4). Rebuilds storage on change.
	float hot_distance = 25.0f; // R3.3 tier thresholds (planar, m).
	float warm_distance = 60.0f;
	float agent_radius = 0.35f;
	float agent_height = 1.8f;
	float separation_radius = 0.9f; // Reynolds neighborhood (Hot only).
	float separation_strength = 1.6f;
	uint32_t collision_mask = 0xFFFFFFFF; // Static layers the mover sweep tests.

	Ref<HordeFlowField> flow_field;
	Ref<HordeFSMConfig> fsm_config;

	// Box3D world handle, packed via b3StoreWorldId (kept box3d-free in this
	// header). 0 == no collision world (movement then skips wall queries).
	uint32_t collision_world_id = 0;

	// --- SoA storage (all sized `capacity`) ---
	LocalVector<float> pos_x;
	LocalVector<float> pos_y;
	LocalVector<float> pos_z;
	LocalVector<float> yaw;
	LocalVector<float> target_x; // Chase target (script-set).
	LocalVector<float> target_z;
	LocalVector<float> state_timer;
	// Squared planar distance to the NEAREST-OF-ANY player; sqrt lazily at
	// consumers. SIM-LOD TIERING ONLY (drives _assign_tiers()/tier below and
	// the IN_RANGE exit check) -- this is NOT per-client wire relevance. R3.7
	// per-client send rates must compute their own player-relative distances
	// (P2.1); do not repurpose this field for that.
	LocalVector<float> nearest_any_dist_sq;
	LocalVector<int32_t> hp;
	LocalVector<uint16_t> epoch; // Reuse epoch per slot (low bit is the wire parity).
	LocalVector<uint16_t> wander; // PATROL re-roll counter; bumps on every applied transition.
	LocalVector<uint8_t> state;
	// LOD brain tier (Hot/Warm/Cold), assigned from nearest_any_dist_sq. SIM-LOD
	// TIERING ONLY -- NOT per-client wire relevance. R3.7 send rates are
	// per-client and must compute their own distances (P2.1); never gate wire
	// send decisions on this field.
	LocalVector<uint8_t> tier;
	LocalVector<uint8_t> archetype;
	LocalVector<uint8_t> active; // 1 if slot holds a live agent.
	LocalVector<uint8_t> pending_reason; // TransitionReason armed this tick.
	// 1 if this slot's most recent movement sweep (Hot/Warm only) was clamped
	// by an obstacle; refreshed every time _move_agent() runs the slot (same
	// staggered-LOD cadence as flow_octant below), consumed by
	// _evaluate_exits() to arm REASON_BLOCKED. Always 0 for Cold (never swept).
	LocalVector<uint8_t> blocked_contact;
	// Flow octant sampled at the agent's last movement step (OCTANT_NONE when the
	// state isn't flow-driven). Exit evaluation reads this instead of re-sampling
	// world_to_cell + octant. Staleness: a stagger-gated Warm/Cold agent keeps its
	// previous sample for up to 4 ticks -- acceptable for AT_GOAL arming. Ticked
	// state, seeded identically on spawn/transition, so determinism holds.
	LocalVector<uint8_t> flow_octant;

	LocalVector<int32_t> free_slots; // Stack of available slots.
	int active_count = 0;

	// Player positions (planar) fed per tick (R3.3).
	LocalVector<float> player_x;
	LocalVector<float> player_z;

	// Hot-tier spatial hash: open-addressed flat table (cell key -> chain head)
	// over reused LocalVectors, plus the per-slot next link. Entries invalidate
	// by comparing a per-bucket generation stamp against the tick's generation,
	// so a rebuild is a counter bump -- zero per-tick heap traffic (the old
	// HashMap memnew'd/memdelete'd one node per hot agent per tick).
	LocalVector<int64_t> hash_key;
	LocalVector<int32_t> hash_head;
	LocalVector<uint32_t> hash_gen;
	uint32_t hash_generation = 0;
	uint32_t hash_mask = 0; // Table size (power of two) minus one.
	LocalVector<int32_t> spatial_next;

	uint64_t tick_counter = 0;

	// Reused output buffer for the GDScript-facing pack_interest_snapshot()
	// (returning wrapper). Grows to the largest snapshot seen; the native
	// pack_snapshot_into() core takes a caller-owned buffer instead (zero-alloc).
	PackedByteArray pack_scratch;

	// --- Metrics (last tick) ---
	uint64_t tick_usec = 0;
	uint64_t pack_usec = 0; // Last pack_snapshot_into() walk (R8.1).
	uint64_t tier_usec[TIER_MAX] = { 0, 0, 0 };
	int tier_count[TIER_MAX] = { 0, 0, 0 };

	static void _bind_methods();

	_FORCE_INLINE_ int _make_id(int p_slot) const {
		return p_slot | ((int)(epoch[p_slot] & 1) << ID_SLOT_BITS);
	}
	// Resolve a packed id to a live slot; false if freed/stale/out-of-range.
	bool _resolve(int p_id, int &r_slot) const;

	void _rebuild_storage();
	void _init_slot(int p_slot, int p_archetype, const Vector3 &p_pos, int p_state, int p_hp);

	// Per-tick stages.
	void _assign_tiers();
	void _build_hot_spatial_hash();
	void _step_movement(double p_delta);
	void _move_agent(int p_slot, double p_delta, HordeNavGrid *p_grid, bool p_have_field);
	void _evaluate_exits();

	// Movement helpers (hot path).
	Vector2 _separation_offset(int p_slot) const;
	float _state_speed(int p_archetype, int p_state) const;
	int _movement_mode(int p_archetype, int p_state) const;
	float _wander_angle(int p_slot) const;
	float _sweep_fraction(int p_slot, const Vector2 &p_disp) const;
	// Grid of the published flow field, or null when no field is available.
	HordeNavGrid *_resolve_field() const;
	int32_t _hash_lookup(int64_t p_key) const;

	_FORCE_INLINE_ int _tier_divisor(int p_tier) const {
		return p_tier == TIER_HOT ? 1 : (p_tier == TIER_WARM ? 2 : 4);
	}

public:
	// --- Config ---
	void set_capacity(int p_capacity);
	int get_capacity() const { return capacity; }
	void set_hot_distance(float p_d) { hot_distance = p_d; }
	float get_hot_distance() const { return hot_distance; }
	void set_warm_distance(float p_d) { warm_distance = p_d; }
	float get_warm_distance() const { return warm_distance; }
	void set_agent_radius(float p_r) { agent_radius = p_r; }
	float get_agent_radius() const { return agent_radius; }
	void set_agent_height(float p_h) { agent_height = p_h; }
	float get_agent_height() const { return agent_height; }
	void set_separation_radius(float p_r) { separation_radius = p_r; }
	float get_separation_radius() const { return separation_radius; }
	void set_separation_strength(float p_s) { separation_strength = p_s; }
	float get_separation_strength() const { return separation_strength; }
	void set_collision_mask(int p_mask) { collision_mask = (uint32_t)p_mask; }
	int get_collision_mask() const { return (int)collision_mask; }

	void set_flow_field(const Ref<HordeFlowField> &p_field) { flow_field = p_field; }
	Ref<HordeFlowField> get_flow_field() const { return flow_field; }
	void set_fsm_config(const Ref<HordeFSMConfig> &p_config) { fsm_config = p_config; }
	Ref<HordeFSMConfig> get_fsm_config() const { return fsm_config; }

	// Resolve a Godot physics space RID to the underlying Box3D b3World and keep
	// its packed handle for native mover queries. No-op (queries disabled) if the
	// active physics server is not Box3D. Call once after the space exists.
	void set_physics_space(RID p_space);
	void clear_physics_space();
	bool has_physics_space() const { return collision_world_id != 0; }

	// Native-only injection of a raw b3World handle (packed via b3StoreWorldId).
	// Used by headless tests that build their own b3World; not bound to script.
	void set_collision_world_packed(uint32_t p_packed);

	// --- Spawn / despawn / recycle (NET R3.4-R3.5) ---
	int spawn(int p_archetype, const Vector3 &p_position, int p_state = STATE_DORMANT, int p_hp = 100);
	bool despawn(int p_id);
	bool is_alive(int p_id) const;
	int get_active_count() const { return active_count; }
	void clear();

	// Rank live Cold agents furthest-first so the Director can recycle them to
	// feed spawns near the action (D5.8). Native ranks; policy stays in script.
	PackedInt32Array recycle_candidates(int p_max) const;

	// --- Per-tick ---
	void set_player_positions(const PackedVector3Array &p_positions);
	void tick(double p_delta);

	// --- Batched FSM transition API (G6.2) ---
	// Flat quads [id, archetype, state, reason, ...] for every agent whose exit
	// condition armed this tick. Script maps (archetype, state, reason) -> next
	// state directly from the quad (no per-agent archetype lookup), then applies
	// via apply_transitions() with flat pairs [id, new_state, ...].
	PackedInt32Array query_transitions() const;
	bool apply_transition(int p_id, int p_new_state);
	void apply_transitions(const PackedInt32Array &p_pairs);

	// --- Agent access (script + presentation, read-mostly) ---
	int get_agent_state(int p_id) const;
	int get_agent_tier(int p_id) const;
	int get_agent_archetype(int p_id) const;
	Vector3 get_agent_position(int p_id) const;
	float get_agent_yaw(int p_id) const;
	int get_agent_hp(int p_id) const;
	float get_nearest_player_distance(int p_id) const;
	void set_agent_hp(int p_id, int p_hp);
	void set_agent_target(int p_id, const Vector3 &p_target);

	// Bulk snapshots for MultiMesh presentation (P1.B3). All aligned to
	// get_active_ids() order. The fill_* variants write into a caller-owned
	// buffer, resizing only when the active count changed, so the per-frame
	// render path allocates nothing; they are native-only (reference out-params
	// don't round-trip through Variant). Script uses the returning variants.
	void fill_active_ids(PackedInt32Array &r_out) const;
	void fill_positions(PackedVector3Array &r_out) const;
	void fill_yaws(PackedFloat32Array &r_out) const;
	void fill_states(PackedByteArray &r_out) const;
	void fill_tiers(PackedByteArray &r_out) const;
	PackedInt32Array get_active_ids() const;
	PackedVector3Array get_positions() const;
	PackedFloat32Array get_yaws() const;
	PackedByteArray get_states() const;
	PackedByteArray get_tiers() const;

	// --- T3 wire pack (NET R3.5-R3.7, P2.1) ---
	// One SoA walk per client per send. Classifies every live agent by its
	// distance to THIS client (per-client relevance, R3.7 -- computed here, never
	// from the sim tier / nearest_any_dist_sq), and emits an R3.5 record for each
	// agent whose relevance tier is due this tick (p_rate_mask, a HordeWireScheduler
	// SendBit mask). Records are quantized to p_bounds (the nav-grid AABB) and
	// batched into MTU-bounded packets, each prefixed with [server_tick u32][count
	// u16] (R3.6). Positions are absolute-quantized (R3.8 absolute-first MVP).
	//
	// pack_snapshot_into: native core, writes into a caller-owned reused buffer
	// (zero per-call heap on the steady state), returns the packet count. Not
	// bound to script -- a reference out-param does not round-trip through Variant.
	int pack_snapshot_into(uint32_t p_server_tick, const Vector3 &p_client_pos, const AABB &p_bounds,
			float p_hot_radius, float p_mid_radius, int p_rate_mask, PackedByteArray &r_out);
	// GDScript-facing wrapper: packs into the reused member buffer and returns it
	// (the bytes GDScript hands straight to the transport; it never touches
	// per-agent data). Multiple concatenated packets when the set exceeds one MTU.
	PackedByteArray pack_interest_snapshot(uint32_t p_server_tick, const Vector3 &p_client_pos, const AABB &p_bounds,
			float p_hot_radius, float p_mid_radius, int p_rate_mask);
	uint64_t get_pack_time_usec() const { return pack_usec; }

	// --- Metrics (R8.1) ---
	uint64_t get_tick_time_usec() const { return tick_usec; }
	uint64_t get_tier_time_usec(int p_tier) const;
	int get_tier_count(int p_tier) const;

	HordeAgents();
};

VARIANT_ENUM_CAST(HordeAgents::State);
VARIANT_ENUM_CAST(HordeAgents::Tier);
VARIANT_ENUM_CAST(HordeAgents::TransitionReason);
