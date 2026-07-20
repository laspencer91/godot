/**************************************************************************/
/*  horde_agents.cpp                                                        */
/**************************************************************************/

#include "horde_agents.h"

#include "horde_wire.h"

#include "core/io/marshalls.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "core/templates/hashfuncs.h"

// Engine-internal Box3D access (D-009): resolve the physics space RID to the
// underlying b3World and call Box3D's kinematic mover sweep natively.
#include "modules/box3d_physics/box3d_conversions.h"
#include "modules/box3d_physics/box3d_direct_space_state_3d.h"
#include "modules/box3d_physics/box3d_space_3d.h"

#include "servers/physics_3d/physics_server_3d.h"

#include "box3d/box3d.h"
#include "box3d/id.h"

// The FSM state set is shared between the sim and its data-driven tuning table;
// keep the two enums in lockstep.
static_assert(HordeAgents::STATE_MAX == HordeFSMConfig::STATE_COUNT,
		"HordeAgents::State and HordeFSMConfig must declare the same number of states.");

// Skin (m) kept between a blocked agent and the wall it swept into, so the next
// tick's mover cast starts outside overlap (the cast ignores initial overlap).
static constexpr float CONTACT_SKIN = 0.02f;

void HordeAgents::set_attack_seek_radius(float p_r) {
	attack_seek_radius = Math::is_finite(p_r) ? MAX(p_r, 0.0f) : 0.0f;
}

void HordeAgents::set_attack_standoff_distance(float p_distance) {
	attack_standoff_distance = Math::is_finite(p_distance) ? MAX(p_distance, 0.0f) : 0.0f;
}

void HordeAgents::set_engage_ring_distance(float p_distance) {
	engage_ring_distance = Math::is_finite(p_distance) ? MAX(p_distance, 0.0f) : 0.0f;
}

void HordeAgents::set_engage_drift_speed(float p_speed) {
	engage_drift_speed = Math::is_finite(p_speed) ? MAX(p_speed, 0.0f) : 0.0f;
}

void HordeAgents::set_move_acceleration(float p_accel) {
	// <= 0 is the master sentinel (legacy instant path); a non-finite write also
	// fails safe to 0 (momentum off) rather than to garbage.
	move_acceleration = Math::is_finite(p_accel) ? MAX(p_accel, 0.0f) : 0.0f;
}

void HordeAgents::set_move_turn_rate(float p_rate) {
	move_turn_rate = Math::is_finite(p_rate) ? MAX(p_rate, 0.0f) : 0.0f;
}

void HordeAgents::set_windup_facing_scale(float p_scale) {
	// Default 1.0 = legacy facing; a non-finite write fails safe to 1.0 (no change).
	windup_facing_scale = Math::is_finite(p_scale) ? CLAMP(p_scale, 0.0f, 1.0f) : 1.0f;
}

void HordeAgents::_ensure_field_capacity(int p_field_id) {
	const uint32_t count = (uint32_t)p_field_id + 1;
	if (fields.size() < count) {
		fields.resize(count);
	}
	if (field_domains.size() < count) {
		field_domains.resize(count);
	}
}

void HordeAgents::set_field(int p_field_id, const Ref<HordeFlowField> &p_field) {
	ERR_FAIL_INDEX(p_field_id, MAX_FIELDS);
	_ensure_field_capacity(p_field_id);
	fields[p_field_id] = p_field;
}

void HordeAgents::clear_fields() {
	fields.clear();
	field_domains.clear();
	resolved_fields.clear();
}

void HordeAgents::set_field_domain(int p_field_id, const Rect2 &p_footprint_xz, float p_apron) {
	ERR_FAIL_INDEX(p_field_id, MAX_FIELDS);
	ERR_FAIL_COND_MSG(p_field_id == 0, "Field 0 is the coarse/world field and cannot have a handoff domain.");
	ERR_FAIL_COND_MSG(!p_footprint_xz.is_finite() || p_footprint_xz.size.x <= 0.0f || p_footprint_xz.size.y <= 0.0f,
			"Field footprint must be finite with positive size.");
	ERR_FAIL_COND_MSG(!Math::is_finite(p_apron) || p_apron < 0.0f, "Field apron must be finite and non-negative.");
	const Rect2 outer = p_footprint_xz.grow(p_apron);
	ERR_FAIL_COND_MSG(!outer.is_finite(), "Field footprint plus apron must be finite.");

	_ensure_field_capacity(p_field_id);
	FieldDomain &domain = field_domains[p_field_id];
	domain.footprint = p_footprint_xz;
	domain.outer = outer;
	domain.center = p_footprint_xz.get_center();
	const Vector2 inner_half = p_footprint_xz.size * 0.5f;
	const Vector2 outer_half = domain.outer.size * 0.5f;
	domain.inner_radius_sq = inner_half.length_squared();
	domain.outer_radius_sq = outer_half.length_squared();
	domain.enabled = true;
}

void HordeAgents::set_flow_field(const Ref<HordeFlowField> &p_field) {
	set_field(0, p_field);
}

Ref<HordeFlowField> HordeAgents::get_flow_field() const {
	return fields.is_empty() ? Ref<HordeFlowField>() : fields[0];
}

// Fallback within-state speeds (m/s) when the FSM config leaves move_speed at 0
// (or no config is assigned). Indexed by HordeAgents::State.
static const float DEFAULT_STATE_SPEED[HordeAgents::STATE_MAX] = {
	0.0f, // DORMANT
	0.0f, // WAKE
	1.2f, // ADVANCE
	2.4f, // CHASE
	0.0f, // ATTACK_PLAYER
	0.0f, // GRAB
	0.0f, // ATTACK_OBSTACLE
	0.0f, // STAGGER
	0.0f, // SCREAM
	0.6f, // CRAWL
	0.0f, // DEAD
	0.0f, // KNOCKDOWN
	0.0f, // GET_UP
};

HordeAgents::HordeAgents() {
	_rebuild_storage();
}

void HordeAgents::_rebuild_storage() {
	const uint32_t cap = (uint32_t)capacity;
	pos_x.resize(cap);
	pos_y.resize(cap);
	pos_z.resize(cap);
	yaw.resize(cap);
	target_x.resize(cap);
	target_z.resize(cap);
	state_timer.resize(cap);
	nearest_any_dist_sq.resize(cap);
	hp.resize(cap);
	kb_x.resize(cap);
	kb_z.resize(cap);
	kb_ticks.resize(cap);
	stagger_ticks_left.resize(cap);
	prev_state.resize(cap);
	hit_killer.resize(cap);
	hit_impulse_x.resize(cap);
	hit_impulse_y.resize(cap);
	hit_impulse_z.resize(cap);
	epoch.resize(cap);
	wander.resize(cap);
	state.resize(cap);
	tier.resize(cap);
	archetype.resize(cap);
	field_id.resize(cap);
	speed_scale.resize(cap);
	vel_x.resize(cap);
	vel_z.resize(cap);
	engage_token.resize(cap);
	active.resize(cap);
	pending_reason.resize(cap);
	blocked_contact.resize(cap);
	flow_octant.resize(cap);
	spatial_next.resize(cap);

	for (uint32_t i = 0; i < cap; i++) {
		active[i] = 0;
		epoch[i] = 0;
		vel_x[i] = 0.0f;
		vel_z[i] = 0.0f;
	}

	// Open-addressed spatial hash table: power of two, load factor <= 0.5 even
	// with every agent Hot.
	uint32_t table = 16;
	while (table < cap * 2) {
		table <<= 1;
	}
	hash_mask = table - 1;
	hash_key.resize(table);
	hash_head.resize(table);
	hash_gen.resize(table);
	for (uint32_t b = 0; b < table; b++) {
		hash_gen[b] = 0;
	}
	hash_generation = 0;

	free_slots.clear();
	free_slots.reserve(cap);
	// Push high slots first so the first spawns get low, contiguous ids.
	for (int i = capacity - 1; i >= 0; i--) {
		free_slots.push_back(i);
	}
	active_count = 0;
}

void HordeAgents::set_capacity(int p_capacity) {
	ERR_FAIL_COND_MSG(p_capacity <= 0 || p_capacity > HARD_CAP, "capacity must be in 1..1024 (10-bit id space).");
	capacity = p_capacity;
	_rebuild_storage();
}

void HordeAgents::clear() {
	for (int i = 0; i < capacity; i++) {
		active[i] = 0;
	}
	free_slots.clear();
	free_slots.reserve((uint32_t)capacity);
	for (int i = capacity - 1; i >= 0; i--) {
		free_slots.push_back(i);
	}
	active_count = 0;
	tick_counter = 0;
}

// --- Physics space resolution -----------------------------------------------

void HordeAgents::set_physics_space(RID p_space) {
	clear_physics_space();
	if (!p_space.is_valid()) {
		return;
	}
	PhysicsServer3D *ps = PhysicsServer3D::get_singleton();
	if (ps == nullptr) {
		return;
	}
	PhysicsDirectSpaceState3D *dss = ps->space_get_direct_state(p_space);
	Box3DDirectSpaceState3D *box_state = Object::cast_to<Box3DDirectSpaceState3D>(dss);
	if (box_state == nullptr || box_state->space == nullptr) {
		WARN_PRINT_ONCE("HordeAgents: active physics space is not Box3D; native wall queries disabled.");
		return;
	}
	collision_world_id = b3StoreWorldId(box_state->space->get_world());
}

void HordeAgents::clear_physics_space() {
	collision_world_id = 0;
}

void HordeAgents::set_collision_world_packed(uint32_t p_packed) {
	collision_world_id = p_packed;
}

// --- Spawn / despawn / recycle ----------------------------------------------

bool HordeAgents::_resolve(int p_id, int &r_slot) const {
	const int slot = p_id & ID_SLOT_MASK;
	if (slot < 0 || slot >= capacity || active[slot] == 0) {
		return false;
	}
	if (((int)(epoch[slot] & 1) << ID_SLOT_BITS) != (p_id & ID_EPOCH_BIT)) {
		return false; // Stale id: slot was recycled since this id was issued.
	}
	r_slot = slot;
	return true;
}

void HordeAgents::_init_slot(int p_slot, int p_archetype, const Vector3 &p_pos, int p_state, float p_hp, int p_field_id) {
	pos_x[p_slot] = p_pos.x;
	pos_y[p_slot] = p_pos.y;
	pos_z[p_slot] = p_pos.z;
	yaw[p_slot] = 0.0f;
	target_x[p_slot] = p_pos.x;
	target_z[p_slot] = p_pos.z;
	state_timer[p_slot] = 0.0f;
	nearest_any_dist_sq[p_slot] = 1e30f;
	hp[p_slot] = p_hp;
	kb_x[p_slot] = 0.0f;
	kb_z[p_slot] = 0.0f;
	kb_ticks[p_slot] = 0;
	stagger_ticks_left[p_slot] = 0;
	prev_state[p_slot] = (uint8_t)p_state;
	hit_killer[p_slot] = -1;
	hit_impulse_x[p_slot] = 0.0f;
	hit_impulse_y[p_slot] = 0.0f;
	hit_impulse_z[p_slot] = 0.0f;
	wander[p_slot] = 0;
	state[p_slot] = (uint8_t)p_state;
	tier[p_slot] = (uint8_t)TIER_COLD;
	archetype[p_slot] = (uint8_t)p_archetype;
	field_id[p_slot] = (uint8_t)p_field_id;
	// Spawn-fixed speed identity: pure hash of (slot, current epoch). The epoch
	// bump on despawn re-rolls a recycled slot for free.
	speed_scale[p_slot] = _speed_scale_for_slot(p_slot);
	// Momentum starts at rest; only integrated once move_acceleration > 0.
	vel_x[p_slot] = 0.0f;
	vel_z[p_slot] = 0.0f;
	// Fail-safe: a fresh agent holds the ring until the game grants it a token.
	engage_token[p_slot] = 0;
	active[p_slot] = 1;
	pending_reason[p_slot] = (uint8_t)REASON_NONE;
	blocked_contact[p_slot] = 0;
	flow_octant[p_slot] = (uint8_t)HordeFlowField::OCTANT_NONE;
}

int HordeAgents::spawn(int p_archetype, const Vector3 &p_position, int p_state, float p_hp, int p_field_id) {
	ERR_FAIL_INDEX_V(p_state, STATE_MAX, -1);
	ERR_FAIL_INDEX_V(p_field_id, MAX_FIELDS, -1);
	if (free_slots.is_empty()) {
		return -1; // At the alive cap (R3.4).
	}
	const int slot = free_slots[free_slots.size() - 1];
	free_slots.remove_at(free_slots.size() - 1);
	const float base_hp = p_hp >= 0.0f ? p_hp : _combat_rule(p_archetype).max_hp;
	_init_slot(slot, p_archetype, p_position, p_state, base_hp, p_field_id);
	active_count++;
	return _make_id(slot);
}

bool HordeAgents::despawn(int p_id) {
	int slot;
	if (!_resolve(p_id, slot)) {
		return false;
	}
	active[slot] = 0;
	epoch[slot]++; // Flip the reuse-epoch parity so stale ids fail validation.
	free_slots.push_back(slot);
	active_count--;
	return true;
}

bool HordeAgents::is_alive(int p_id) const {
	int slot;
	return _resolve(p_id, slot);
}

PackedInt32Array HordeAgents::recycle_candidates(int p_max) const {
	PackedInt32Array out;
	if (p_max <= 0) {
		return out;
	}
	// Collect live Cold agents with their (squared) distance, then rank
	// furthest-first -- squared ranks identically to linear. Small N
	// (<= capacity); a simple selection over a scratch list is fine.
	LocalVector<int32_t> slots;
	LocalVector<float> dists;
	for (int i = 0; i < capacity; i++) {
		if (active[i] != 0 && tier[i] == (uint8_t)TIER_COLD) {
			slots.push_back(i);
			dists.push_back(nearest_any_dist_sq[i]);
		}
	}
	const int take = MIN(p_max, (int)slots.size());
	for (int n = 0; n < take; n++) {
		int best = -1;
		float best_d = -1.0f;
		for (uint32_t j = 0; j < slots.size(); j++) {
			if (slots[j] < 0) {
				continue;
			}
			if (dists[j] > best_d) {
				best_d = dists[j];
				best = (int)j;
			}
		}
		if (best < 0) {
			break;
		}
		out.push_back(_make_id(slots[best]));
		slots[best] = -1; // Mark consumed.
	}
	return out;
}

// --- Per-tick ---------------------------------------------------------------

void HordeAgents::set_player_positions(const PackedVector3Array &p_positions) {
	const int n = p_positions.size();
	player_x.resize((uint32_t)n);
	player_z.resize((uint32_t)n);
	for (int i = 0; i < n; i++) {
		const Vector3 p = p_positions[i];
		player_x[i] = p.x;
		player_z[i] = p.z;
	}
}

void HordeAgents::_assign_tiers() {
	const float hot_sq = hot_distance * hot_distance;
	const float warm_sq = warm_distance * warm_distance;
	const int pc = (int)player_x.size();
	const float *px = player_x.ptr();
	const float *pz = player_z.ptr();

	tier_count[TIER_HOT] = 0;
	tier_count[TIER_WARM] = 0;
	tier_count[TIER_COLD] = 0;

	for (int i = 0; i < capacity; i++) {
		if (active[i] == 0) {
			continue;
		}
		float best_sq = 1e30f;
		const float ax = pos_x[i];
		const float az = pos_z[i];
		for (int p = 0; p < pc; p++) {
			const float dx = ax - px[p];
			const float dz = az - pz[p];
			const float d2 = dx * dx + dz * dz;
			if (d2 < best_sq) {
				best_sq = d2;
			}
		}
		nearest_any_dist_sq[i] = best_sq; // Squared; consumers sqrt lazily if needed.
		uint8_t t;
		if (best_sq < hot_sq) {
			t = (uint8_t)TIER_HOT;
		} else if (best_sq < warm_sq) {
			t = (uint8_t)TIER_WARM;
		} else {
			t = (uint8_t)TIER_COLD;
		}
		// Cold advances on the legacy instant path and never runs the wall sweep;
		// any stored momentum would carry the agent through corners at range, so
		// zero it here (the tier transition site). Held at zero every Cold tick, so
		// re-promotion to Warm/Hot starts from rest -- no slingshot on the seam.
		if (t == (uint8_t)TIER_COLD) {
			vel_x[i] = 0.0f;
			vel_z[i] = 0.0f;
		}
		tier[i] = t;
		tier_count[t]++;
	}
}

_FORCE_INLINE_ static int64_t _hash_cell(int32_t p_cx, int32_t p_cz) {
	return ((int64_t)p_cx << 32) ^ (uint32_t)p_cz;
}

void HordeAgents::_build_hot_spatial_hash() {
	// Rebuild = bump the generation; stale buckets self-invalidate on compare.
	if (unlikely(++hash_generation == 0)) {
		// Stamp wraparound (once per ~2^32 rebuilds): reset all stamps.
		for (uint32_t b = 0; b <= hash_mask; b++) {
			hash_gen[b] = 0;
		}
		hash_generation = 1;
	}
	const uint32_t gen = hash_generation;
	const float inv = 1.0f / separation_radius;
	for (int i = 0; i < capacity; i++) {
		if (active[i] == 0 || tier[i] != (uint8_t)TIER_HOT) {
			continue;
		}
		const int32_t cx = (int32_t)Math::floor(pos_x[i] * inv);
		const int32_t cz = (int32_t)Math::floor(pos_z[i] * inv);
		const int64_t key = _hash_cell(cx, cz);
		uint32_t b = (uint32_t)hash_one_uint64((uint64_t)key) & hash_mask;
		while (hash_gen[b] == gen && hash_key[b] != key) {
			b = (b + 1) & hash_mask; // Linear probe; load <= 0.5 by sizing.
		}
		if (hash_gen[b] != gen) {
			hash_gen[b] = gen;
			hash_key[b] = key;
			spatial_next[i] = -1;
			hash_head[b] = i;
		} else {
			spatial_next[i] = hash_head[b];
			hash_head[b] = i;
		}
	}
}

int32_t HordeAgents::_hash_lookup(int64_t p_key) const {
	uint32_t b = (uint32_t)hash_one_uint64((uint64_t)p_key) & hash_mask;
	while (hash_gen[b] == hash_generation) {
		if (hash_key[b] == p_key) {
			return hash_head[b];
		}
		b = (b + 1) & hash_mask;
	}
	return -1;
}

Vector2 HordeAgents::_separation_offset(int p_slot) const {
	// Reynolds separation over the 3x3 hash neighborhood (Hot tier only, R3.3).
	const float inv = 1.0f / separation_radius;
	const float ax = pos_x[p_slot];
	const float az = pos_z[p_slot];
	const int32_t cx = (int32_t)Math::floor(ax * inv);
	const int32_t cz = (int32_t)Math::floor(az * inv);
	const float radius_sq = separation_radius * separation_radius;

	Vector2 push;
	for (int32_t oz = -1; oz <= 1; oz++) {
		for (int32_t ox = -1; ox <= 1; ox++) {
			for (int32_t j = _hash_lookup(_hash_cell(cx + ox, cz + oz)); j != -1; j = spatial_next[j]) {
				if (j == p_slot) {
					continue;
				}
				const float dx = ax - pos_x[j];
				const float dz = az - pos_z[j];
				const float d2 = dx * dx + dz * dz;
				if (d2 >= radius_sq || d2 <= 1e-8f) {
					continue;
				}
				const float d = Math::sqrt(d2);
				// Weight rises as agents overlap; normalize the away-vector.
				const float w = (separation_radius - d) / d;
				push.x += dx * w;
				push.y += dz * w;
			}
		}
	}
	return push;
}

float HordeAgents::_state_speed(int p_archetype, int p_state) const {
	if (fsm_config.is_valid() && p_archetype < fsm_config->get_archetype_count()) {
		const float s = fsm_config->rule(p_archetype, p_state).move_speed;
		if (s > 0.0f) {
			return s;
		}
	}
	return DEFAULT_STATE_SPEED[p_state];
}

int HordeAgents::_movement_mode(int p_archetype, int p_state) const {
	if (fsm_config.is_valid() && p_archetype < fsm_config->get_archetype_count()) {
		return fsm_config->rule(p_archetype, p_state).movement_mode;
	}
	return HordeFSMConfig::default_movement_mode(p_state);
}

const HordeFSMConfig::CombatRule &HordeAgents::_combat_rule(int p_archetype) const {
	if (fsm_config.is_valid() && p_archetype < fsm_config->get_archetype_count()) {
		return fsm_config->combat_rule(p_archetype);
	}
	return HordeFSMConfig::DEFAULT_COMBAT;
}

float HordeAgents::_wander_angle(int p_slot) const {
	// Seeded per-agent wander direction (PATROL): pure integer hash of the
	// slot, its reuse epoch, and the wander re-roll counter -- deterministic,
	// re-rolls on every applied transition (state timer re-arm), no RNG state.
	uint32_t h = (uint32_t)p_slot * 0x9E3779B9u;
	h ^= (uint32_t)epoch[p_slot] * 0x85EBCA6Bu;
	h ^= (uint32_t)wander[p_slot] * 0xC2B2AE35u;
	h = hash_fmix32(h);
	return (float)((double)h * (Math::TAU / 4294967296.0));
}

float HordeAgents::_speed_scale_for_slot(int p_slot) const {
	// jitter == 0 -> exactly 1.0 (byte-identical to a no-jitter build). No
	// dependence on `wander`, so the multiplier is spawn-fixed, not per-state.
	if (speed_jitter <= 0.0f) {
		return 1.0f;
	}
	uint32_t h = (uint32_t)p_slot * 0x9E3779B9u;
	h ^= (uint32_t)epoch[p_slot] * 0x85EBCA6Bu;
	h ^= 0x517CC1B7u; // Domain separator: decorrelate speed from _wander_angle.
	h = hash_fmix32(h);
	// Map the 32-bit hash to [-1, 1), then into [1 - jitter, 1 + jitter).
	const double unit01 = (double)h / 4294967296.0;
	const float centered = (float)(unit01 * 2.0 - 1.0);
	return 1.0f + centered * speed_jitter;
}

float HordeAgents::_engage_drift_sign(int p_slot) const {
	// Pure hash of (slot, epoch) with its own domain separator so the circling
	// direction is a stable per-agent identity, uncorrelated with wander/speed, and
	// re-rolls for free when a slot is recycled (epoch bump). RNG-free (L6).
	uint32_t h = (uint32_t)p_slot * 0x9E3779B9u;
	h ^= (uint32_t)epoch[p_slot] * 0x85EBCA6Bu;
	h ^= 0x27D4EB2Fu; // Domain separator: decorrelate drift sign from speed/wander.
	h = hash_fmix32(h);
	return (h & 1u) ? 1.0f : -1.0f;
}

void HordeAgents::set_speed_jitter(float p_jitter) {
	speed_jitter = Math::is_finite(p_jitter) ? MAX(p_jitter, 0.0f) : 0.0f;
	// Re-seed every live slot so the knob is coherent regardless of whether it is
	// set before or after spawns. Pure hash of (slot, epoch): a live agent's scale
	// is stable for its lifetime once jitter is fixed.
	for (int i = 0; i < capacity; i++) {
		if (active[i] != 0) {
			speed_scale[i] = _speed_scale_for_slot(i);
		}
	}
}

float HordeAgents::_sweep_fraction(int p_slot, const Vector2 &p_disp) const {
	if (collision_world_id == 0) {
		return 1.0f;
	}
	const b3WorldId world = b3LoadWorldId(collision_world_id);
	// Capsule mover is relative to the origin (agent feet). Spans radius..height-
	// radius above the feet so it clears low sills but stops at walls.
	const float r = agent_radius;
	const float top = MAX(r, agent_height - r);
	b3Capsule cap;
	cap.center1 = b3Vec3{ 0.0f, r, 0.0f };
	cap.center2 = b3Vec3{ 0.0f, top, 0.0f };
	cap.radius = r;
	const b3Pos origin = to_box3d(Vector3(pos_x[p_slot], pos_y[p_slot], pos_z[p_slot]));
	const b3Vec3 translation = to_box3d(Vector3(p_disp.x, 0.0f, p_disp.y));

	// Filter built by box3d_physics (single source of truth for the query
	// category bit) so this sweep can never silently diverge from the engine's
	// own scene queries.
	const b3QueryFilter filter = Box3DDirectSpaceState3D::make_query_filter(collision_mask);

	// b3World_CastMover: Box3D's kinematic capsule sweep (slide + anti-clip),
	// returns the translation fraction. No per-call shape build or Variant
	// marshaling -- the cheapest correct wall-contact entry point.
	return b3World_CastMover(world, origin, &cap, translation, filter, nullptr, nullptr);
}

void HordeAgents::_step_movement(double p_delta) {
	// Resolve each registry Ref once per tick. Per-agent sampling below is then
	// one indexed load of a raw field/grid pair, including holes in the id space.
	resolved_fields.resize(fields.size());
	for (uint32_t id = 0; id < resolved_fields.size(); id++) {
		ResolvedField &resolved = resolved_fields[id];
		resolved.field = nullptr;
		resolved.grid = nullptr;
		HordeFlowField *field = fields[id].ptr();
		if (field == nullptr || !field->has_field()) {
			continue;
		}
		HordeNavGrid *grid = field->get_grid().ptr();
		if (grid == nullptr) {
			continue;
		}
		resolved.field = field;
		resolved.grid = grid;
	}

	// One pass per tier so per-tier time is a single bracket (not two clock reads
	// per agent, which would both waste hot-loop cycles and inflate the metric).
	// Iteration stays ascending-slot within each tier, so determinism holds and
	// Hot-tier separation still reads the full Hot set from the spatial hash.
	for (int pass = 0; pass < TIER_MAX; pass++) {
		const uint64_t t0 = OS::get_singleton()->get_ticks_usec();
		for (int i = 0; i < capacity; i++) {
			if (active[i] == 0 || tier[i] != pass) {
				continue;
			}
			// Staggered LOD gate (R3.3): each agent runs on its slot's phase so
			// warm/cold updates spread across ticks instead of spiking one tick.
			const int divisor = _tier_divisor(pass);
			if (((tick_counter + (uint64_t)i) % (uint64_t)divisor) != 0) {
				continue;
			}
			ResolvedField resolved;
			const uint32_t id = field_id[i];
			if (id < resolved_fields.size()) {
				resolved = resolved_fields[id];
			}
			_move_agent(i, p_delta, resolved.field, resolved.grid);
		}
		tier_usec[pass] = OS::get_singleton()->get_ticks_usec() - t0;
	}
}

// True when p_point lies inside p_rect, rejecting first against a circumscribed
// circle (p_center, squared radius p_radius_sq) that bounds the rect: a point
// outside the circle is outside the rect, so the prefilter only short-circuits a
// miss and can never flip a hit. Both handoff edges share this exact test; only
// the circle/rect pair differs (outer apron on exit, inner footprint on enter).
static _FORCE_INLINE_ bool _circle_prefiltered_contains(const Vector2 &p_center, float p_radius_sq,
		const Rect2 &p_rect, const Vector2 &p_point) {
	return (p_point - p_center).length_squared() <= p_radius_sq && p_rect.has_point(p_point);
}

void HordeAgents::_update_field_handoff(int p_slot, int p_movement_mode) {
	if (p_movement_mode != HordeFSMConfig::MOVE_FLOW) {
		return;
	}

	const uint32_t current = field_id[p_slot];
	const Vector2 point(pos_x[p_slot], pos_z[p_slot]);
	if (current != 0) {
		// A fine field without a domain stays explicitly assigned; with one, the
		// outer apron edge is the sole exit threshold (hysteresis: exit on outer).
		if (current >= field_domains.size() || !field_domains[current].enabled) {
			return;
		}
		const FieldDomain &domain = field_domains[current];
		if (!_circle_prefiltered_contains(domain.center, domain.outer_radius_sq, domain.outer, point)) {
			field_id[p_slot] = 0;
			flow_octant[p_slot] = (uint8_t)HordeFlowField::OCTANT_NONE;
		}
		return;
	}

	// Coarse agents enter the first matching fine footprint (hysteresis: enter on
	// the inner footprint). Registry order is the deterministic tie-break when
	// fine domains overlap.
	for (uint32_t id = 1; id < field_domains.size(); id++) {
		const FieldDomain &domain = field_domains[id];
		if (!domain.enabled || id >= resolved_fields.size() || resolved_fields[id].field == nullptr) {
			continue;
		}
		if (_circle_prefiltered_contains(domain.center, domain.inner_radius_sq, domain.footprint, point)) {
			field_id[p_slot] = (uint8_t)id;
			flow_octant[p_slot] = (uint8_t)HordeFlowField::OCTANT_NONE;
			return;
		}
	}
}

void HordeAgents::_move_agent(int i, double p_delta, HordeFlowField *field, HordeNavGrid *grid) {
	const int t = tier[i];
	const int divisor = _tier_divisor(t);
	const float eff_dt = (float)p_delta * (float)divisor;
	state_timer[i] += eff_dt;

	// Native damage-stagger countdown (apply_damage): the halt lasts
	// stagger_duration_ticks of wall ticks (consumed at this slot's LOD
	// cadence), then the pre-stagger state -- and its movement mode -- resumes
	// without a script round-trip. Script-applied STAGGER (ticks_left == 0)
	// keeps the normal timer/exit machinery instead.
	if (state[i] == (uint8_t)STATE_STAGGER && stagger_ticks_left[i] > 0) {
		if ((int)stagger_ticks_left[i] > divisor) {
			stagger_ticks_left[i] -= (uint16_t)divisor;
		} else {
			stagger_ticks_left[i] = 0;
			_enter_state(i, prev_state[i]); // Movement resumes this step.
		}
	}

	const int st = state[i];
	// Single locomotion speed choke point: the spawn-fixed per-agent multiplier
	// scales every downstream branch (flow / seek / patrol / link / goal settle)
	// uniformly. speed_scale is exactly 1.0 for every slot when speed_jitter == 0.
	const float speed = _state_speed(archetype[i], st) * speed_scale[i];
	const int mode = _movement_mode(archetype[i], st);
	const bool have_field = field != nullptr && grid != nullptr;
	Vector2 disp; // Planar displacement this step.
	bool moved_link = false;

	// Refreshed below when the state is flow-driven; exit evaluation reads it.
	flow_octant[i] = (uint8_t)HordeFlowField::OCTANT_NONE;
	// Refreshed below by the wall-contact sweep (Hot/Warm only, further down);
	// exit evaluation reads it to arm REASON_BLOCKED. Cold never runs the
	// sweep branch, so this stays 0 for Cold agents every tick.
	blocked_contact[i] = 0;

	// Close-range attack seek (COMBAT_FEEL): an attacking agent within
	// attack_seek_radius of a player steers/faces the player's real position
	// directly, pre-empting the flow field. This fixes the two failure modes the
	// shared field can't: the goal-cell gradient collapses to zero (OCTANT_GOAL),
	// which left CHASE agents drifting onto the cell center, and ATTACK_PLAYER has
	// speed 0 so it never steered at all -- packed onto a player, the separation
	// ring spun them to face outward and they swung at the air. seek_dir also
	// drives the facing slew below (even at speed 0), so an attacking agent always
	// faces its victim. player_x/player_z and nearest_any_dist_sq are refreshed by
	// set_player_positions/_assign_tiers each retarget.
	bool close_seek = false;
	Vector2 seek_dir;
	float close_seek_distance = 0.0f;
	if ((st == STATE_WAKE || st == STATE_CHASE || st == STATE_ATTACK_PLAYER) && attack_seek_radius > 0.0f &&
			nearest_any_dist_sq[i] <= attack_seek_radius * attack_seek_radius) {
		const int pc = (int)player_x.size();
		const float ax = pos_x[i];
		const float az = pos_z[i];
		float best_sq = 1e30f;
		int best_p = -1;
		for (int p = 0; p < pc; p++) {
			const float dx = player_x[p] - ax;
			const float dz = player_z[p] - az;
			const float d2 = dx * dx + dz * dz;
			if (d2 < best_sq) {
				best_sq = d2;
				best_p = p;
			}
		}
		if (best_p >= 0 && best_sq > 1e-6f) {
			close_seek = true;
			close_seek_distance = Math::sqrt(best_sq);
			seek_dir = Vector2(player_x[best_p] - ax, player_z[best_p] - az) / close_seek_distance;
		}
	}

	if (close_seek) {
		// speed is 0 in ATTACK_PLAYER -> zero disp (holds position), facing still
		// driven by seek_dir; CHASE closes the final gap onto the player.
		disp = seek_dir * (speed * eff_dt);
	} else if (speed > 0.0f && have_field && mode == HordeFSMConfig::MOVE_FLOW) {
		const Vector3 wp(pos_x[i], pos_y[i], pos_z[i]);
		const Vector3i cell = grid->world_to_cell(wp);
		const int32_t idx = grid->cell_index(cell.x, cell.y, cell.z);
		const int oct = field->octant_at_index(idx);
		flow_octant[i] = (uint8_t)oct;
		if (oct == HordeFlowField::OCTANT_LINK) {
			// Step through an inter-floor link (OCTANT_LINK vocabulary): move
			// toward the target cell in 3D, snapping (changing floor) on arrival.
			const int32_t tgt = field->link_target_at_index(idx);
			if (tgt >= 0) {
				const Vector3i tcell = grid->index_to_cell(tgt);
				const Vector3 twp = grid->cell_to_world(tcell.x, tcell.y, tcell.z);
				const Vector3 to = twp - wp;
				const float dist = to.length();
				const float step = speed * eff_dt;
				if (dist <= step || dist <= 1e-5f) {
					pos_x[i] = twp.x;
					pos_y[i] = twp.y;
					pos_z[i] = twp.z;
				} else {
					const Vector3 d = to / dist * step;
					pos_x[i] += d.x;
					pos_y[i] += d.y;
					pos_z[i] += d.z;
				}
				moved_link = true;
			}
		} else if (oct >= HordeFlowField::OCTANT_E && oct <= HordeFlowField::OCTANT_SE) {
			const Vector2 dir = HordeFlowField::octant_to_vector(oct);
			disp = dir * (speed * eff_dt);
		} else if (oct == HordeFlowField::OCTANT_GOAL) {
			// Settle onto the goal cell center in 3D. This also finishes a link
			// ascent: crossing into the (coincident) upstairs goal cell mid-climb
			// hands off from the LINK branch to here, which walks the agent up to
			// the cell-center floor height instead of freezing it in mid-air.
			const Vector3 center = grid->cell_to_world(cell.x, cell.y, cell.z);
			const Vector3 to = center - wp;
			const float dist = to.length();
			if (dist > 1e-4f) {
				const float step = speed * eff_dt;
				if (dist <= step) {
					pos_x[i] = center.x;
					pos_y[i] = center.y;
					pos_z[i] = center.z;
				} else {
					const Vector3 d = to / dist * step;
					pos_x[i] += d.x;
					pos_y[i] += d.y;
					pos_z[i] += d.z;
				}
				moved_link = true;
			}
		}
		// OCTANT_NONE: no advance (locally stuck / unreachable).
	} else if (speed > 0.0f && mode == HordeFSMConfig::MOVE_SEEK_TARGET) {
		Vector2 to(target_x[i] - pos_x[i], target_z[i] - pos_z[i]);
		const float dist = to.length();
		if (dist > 1e-5f) {
			disp = to / dist * MIN(speed * eff_dt, dist);
		}
	} else if (speed > 0.0f && mode == HordeFSMConfig::MOVE_PATROL) {
		// Seeded wander off the flow field (screamer patrol, DES A4.1).
		const float ang = _wander_angle(i);
		disp = Vector2(Math::sin(ang), Math::cos(ang)) * (speed * eff_dt);
	}

	if (moved_link) {
		_update_field_handoff(i, mode);
		return;
	}

	// Desired facing source: the pure steering direction toward the shared flow
	// goal / seek target / patrol heading (or the close-range player seek),
	// captured BEFORE Reynolds separation and knockback perturb `disp`. Facing is
	// slewed toward THIS stable direction, NOT the post-separation displacement: in
	// a dense clump the separation push flips direction every tick and driving
	// atan2 off it snapped the heading around (the flicker bug). A close-range
	// attacker faces its victim via seek_dir even at speed 0 (disp is then zero but
	// the heading still tracks the player). Zero when the agent isn't steering this
	// step (pure shove / idle), which holds its facing.
	const Vector2 desired_dir = close_seek ? seek_dir : disp;

	// Slew yaw toward desired_dir at a capped turn rate, shortest-arc. Hoisted
	// ABOVE the zero-displacement early return so a stationary ATTACK_PLAYER
	// (disp == 0) still turns to face its victim. max_turn_rate bounds |dyaw| per
	// tick so the approach is smooth; yaw stays a per-slot ticked value wrapped to
	// [-PI, PI] (matches atan2's range and the wire codec's assumption), so
	// replays/keyframes stay bit-identical -- no unbounded accumulation for a
	// continuously circling agent.
	if (desired_dir.length_squared() > 1e-8f) {
		const float target_yaw = Math::atan2(desired_dir.x, desired_dir.y);
		// Shortest-arc signed delta in [-PI, PI], then clamp to this tick's cap.
		float d = Math::fposmod(target_yaw - yaw[i] + (float)Math::PI, (float)Math::TAU) - (float)Math::PI;
		// Windup facing gate (F3): ATTACK_PLAYER has zero locomotion, so this slew is
		// the only tracking that exists. windup_facing_scale scales the cap while
		// attacking -- 1.0 = legacy (byte-identical), 0.0 freezes the entry heading so
		// a strafing victim leaves the frozen strike arc and the swing whiffs.
		float max_step = max_turn_rate * eff_dt;
		if (st == STATE_ATTACK_PLAYER) {
			max_step *= windup_facing_scale;
		}
		d = CLAMP(d, -max_step, max_step);
		yaw[i] = Math::fposmod(yaw[i] + d + (float)Math::PI, (float)Math::TAU) - (float)Math::PI;
	}

	// Momentum integration (PLAN_horde_feel_and_interior_nav F2). MASTER SENTINEL:
	// move_acceleration <= 0 skips this entirely, so `disp` stays the exact legacy
	// steering displacement (byte-identical). Cold stays on the legacy instant path
	// too (velocity was zeroed on its demotion). The LINK/GOAL direct-write branches
	// already returned above (moved_link), so they never reach here.
	//
	// The steering `disp` computed above is the DESIRED velocity * eff_dt (speed
	// already folds in speed_scale), so desired_vel = disp / eff_dt. Velocity
	// magnitude accelerates toward the desired magnitude capped by
	// move_acceleration*eff_dt, and its heading rotates toward the desired heading
	// capped by move_turn_rate*eff_dt (uncapped when move_turn_rate <= 0). The
	// realized disp is velocity * eff_dt; the rest of the pipeline (ring/standoff
	// clamps, separation, knockback, sweep) then applies to it unchanged, with each
	// disp-shortening stage writing its correction back into velocity below so
	// stored momentum can't pile against a clamp and slingshot on release.
	const bool momentum = move_acceleration > 0.0f && t != TIER_COLD;
	if (momentum && eff_dt > 1e-8f) {
		const Vector2 desired_vel = disp / eff_dt;
		const float desired_speed = desired_vel.length();
		Vector2 vel(vel_x[i], vel_z[i]);
		const float cur_speed = vel.length();

		// Heading: adopt the desired heading immediately from rest, else slew the
		// current heading toward it at the capped rate. move_turn_rate <= 0 snaps.
		// The snap/coast branches reuse the unit vectors already in hand; only the
		// capped slew needs polar math.
		Vector2 new_dir;
		if (desired_speed <= 1e-6f) {
			// No desired direction (decelerating to a stop): keep the current heading
			// so the coast-out stays collinear and just loses magnitude.
			new_dir = cur_speed > 1e-6f ? vel / cur_speed : Vector2(1.0f, 0.0f);
		} else if (cur_speed <= 1e-6f || move_turn_rate <= 0.0f) {
			new_dir = desired_vel / desired_speed;
		} else {
			const float cur_h = Math::atan2(vel.y, vel.x);
			const float des_h = Math::atan2(desired_vel.y, desired_vel.x);
			float dh = Math::fposmod(des_h - cur_h + (float)Math::PI, (float)Math::TAU) - (float)Math::PI;
			const float max_dh = move_turn_rate * eff_dt;
			dh = CLAMP(dh, -max_dh, max_dh);
			const float new_heading = cur_h + dh;
			new_dir = Vector2(Math::cos(new_heading), Math::sin(new_heading));
		}

		// Magnitude: accelerate toward the desired speed, capped per step. Monotone
		// toward desired_speed and never overshoots it (the ramp check relies on this).
		const float new_speed = Math::move_toward(cur_speed, desired_speed, move_acceleration * eff_dt);

		vel = new_dir * new_speed;
		vel_x[i] = vel.x;
		vel_z[i] = vel.y;
		disp = vel * eff_dt;
	}

	// Momentum write-back (F2, decision #4 / D-141): shared by every clamp that
	// shortens player-ward disp — strip the same component from velocity so stored
	// momentum can't pile against the clamp and slingshot on release.
	auto strip_velocity_toward = [&](const Vector2 &p_dir) {
		if (!momentum) {
			return;
		}
		const float v_forward = Vector2(vel_x[i], vel_z[i]).dot(p_dir);
		if (v_forward > 0.0f) {
			vel_x[i] -= p_dir.x * v_forward;
			vel_z[i] -= p_dir.y * v_forward;
		}
	};

	// Hold-ring steering (PLAN_horde_attack_shaping N1): a CHASE agent inside the
	// close-seek envelope that holds no attack token stops at engage_ring_distance
	// instead of closing to attack_standoff_distance, and drifts tangentially so the
	// pack encircles the player rather than stacking at the standoff. Clamps the
	// player-ward component of disp (mirroring the ATTACK_PLAYER standoff clamp below)
	// and adds a per-agent-signed strafe. Layered BEFORE Reynolds separation on
	// purpose: separation (immediately below) is what fans the drifters apart around
	// the arc; facing (slewed above off seek_dir) is untouched, so ring-holders keep
	// looking at the player while they circle. Token holders skip this and close via
	// the ordinary ATTACK ladder. engage_ring_distance == 0 disables it (legacy).
	if (st == STATE_CHASE && close_seek && engage_ring_distance > 0.0f && engage_token[i] == 0) {
		const float forward = disp.dot(seek_dir);
		const float max_forward = MAX(close_seek_distance - engage_ring_distance, 0.0f);
		if (forward > max_forward) {
			disp -= seek_dir * (forward - max_forward);
			// Lateral drift is preserved; only the player-ward component is stripped.
			strip_velocity_toward(seek_dir);
		}
		if (engage_drift_speed > 0.0f) {
			// Perpendicular to seek_dir (player-ward); sign is the agent's fixed hash.
			const Vector2 tangent(-seek_dir.y, seek_dir.x);
			disp += tangent * (_engage_drift_sign(i) * engage_drift_speed * eff_dt);
		}
	}

	// Reynolds separation, Hot tier only (R3.3 / A2.3).
	if (t == TIER_HOT && (disp != Vector2() || mode != HordeFSMConfig::MOVE_STATIONARY)) {
		disp += _separation_offset(i) * (separation_strength * eff_dt);
	}

	// ATTACK_PLAYER is a moving visual commitment, but it must not carry the
	// agent through the victim. Clamp only the displacement component toward the
	// nearest player, after crowd separation has contributed, while preserving
	// lateral/backward avoidance. Knockback below remains a forced displacement
	// and intentionally does not obey this locomotion constraint.
	if (st == STATE_ATTACK_PLAYER && close_seek && attack_standoff_distance > 0.0f) {
		const float forward = disp.dot(seek_dir);
		const float max_forward = MAX(close_seek_distance - attack_standoff_distance, 0.0f);
		if (forward > max_forward) {
			disp -= seek_dir * (forward - max_forward);
			// The whiff carry (leftover CHASE velocity) comes to rest at the standoff.
			strip_velocity_toward(seek_dir);
		}
	}

	// Blunt-knockback stumble (P2.4): consume up to `divisor` decay ticks this
	// step. Per-tick steps decay linearly and telescope to exactly the applied
	// displacement (step = remaining * 2 / (ticks + 1)). Added after the
	// separation block on purpose -- a shove is a forced displacement, not
	// steering -- and it rides the wall sweep below like any other movement.
	// Corpses never slide: the death event impulse drives the ragdoll instead.
	if (kb_ticks[i] > 0 && st != STATE_DEAD) {
		const int steps = MIN((int)kb_ticks[i], divisor);
		for (int s = 0; s < steps; s++) {
			const float f = 2.0f / (float)(kb_ticks[i] + 1);
			disp.x += kb_x[i] * f;
			disp.y += kb_z[i] * f;
			kb_x[i] -= kb_x[i] * f;
			kb_z[i] -= kb_z[i] * f;
			kb_ticks[i]--;
		}
	}

	if (disp == Vector2()) {
		_update_field_handoff(i, mode);
		return;
	}

	// Wall/static contact via Box3D (Hot + Warm; Cold advances freely).
	if (t != TIER_COLD) {
		const float frac = _sweep_fraction(i, disp);
		if (frac < 0.999f) {
			// On contact, stop a small skin short of the surface. The mover cast
			// ignores initial overlap, so landing exactly on (or a hair inside) the
			// wall would let the next tick tunnel through; the skin keeps every cast
			// starting clear.
			const float len = disp.length();
			// Momentum + wall contact (F2, decision #4 -- deviation, justified): the plan
			// proposed zeroing the into-wall velocity component here. That is unsafe with
			// this mover: CONTACT_SKIN + the cast's ignore-initial-overlap rule assume the
			// agent always steps at least a skin's width, which the legacy full-speed disp
			// guarantees. Zeroing velocity makes a momentum agent re-accelerate from rest
			// while pressed on the wall, producing sub-skin steps the cast reports as
			// no-contact (frac == 1); those creep the capsule into overlap and then tunnel.
			// Leaving velocity untouched keeps it at the desired magnitude (bounded by the
			// state speed), so disp stays full-width and is truncated to rest every tick --
			// stable, exactly like the legacy path, and no slingshot on release (velocity
			// never exceeds the desired). The tunnel-on-Cold case the plan cites is already
			// covered by zeroing velocity on demotion to Cold (_assign_tiers).
			const float safe = MAX(0.0f, frac * len - CONTACT_SKIN);
			disp = len > 1e-6f ? disp * (safe / len) : Vector2();
			// Deterministic (the sweep fraction is deterministic): record the
			// contact so _evaluate_exits() can arm REASON_BLOCKED for this slot.
			blocked_contact[i] = 1;
		}
	}
	pos_x[i] += disp.x;
	pos_z[i] += disp.y;
	_update_field_handoff(i, mode);
}

void HordeAgents::_evaluate_exits() {
	const bool have_cfg = fsm_config.is_valid();
	const int arch_count = have_cfg ? fsm_config->get_archetype_count() : 0;

	for (int i = 0; i < capacity; i++) {
		if (active[i] == 0) {
			continue;
		}
		const int st = state[i];
		if (st == STATE_DEAD) {
			// DEAD has no exits, and an unacked REASON_DIED must persist across
			// ticks until script applies the ack transition (at-least-once).
			continue;
		}
		pending_reason[i] = (uint8_t)REASON_NONE;
		if (st == STATE_STAGGER && stagger_ticks_left[i] > 0) {
			continue; // Native damage-stagger recovers by countdown, not by exit.
		}
		const int arch = archetype[i];

		float min_time = 0.0f, max_time = 0.0f, exit_range = 0.0f;
		if (have_cfg && arch < arch_count) {
			const HordeFSMConfig::Rule &r = fsm_config->rule(arch, st);
			min_time = r.min_time;
			max_time = r.max_time;
			exit_range = r.exit_range;
		}

		// Priority: range trigger > obstacle contact > timer deadline > flow-field
		// goal arrival. Range compares squared against squared (no sqrt in this
		// loop). REASON_BLOCKED is gated by min_time like REASON_IN_RANGE so a
		// freshly-entered state can't re-arm instantly off a stale sweep.
		if (exit_range > 0.0f && state_timer[i] >= min_time && nearest_any_dist_sq[i] <= exit_range * exit_range) {
			pending_reason[i] = (uint8_t)REASON_IN_RANGE;
		} else if (blocked_contact[i] != 0 && state_timer[i] >= min_time) {
			pending_reason[i] = (uint8_t)REASON_BLOCKED;
		} else if (max_time > 0.0f && state_timer[i] >= max_time) {
			pending_reason[i] = (uint8_t)REASON_TIMER;
		} else if (flow_octant[i] == (uint8_t)HordeFlowField::OCTANT_GOAL) {
			// Read from the movement step's sample instead of re-sampling
			// world_to_cell + octant. For stagger-gated Warm/Cold agents the
			// sample is up to 4 ticks stale -- acceptable for AT_GOAL arming
			// (the agent hasn't moved since it was taken).
			pending_reason[i] = (uint8_t)REASON_AT_GOAL;
		}
	}
}

void HordeAgents::tick(double p_delta) {
	const uint64_t t0 = OS::get_singleton()->get_ticks_usec();
	tick_counter++;

	_assign_tiers();
	_build_hot_spatial_hash();
	_step_movement(p_delta);
	_evaluate_exits();

	tick_usec = OS::get_singleton()->get_ticks_usec() - t0;
}

// --- Batched FSM transition API ---------------------------------------------

PackedInt32Array HordeAgents::query_transitions() const {
	PackedInt32Array out;
	for (int i = 0; i < capacity; i++) {
		if (active[i] == 0 || pending_reason[i] == (uint8_t)REASON_NONE) {
			continue;
		}
		out.push_back(_make_id(i));
		out.push_back((int)archetype[i]);
		out.push_back((int)state[i]);
		out.push_back((int)pending_reason[i]);
	}
	return out;
}

void HordeAgents::_enter_state(int p_slot, int p_new_state) {
	state[p_slot] = (uint8_t)p_new_state;
	state_timer[p_slot] = 0.0f;
	pending_reason[p_slot] = (uint8_t)REASON_NONE;
	blocked_contact[p_slot] = 0; // Old sample is for the old state.
	flow_octant[p_slot] = (uint8_t)HordeFlowField::OCTANT_NONE; // Old sample is for the old state.
	stagger_ticks_left[p_slot] = 0; // Any transition cancels a native stagger.
	wander[p_slot]++; // Re-roll the PATROL direction on every timer re-arm.
}

bool HordeAgents::apply_transition(int p_id, int p_new_state) {
	ERR_FAIL_INDEX_V(p_new_state, STATE_MAX, false);
	int slot;
	if (!_resolve(p_id, slot)) {
		return false;
	}
	_enter_state(slot, p_new_state);
	return true;
}

void HordeAgents::apply_transitions(const PackedInt32Array &p_pairs) {
	const int n = p_pairs.size();
	ERR_FAIL_COND_MSG((n & 1) != 0, "apply_transitions expects flat [id, new_state, ...] pairs.");
	for (int i = 0; i + 1 < n; i += 2) {
		apply_transition(p_pairs[i], p_pairs[i + 1]);
	}
}

// --- Combat ingress (P2.4) ----------------------------------------------------

// Smallest t >= 0 at which a normalized ray hits a sphere (p_rel = center -
// origin). An origin inside the sphere counts as t = 0.
_FORCE_INLINE_ static bool _ray_sphere_t(const Vector3 &p_rel, const Vector3 &p_dir, float p_radius_sq, float &r_t) {
	const float b = p_rel.dot(p_dir);
	const float c = p_rel.length_squared() - p_radius_sq;
	if (c <= 0.0f) {
		r_t = 0.0f; // Origin inside the sphere.
		return true;
	}
	if (b <= 0.0f) {
		return false; // Sphere behind the origin.
	}
	const float disc = b * b - c;
	if (disc < 0.0f) {
		return false;
	}
	r_t = b - Math::sqrt(disc);
	return true;
}

// Smallest t >= 0 at which a normalized ray hits a VERTICAL capsule whose axis
// runs from p_axis_a straight up to y = p_axis_y1, radius p_radius. An origin
// inside the capsule counts as t = 0 (melee arc probes may start inside a
// body). Union of cylinder band + two cap spheres; a planar miss of the
// axis-aligned cylinder clears the caps too, so it rejects everything early.
_FORCE_INLINE_ static bool _ray_capsule_t(const Vector3 &p_from, const Vector3 &p_dir,
		const Vector3 &p_axis_a, float p_axis_y1, float p_radius, float &r_t) {
	const float rr = p_radius * p_radius;
	const float ox = p_from.x - p_axis_a.x;
	const float oz = p_from.z - p_axis_a.z;
	const float a = p_dir.x * p_dir.x + p_dir.z * p_dir.z;
	const float c = ox * ox + oz * oz - rr;
	if (a > 1e-12f) {
		const float b = ox * p_dir.x + oz * p_dir.z;
		const float disc = b * b - a * c;
		if (disc < 0.0f) {
			return false; // Planar miss: clears the cylinder AND both caps.
		}
		const float t = (-b - Math::sqrt(disc)) / a; // Entry into the infinite cylinder.
		const float y = p_from.y + p_dir.y * t;
		if (t >= 0.0f && y >= p_axis_a.y && y <= p_axis_y1) {
			r_t = t; // Side hit within the axis band.
			return true;
		}
	} else if (c > 0.0f) {
		return false; // Vertical ray outside the cylinder radius.
	}
	if (c <= 0.0f && p_from.y >= p_axis_a.y && p_from.y <= p_axis_y1) {
		r_t = 0.0f; // Origin inside the capsule body.
		return true;
	}
	// Cap spheres pick up entries above/below the cylinder band (a band-crossing
	// ray whose cylinder entry lies outside the band always clips a cap first).
	float t_cap;
	bool any = false;
	if (_ray_sphere_t(p_axis_a - p_from, p_dir, rr, t_cap)) {
		r_t = t_cap;
		any = true;
	}
	if (_ray_sphere_t(Vector3(p_axis_a.x, p_axis_y1, p_axis_a.z) - p_from, p_dir, rr, t_cap) && (!any || t_cap < r_t)) {
		r_t = t_cap;
		any = true;
	}
	return any;
}

// Geometry constants of one weapon scan: the capsule the ray is tested against,
// and the bounding radius that rejects candidates before it. Sweeping a SPHERE
// of p_padding against the capsule IS this ray against the capsule fattened by
// it (Minkowski sum), so the probe inflates the RADIUS and nothing else -- the
// axis segment stays on the real r, or the agent would grow taller as well as
// wider, which no spherecast does.
struct HordeCapsuleScan {
	float radius; // Real capsule radius; the axis endpoints hang off this one.
	float probe; // radius + padding: what the ray is actually tested against.
	float axis_top; // Axis height above the feet. Same capsule as the mover sweep.
	float reach; // Bounding-sphere radius around the feet.

	HordeCapsuleScan(float p_radius, float p_height, float p_padding) {
		radius = p_radius;
		probe = p_radius + MAX(0.0f, p_padding);
		axis_top = MAX(p_radius, p_height - p_radius);
		reach = p_height + probe;
	}
};

// One candidate of a nearest-first scan: true only for a hit STRICTLY nearer
// than r_best_t, which it then advances. This is the whole inner loop of both
// raycast entry points, and it is a function rather than two copies on purpose
// -- the host and a predicting client score the same swing with it (D-073), so
// a divergence here is a head-vs-body damage disagreement, not a rounding one.
_FORCE_INLINE_ static bool _scan_capsule(const Vector3 &p_from, const Vector3 &p_dir,
		const Vector3 &p_base, const HordeCapsuleScan &p_scan, float &r_best_t) {
	// Bounding-sphere reject (feet-centered, radius `reach`) before the exact
	// capsule test: behind the origin, past the current best, or farther from
	// the ray line than the whole agent could span.
	const Vector3 to(p_base.x - p_from.x, p_base.y - p_from.y, p_base.z - p_from.z);
	const float proj = to.dot(p_dir);
	if (proj < -p_scan.reach || proj > r_best_t + p_scan.reach) {
		return false;
	}
	if (to.length_squared() - proj * proj > p_scan.reach * p_scan.reach) {
		return false;
	}
	float t;
	if (!_ray_capsule_t(p_from, p_dir, Vector3(p_base.x, p_base.y + p_scan.radius, p_base.z),
				p_base.y + p_scan.axis_top, p_scan.probe, t) ||
			t >= r_best_t) {
		return false; // Strict '<': an exact tie stays with the earlier candidate.
	}
	r_best_t = t;
	return true;
}

// The {id, hit_pos, height_frac} a landed scan reports. height_frac normalizes
// by the REAL agent height, so a probe grazing over the crown clamps to 1.
static Dictionary _capsule_hit(int p_id, const Vector3 &p_from, const Vector3 &p_dir, float p_t,
		float p_base_y, float p_height) {
	Dictionary hit;
	const Vector3 hit_pos = p_from + p_dir * p_t;
	hit["id"] = p_id;
	hit["hit_pos"] = hit_pos;
	hit["height_frac"] = CLAMP((hit_pos.y - p_base_y) / p_height, 0.0f, 1.0f);
	return hit;
}

Dictionary HordeAgents::raycast_agents(const Vector3 &p_from, const Vector3 &p_dir, float p_max_dist, float p_padding) const {
	Dictionary hit;
	const float dir_len = p_dir.length();
	if (dir_len < 1e-6f || p_max_dist <= 0.0f) {
		return hit;
	}
	const Vector3 dir = p_dir / dir_len;
	const HordeCapsuleScan scan(agent_radius, agent_height, p_padding);

	// Flat SoA scan, nearest-first pruning via best_t. Ascending slot order and
	// the strict '<' in _scan_capsule keep ties deterministic (L6). NOTE:
	// deliberately NOT the hot spatial hash -- it only indexes Hot-tier agents,
	// and a rifle out-ranges the Hot radius (a Warm agent must still be
	// hittable); 250 capsule tests with the cheap reject sit far under the 50 us
	// budget anyway.
	float best_t = p_max_dist;
	int best_slot = -1;
	for (int i = 0; i < capacity; i++) {
		if (active[i] == 0 || state[i] == (uint8_t)STATE_DEAD) {
			continue; // Corpses never hit; freed slots can't resurface (epoch).
		}
		if (_scan_capsule(p_from, dir, Vector3(pos_x[i], pos_y[i], pos_z[i]), scan, best_t)) {
			best_slot = i;
		}
	}
	if (best_slot < 0) {
		return hit;
	}
	return _capsule_hit(_make_id(best_slot), p_from, dir, best_t, pos_y[best_slot], agent_height);
}

Dictionary HordeAgents::raycast_capsules(const PackedVector3Array &p_bases, const PackedInt32Array &p_ids,
		float p_radius, float p_height, const Vector3 &p_from, const Vector3 &p_dir,
		float p_max_dist, float p_padding) {
	Dictionary hit;
	ERR_FAIL_COND_V_MSG(p_bases.size() != p_ids.size(), hit,
			vformat("raycast_capsules: bases and ids must be parallel (%d bases, %d ids).", p_bases.size(), p_ids.size()));
	const float dir_len = p_dir.length();
	if (dir_len < 1e-6f || p_max_dist <= 0.0f || p_radius <= 0.0f || p_height <= 0.0f) {
		return hit;
	}
	const Vector3 dir = p_dir / dir_len;
	const HordeCapsuleScan scan(p_radius, p_height, p_padding);

	// Ascending index, exactly as the SoA scan walks ascending slot.
	const Vector3 *bases = p_bases.ptr();
	const int32_t *ids = p_ids.ptr();
	float best_t = p_max_dist;
	int best = -1;
	for (int i = 0; i < p_bases.size(); i++) {
		if (_scan_capsule(p_from, dir, bases[i], scan, best_t)) {
			best = i;
		}
	}
	if (best < 0) {
		return hit;
	}
	return _capsule_hit(ids[best], p_from, dir, best_t, bases[best].y, p_height);
}

void HordeAgents::_gather_overlap_candidates(const Vector3 &p_center, float p_radius, int p_max_candidates,
		LocalVector<OverlapCandidate> &r_candidates) const {
	r_candidates.clear();
	if (!p_center.is_finite() || !Math::is_finite(p_radius) || p_radius < 0.0f || p_max_candidates <= 0) {
		return;
	}

	struct CandidateSort {
		_FORCE_INLINE_ bool operator()(const OverlapCandidate &p_a, const OverlapCandidate &p_b) const {
			return p_a.contact_dist_sq < p_b.contact_dist_sq ||
					(p_a.contact_dist_sq == p_b.contact_dist_sq && p_a.id < p_b.id);
		}
	};
	const float capsule_radius = agent_radius;
	const float axis_top = MAX(capsule_radius, agent_height - capsule_radius);
	const float combined_radius = p_radius + capsule_radius;
	const float combined_radius_sq = combined_radius * combined_radius;

	for (int i = 0; i < capacity; i++) {
		if (active[i] == 0 || state[i] == (uint8_t)STATE_DEAD) {
			continue;
		}
		const float axis_y = CLAMP(p_center.y, pos_y[i] + capsule_radius, pos_y[i] + axis_top);
		const Vector3 axis_point(pos_x[i], axis_y, pos_z[i]);
		const Vector3 from_axis = p_center - axis_point;
		const float axis_dist_sq = from_axis.length_squared();
		if (axis_dist_sq > combined_radius_sq) {
			continue;
		}

		OverlapCandidate candidate;
		candidate.id = _make_id(i);
		const float axis_dist = Math::sqrt(axis_dist_sq);
		if (axis_dist <= capsule_radius || axis_dist <= CMP_EPSILON) {
			// The query center is already inside the solid capsule, so it is the
			// closest point in that volume and contact distance is zero.
			candidate.hit_pos = p_center;
			candidate.contact_dist_sq = 0.0f;
		} else {
			candidate.hit_pos = axis_point + from_axis * (capsule_radius / axis_dist);
			candidate.contact_dist_sq = p_center.distance_squared_to(candidate.hit_pos);
		}
		candidate.height_frac = CLAMP((candidate.hit_pos.y - pos_y[i]) / agent_height, 0.0f, 1.0f);
		r_candidates.push_back(candidate);
	}

	r_candidates.sort_custom<CandidateSort>();
	if ((int)r_candidates.size() > p_max_candidates) {
		r_candidates.resize(p_max_candidates);
	}
}

Array HordeAgents::overlap_agents(const Vector3 &p_center, float p_radius, int p_max_candidates) const {
	LocalVector<OverlapCandidate> candidates;
	_gather_overlap_candidates(p_center, p_radius, p_max_candidates, candidates);

	Array result;
	result.resize(candidates.size());
	for (uint32_t i = 0; i < candidates.size(); i++) {
		Dictionary item;
		item["id"] = candidates[i].id;
		item["hit_pos"] = candidates[i].hit_pos;
		item["height_frac"] = candidates[i].height_frac;
		result[i] = item;
	}
	return result;
}

PackedInt32Array HordeAgents::overlap_agent_ids(const Vector3 &p_center, float p_radius, int p_max_candidates) const {
	LocalVector<OverlapCandidate> candidates;
	_gather_overlap_candidates(p_center, p_radius, p_max_candidates, candidates);

	PackedInt32Array result;
	result.resize(candidates.size());
	int32_t *write = result.ptrw();
	for (uint32_t i = 0; i < candidates.size(); i++) {
		write[i] = candidates[i].id;
	}
	return result;
}

int HordeAgents::apply_damage(int p_id, float p_amount, const Vector3 &p_impulse_dir, int p_killer_hint,
		float p_knockback, int p_stagger_duration_ticks) {
	int slot;
	if (!_resolve(p_id, slot)) {
		return -1;
	}
	if (state[slot] == (uint8_t)STATE_DEAD) {
		return STATE_DEAD; // Corpses absorb nothing; death info stays the killing hit's.
	}
	// Record the hit so the R3.9 death event path can carry killer/impulse
	// (get_death_info); on a kill this is by definition the killing hit.
	hit_killer[slot] = p_killer_hint;
	hit_impulse_x[slot] = p_impulse_dir.x;
	hit_impulse_y[slot] = p_impulse_dir.y;
	hit_impulse_z[slot] = p_impulse_dir.z;

	hp[slot] -= p_amount;
	if (hp[slot] <= 0.0f) {
		hp[slot] = 0.0f;
		_enter_state(slot, STATE_DEAD);
		// The DEAD transition is already applied (HP authority is native); the
		// quad is the death notification script acks, then emits R3.9.
		pending_reason[slot] = (uint8_t)REASON_DIED;
		return STATE_DEAD;
	}

	const HordeFSMConfig::CombatRule &cr = _combat_rule(archetype[slot]);

	// Blunt knockback (COMBAT_FEEL section 4): a horizontal stumble along the
	// impulse, config-capped, decaying over the config window. _move_agent
	// applies it through the CastMover + skin path -- never through walls.
	if (p_knockback > 0.0f && cr.knockback_duration_ticks > 0) {
		Vector2 kb_dir(p_impulse_dir.x, p_impulse_dir.z);
		const float len = kb_dir.length();
		if (len > 1e-5f) {
			const float d = MIN(p_knockback, cr.knockback_distance_cap);
			kb_dir /= len;
			kb_x[slot] = kb_dir.x * d;
			kb_z[slot] = kb_dir.y * d;
			kb_ticks[slot] = (uint16_t)MIN(cr.knockback_duration_ticks, 0xFFFF);
		}
	}

	// Server-confirmed heavy reaction ladder. Knockdown is checked first and uses
	// ordinary timed FSM states; stagger retains its compact native countdown.
	// Re-hitting an already downed agent damages it without restarting the fall.
	if (state[slot] == (uint8_t)STATE_KNOCKDOWN) {
		return STATE_KNOCKDOWN; // Damage lands, but the fall timer and pose do not restart.
	}
	const bool interrupted_get_up = state[slot] == (uint8_t)STATE_GET_UP &&
			p_amount >= cr.stagger_damage_frac * cr.max_hp;
	if (p_amount >= cr.knockdown_damage_frac * cr.max_hp || interrupted_get_up) {
		if (state[slot] != (uint8_t)STATE_KNOCKDOWN) {
			_enter_state(slot, STATE_KNOCKDOWN);
		}
		return STATE_KNOCKDOWN;
	}

	// A lighter qualifying hit halts the agent for the requested weapon override,
	// or the archetype config window when no override was supplied.
	const int stagger_duration_ticks = p_stagger_duration_ticks >= 0 ? p_stagger_duration_ticks : cr.stagger_duration_ticks;
	if (p_amount >= cr.stagger_damage_frac * cr.max_hp && stagger_duration_ticks > 0) {
		if (state[slot] != (uint8_t)STATE_STAGGER) {
			prev_state[slot] = state[slot]; // A re-stagger keeps the original resume state.
		}
		_enter_state(slot, STATE_STAGGER);
		stagger_ticks_left[slot] = (uint16_t)MIN(stagger_duration_ticks, 0xFFFF);
	}
	return (int)state[slot];
}

Dictionary HordeAgents::get_death_info(int p_id) const {
	Dictionary d;
	int slot;
	if (!_resolve(p_id, slot)) {
		return d;
	}
	d["killer_hint"] = hit_killer[slot];
	d["impulse_dir"] = Vector3(hit_impulse_x[slot], hit_impulse_y[slot], hit_impulse_z[slot]);
	return d;
}

// --- Agent access -----------------------------------------------------------

int HordeAgents::get_agent_state(int p_id) const {
	int slot;
	return _resolve(p_id, slot) ? (int)state[slot] : -1;
}

int HordeAgents::get_agent_tier(int p_id) const {
	int slot;
	return _resolve(p_id, slot) ? (int)tier[slot] : -1;
}

int HordeAgents::get_agent_archetype(int p_id) const {
	int slot;
	return _resolve(p_id, slot) ? (int)archetype[slot] : -1;
}

int HordeAgents::get_agent_field_id(int p_id) const {
	int slot;
	return _resolve(p_id, slot) ? (int)field_id[slot] : -1;
}

bool HordeAgents::set_agent_field(int p_id, int p_field) {
	if (p_field < 0 || p_field >= MAX_FIELDS) {
		return false;
	}
	int slot;
	if (!_resolve(p_id, slot)) {
		return false;
	}
	field_id[slot] = (uint8_t)p_field;
	// The cached flow octant belongs to the old field; clear it so the next
	// movement step resamples and _evaluate_exits() cannot arm a stale AT_GOAL
	// (mirrors the domain handoff at _update_field_handoff).
	flow_octant[slot] = (uint8_t)HordeFlowField::OCTANT_NONE;
	return true;
}

void HordeAgents::set_agent_fields(const PackedInt32Array &p_pairs) {
	const int n = p_pairs.size();
	ERR_FAIL_COND_MSG((n & 1) != 0, "set_agent_fields expects flat [id, field, ...] pairs.");
	const int *r = p_pairs.ptr();
	for (int i = 0; i + 1 < n; i += 2) {
		set_agent_field(r[i], r[i + 1]); // Invalid entries are skipped, not fatal.
	}
}

bool HordeAgents::set_agent_engage_token(int p_id, bool p_granted) {
	int slot;
	if (!_resolve(p_id, slot)) {
		return false;
	}
	engage_token[slot] = p_granted ? 1 : 0;
	return true;
}

void HordeAgents::set_agent_engage_tokens(const PackedInt32Array &p_pairs) {
	const int n = p_pairs.size();
	ERR_FAIL_COND_MSG((n & 1) != 0, "set_agent_engage_tokens expects flat [id, 0|1, ...] pairs.");
	const int *r = p_pairs.ptr();
	for (int i = 0; i + 1 < n; i += 2) {
		set_agent_engage_token(r[i], r[i + 1] != 0); // Stale ids are skipped, not fatal.
	}
}

int HordeAgents::get_agent_engage_token(int p_id) const {
	int slot;
	return _resolve(p_id, slot) ? (int)engage_token[slot] : -1;
}

float HordeAgents::get_agent_speed_scale(int p_id) const {
	int slot;
	return _resolve(p_id, slot) ? speed_scale[slot] : -1.0f;
}

Vector3 HordeAgents::get_agent_position(int p_id) const {
	int slot;
	if (!_resolve(p_id, slot)) {
		return Vector3();
	}
	return Vector3(pos_x[slot], pos_y[slot], pos_z[slot]);
}

float HordeAgents::get_agent_yaw(int p_id) const {
	int slot;
	return _resolve(p_id, slot) ? yaw[slot] : 0.0f;
}

bool HordeAgents::set_agent_yaw(int p_id, float p_yaw) {
	int slot;
	if (!_resolve(p_id, slot) || !Math::is_finite(p_yaw)) {
		return false;
	}
	yaw[slot] = Math::fposmod(p_yaw + (float)Math::PI, (float)Math::TAU) - (float)Math::PI;
	return true;
}

float HordeAgents::get_agent_hp(int p_id) const {
	int slot;
	return _resolve(p_id, slot) ? hp[slot] : 0.0f;
}

float HordeAgents::get_nearest_player_distance(int p_id) const {
	int slot;
	return _resolve(p_id, slot) ? Math::sqrt(nearest_any_dist_sq[slot]) : -1.0f;
}

void HordeAgents::set_agent_hp(int p_id, float p_hp) {
	int slot;
	if (_resolve(p_id, slot)) {
		hp[slot] = p_hp;
	}
}

void HordeAgents::set_agent_target(int p_id, const Vector3 &p_target) {
	int slot;
	if (_resolve(p_id, slot)) {
		target_x[slot] = p_target.x;
		target_z[slot] = p_target.z;
	}
}

void HordeAgents::fill_active_ids(PackedInt32Array &r_out) const {
	if (r_out.size() != active_count) {
		r_out.resize(active_count);
	}
	int *w = r_out.ptrw();
	int n = 0;
	for (int i = 0; i < capacity; i++) {
		if (active[i] != 0) {
			w[n++] = _make_id(i);
		}
	}
}

void HordeAgents::fill_positions(PackedVector3Array &r_out) const {
	if (r_out.size() != active_count) {
		r_out.resize(active_count);
	}
	Vector3 *w = r_out.ptrw();
	int n = 0;
	for (int i = 0; i < capacity; i++) {
		if (active[i] != 0) {
			w[n++] = Vector3(pos_x[i], pos_y[i], pos_z[i]);
		}
	}
}

void HordeAgents::fill_yaws(PackedFloat32Array &r_out) const {
	if (r_out.size() != active_count) {
		r_out.resize(active_count);
	}
	float *w = r_out.ptrw();
	int n = 0;
	for (int i = 0; i < capacity; i++) {
		if (active[i] != 0) {
			w[n++] = yaw[i];
		}
	}
}

void HordeAgents::fill_states(PackedByteArray &r_out) const {
	if (r_out.size() != active_count) {
		r_out.resize(active_count);
	}
	uint8_t *w = r_out.ptrw();
	int n = 0;
	for (int i = 0; i < capacity; i++) {
		if (active[i] != 0) {
			w[n++] = state[i];
		}
	}
}

void HordeAgents::fill_tiers(PackedByteArray &r_out) const {
	if (r_out.size() != active_count) {
		r_out.resize(active_count);
	}
	uint8_t *w = r_out.ptrw();
	int n = 0;
	for (int i = 0; i < capacity; i++) {
		if (active[i] != 0) {
			w[n++] = tier[i];
		}
	}
}

PackedInt32Array HordeAgents::get_active_ids() const {
	PackedInt32Array out;
	fill_active_ids(out);
	return out;
}

PackedVector3Array HordeAgents::get_positions() const {
	PackedVector3Array out;
	fill_positions(out);
	return out;
}

PackedFloat32Array HordeAgents::get_yaws() const {
	PackedFloat32Array out;
	fill_yaws(out);
	return out;
}

PackedByteArray HordeAgents::get_states() const {
	PackedByteArray out;
	fill_states(out);
	return out;
}

PackedByteArray HordeAgents::get_tiers() const {
	PackedByteArray out;
	fill_tiers(out);
	return out;
}

// --- T3 wire pack (NET R3.5-R3.7) -------------------------------------------

int HordeAgents::pack_snapshot_into(uint32_t p_server_tick, const Vector3 &p_client_pos, const AABB &p_bounds,
		float p_hot_radius, float p_mid_radius, int p_rate_mask, PackedByteArray &r_out) {
	const uint64_t t0 = OS::get_singleton()->get_ticks_usec();

	const float hot_sq = p_hot_radius * p_hot_radius;
	const float mid_sq = p_mid_radius * p_mid_radius;
	const bool send_hot = (p_rate_mask & HordeWireScheduler::SEND_HOT) != 0;
	const bool send_mid = (p_rate_mask & HordeWireScheduler::SEND_MID) != 0;
	const bool send_far = (p_rate_mask & HordeWireScheduler::SEND_FAR) != 0;

	// Per-client relevance (R3.7): distance to THIS client, computed here. The
	// sim tier / nearest_any_dist_sq are nearest-to-ANY-player sim-LOD and are
	// deliberately not consulted (ticket structural decision 2). One predicate
	// shared by both passes so the count can never drift from what gets emitted.
	const float cx = p_client_pos.x;
	const float cz = p_client_pos.z;
	auto is_due = [&](int i) -> bool {
		const float dx = pos_x[i] - cx;
		const float dz = pos_z[i] - cz;
		const float d2 = dx * dx + dz * dz;
		return d2 < hot_sq ? send_hot : (d2 < mid_sq ? send_mid : send_far);
	};

	// Pass 1: count due records so the output is sized in a single resize.
	int due = 0;
	for (int i = 0; i < capacity; i++) {
		if (active[i] != 0 && is_due(i)) {
			due++;
		}
	}

	if (due == 0) {
		r_out.resize(0);
		pack_usec = OS::get_singleton()->get_ticks_usec() - t0;
		return 0;
	}

	using namespace HordeWireFormat;
	const int packets = (due + MAX_RECORDS_PER_PACKET - 1) / MAX_RECORDS_PER_PACKET;
	const int total = due * RECORD_BYTES + packets * HEADER_BYTES;
	if (r_out.size() != total) {
		r_out.resize(total);
	}
	uint8_t *w = r_out.ptrw();

	// Pass 2: emit in ascending slot order (deterministic), opening a new
	// MTU-bounded packet whenever the current one fills. Counts are known up
	// front (full packets hold MAX_RECORDS_PER_PACKET, the last holds the
	// remainder), so each header is written complete -- no backpatching.
	int cursor = 0;
	int emitted = 0;
	int packet_remaining = 0;
	for (int i = 0; i < capacity; i++) {
		if (active[i] == 0 || !is_due(i)) {
			continue;
		}
		if (packet_remaining == 0) {
			const int this_count = MIN(MAX_RECORDS_PER_PACKET, due - emitted);
			encode_header(p_server_tick, (uint16_t)this_count, w + cursor);
			cursor += HEADER_BYTES;
			packet_remaining = this_count;
		}
		const uint64_t rec = encode_record(_make_id(i),
				Vector3(pos_x[i], pos_y[i], pos_z[i]), yaw[i], state[i], p_bounds);
		encode_uint64(rec, w + cursor);
		cursor += RECORD_BYTES;
		emitted++;
		packet_remaining--;
	}

	pack_usec = OS::get_singleton()->get_ticks_usec() - t0;
	return packets;
}

PackedByteArray HordeAgents::pack_interest_snapshot(uint32_t p_server_tick, const Vector3 &p_client_pos, const AABB &p_bounds,
		float p_hot_radius, float p_mid_radius, int p_rate_mask) {
	pack_snapshot_into(p_server_tick, p_client_pos, p_bounds, p_hot_radius, p_mid_radius, p_rate_mask, pack_scratch);
	return pack_scratch;
}

uint64_t HordeAgents::get_tier_time_usec(int p_tier) const {
	ERR_FAIL_INDEX_V(p_tier, TIER_MAX, 0);
	return tier_usec[p_tier];
}

int HordeAgents::get_tier_count(int p_tier) const {
	ERR_FAIL_INDEX_V(p_tier, TIER_MAX, 0);
	return tier_count[p_tier];
}

void HordeAgents::_bind_methods() {
	// Config.
	ClassDB::bind_method(D_METHOD("set_capacity", "capacity"), &HordeAgents::set_capacity);
	ClassDB::bind_method(D_METHOD("get_capacity"), &HordeAgents::get_capacity);
	ClassDB::bind_method(D_METHOD("set_hot_distance", "distance"), &HordeAgents::set_hot_distance);
	ClassDB::bind_method(D_METHOD("get_hot_distance"), &HordeAgents::get_hot_distance);
	ClassDB::bind_method(D_METHOD("set_warm_distance", "distance"), &HordeAgents::set_warm_distance);
	ClassDB::bind_method(D_METHOD("get_warm_distance"), &HordeAgents::get_warm_distance);
	ClassDB::bind_method(D_METHOD("set_agent_radius", "radius"), &HordeAgents::set_agent_radius);
	ClassDB::bind_method(D_METHOD("get_agent_radius"), &HordeAgents::get_agent_radius);
	ClassDB::bind_method(D_METHOD("set_agent_height", "height"), &HordeAgents::set_agent_height);
	ClassDB::bind_method(D_METHOD("get_agent_height"), &HordeAgents::get_agent_height);
	ClassDB::bind_method(D_METHOD("set_separation_radius", "radius"), &HordeAgents::set_separation_radius);
	ClassDB::bind_method(D_METHOD("get_separation_radius"), &HordeAgents::get_separation_radius);
	ClassDB::bind_method(D_METHOD("set_separation_strength", "strength"), &HordeAgents::set_separation_strength);
	ClassDB::bind_method(D_METHOD("get_separation_strength"), &HordeAgents::get_separation_strength);
	ClassDB::bind_method(D_METHOD("set_max_turn_rate", "rate"), &HordeAgents::set_max_turn_rate);
	ClassDB::bind_method(D_METHOD("get_max_turn_rate"), &HordeAgents::get_max_turn_rate);
	ClassDB::bind_method(D_METHOD("set_speed_jitter", "jitter"), &HordeAgents::set_speed_jitter);
	ClassDB::bind_method(D_METHOD("get_speed_jitter"), &HordeAgents::get_speed_jitter);
	ClassDB::bind_method(D_METHOD("set_attack_seek_radius", "radius"), &HordeAgents::set_attack_seek_radius);
	ClassDB::bind_method(D_METHOD("get_attack_seek_radius"), &HordeAgents::get_attack_seek_radius);
	ClassDB::bind_method(D_METHOD("set_attack_standoff_distance", "distance"), &HordeAgents::set_attack_standoff_distance);
	ClassDB::bind_method(D_METHOD("get_attack_standoff_distance"), &HordeAgents::get_attack_standoff_distance);
	ClassDB::bind_method(D_METHOD("set_engage_ring_distance", "distance"), &HordeAgents::set_engage_ring_distance);
	ClassDB::bind_method(D_METHOD("get_engage_ring_distance"), &HordeAgents::get_engage_ring_distance);
	ClassDB::bind_method(D_METHOD("set_engage_drift_speed", "speed"), &HordeAgents::set_engage_drift_speed);
	ClassDB::bind_method(D_METHOD("get_engage_drift_speed"), &HordeAgents::get_engage_drift_speed);
	ClassDB::bind_method(D_METHOD("set_move_acceleration", "accel"), &HordeAgents::set_move_acceleration);
	ClassDB::bind_method(D_METHOD("get_move_acceleration"), &HordeAgents::get_move_acceleration);
	ClassDB::bind_method(D_METHOD("set_move_turn_rate", "rate"), &HordeAgents::set_move_turn_rate);
	ClassDB::bind_method(D_METHOD("get_move_turn_rate"), &HordeAgents::get_move_turn_rate);
	ClassDB::bind_method(D_METHOD("set_windup_facing_scale", "scale"), &HordeAgents::set_windup_facing_scale);
	ClassDB::bind_method(D_METHOD("get_windup_facing_scale"), &HordeAgents::get_windup_facing_scale);
	ClassDB::bind_method(D_METHOD("set_collision_mask", "mask"), &HordeAgents::set_collision_mask);
	ClassDB::bind_method(D_METHOD("get_collision_mask"), &HordeAgents::get_collision_mask);

	ClassDB::bind_method(D_METHOD("set_flow_field", "flow_field"), &HordeAgents::set_flow_field);
	ClassDB::bind_method(D_METHOD("get_flow_field"), &HordeAgents::get_flow_field);
	ClassDB::bind_method(D_METHOD("set_field", "field_id", "field"), &HordeAgents::set_field);
	ClassDB::bind_method(D_METHOD("clear_fields"), &HordeAgents::clear_fields);
	ClassDB::bind_method(D_METHOD("set_field_domain", "field_id", "footprint_xz", "apron"), &HordeAgents::set_field_domain, DEFVAL(3.0));
	ClassDB::bind_method(D_METHOD("set_fsm_config", "config"), &HordeAgents::set_fsm_config);
	ClassDB::bind_method(D_METHOD("get_fsm_config"), &HordeAgents::get_fsm_config);

	ClassDB::bind_method(D_METHOD("set_physics_space", "space"), &HordeAgents::set_physics_space);
	ClassDB::bind_method(D_METHOD("clear_physics_space"), &HordeAgents::clear_physics_space);
	ClassDB::bind_method(D_METHOD("has_physics_space"), &HordeAgents::has_physics_space);

	// Spawn / despawn / recycle.
	ClassDB::bind_method(D_METHOD("spawn", "archetype", "position", "state", "hp", "field_id"), &HordeAgents::spawn, DEFVAL(STATE_DORMANT), DEFVAL(-1.0), DEFVAL(0));
	ClassDB::bind_method(D_METHOD("despawn", "id"), &HordeAgents::despawn);
	ClassDB::bind_method(D_METHOD("is_alive", "id"), &HordeAgents::is_alive);
	ClassDB::bind_method(D_METHOD("get_active_count"), &HordeAgents::get_active_count);
	ClassDB::bind_method(D_METHOD("clear"), &HordeAgents::clear);
	ClassDB::bind_method(D_METHOD("recycle_candidates", "max"), &HordeAgents::recycle_candidates);

	// Per-tick.
	ClassDB::bind_method(D_METHOD("set_player_positions", "positions"), &HordeAgents::set_player_positions);
	ClassDB::bind_method(D_METHOD("tick", "delta"), &HordeAgents::tick);

	// Batched FSM transition API.
	ClassDB::bind_method(D_METHOD("query_transitions"), &HordeAgents::query_transitions);
	ClassDB::bind_method(D_METHOD("apply_transition", "id", "new_state"), &HordeAgents::apply_transition);
	ClassDB::bind_method(D_METHOD("apply_transitions", "pairs"), &HordeAgents::apply_transitions);

	// Combat ingress (P2.4).
	ClassDB::bind_method(D_METHOD("raycast_agents", "from", "dir", "max_dist", "padding"), &HordeAgents::raycast_agents, DEFVAL(0.0));
	ClassDB::bind_static_method("HordeAgents", D_METHOD("raycast_capsules", "bases", "ids", "radius", "height", "from", "dir", "max_dist", "padding"), &HordeAgents::raycast_capsules, DEFVAL(0.0));
	ClassDB::bind_method(D_METHOD("overlap_agents", "center", "radius", "max_candidates"), &HordeAgents::overlap_agents);
	ClassDB::bind_method(D_METHOD("overlap_agent_ids", "center", "radius", "max_candidates"), &HordeAgents::overlap_agent_ids);
	ClassDB::bind_method(D_METHOD("apply_damage", "id", "amount", "impulse_dir", "killer_hint", "knockback", "stagger_duration_ticks"), &HordeAgents::apply_damage, DEFVAL(0.0), DEFVAL(-1));
	ClassDB::bind_method(D_METHOD("get_death_info", "id"), &HordeAgents::get_death_info);

	// Agent access.
	ClassDB::bind_method(D_METHOD("get_agent_state", "id"), &HordeAgents::get_agent_state);
	ClassDB::bind_method(D_METHOD("get_agent_tier", "id"), &HordeAgents::get_agent_tier);
	ClassDB::bind_method(D_METHOD("get_agent_archetype", "id"), &HordeAgents::get_agent_archetype);
	ClassDB::bind_method(D_METHOD("get_agent_field_id", "id"), &HordeAgents::get_agent_field_id);
	ClassDB::bind_method(D_METHOD("set_agent_field", "id", "field"), &HordeAgents::set_agent_field);
	ClassDB::bind_method(D_METHOD("set_agent_fields", "pairs"), &HordeAgents::set_agent_fields);
	ClassDB::bind_method(D_METHOD("set_agent_engage_token", "id", "granted"), &HordeAgents::set_agent_engage_token);
	ClassDB::bind_method(D_METHOD("set_agent_engage_tokens", "pairs"), &HordeAgents::set_agent_engage_tokens);
	ClassDB::bind_method(D_METHOD("get_agent_engage_token", "id"), &HordeAgents::get_agent_engage_token);
	ClassDB::bind_method(D_METHOD("get_agent_speed_scale", "id"), &HordeAgents::get_agent_speed_scale);
	ClassDB::bind_method(D_METHOD("get_agent_position", "id"), &HordeAgents::get_agent_position);
	ClassDB::bind_method(D_METHOD("get_agent_yaw", "id"), &HordeAgents::get_agent_yaw);
	ClassDB::bind_method(D_METHOD("set_agent_yaw", "id", "yaw"), &HordeAgents::set_agent_yaw);
	ClassDB::bind_method(D_METHOD("get_agent_hp", "id"), &HordeAgents::get_agent_hp);
	ClassDB::bind_method(D_METHOD("get_nearest_player_distance", "id"), &HordeAgents::get_nearest_player_distance);
	ClassDB::bind_method(D_METHOD("set_agent_hp", "id", "hp"), &HordeAgents::set_agent_hp);
	ClassDB::bind_method(D_METHOD("set_agent_target", "id", "target"), &HordeAgents::set_agent_target);

	ClassDB::bind_method(D_METHOD("get_active_ids"), &HordeAgents::get_active_ids);
	ClassDB::bind_method(D_METHOD("get_positions"), &HordeAgents::get_positions);
	ClassDB::bind_method(D_METHOD("get_yaws"), &HordeAgents::get_yaws);
	ClassDB::bind_method(D_METHOD("get_states"), &HordeAgents::get_states);
	ClassDB::bind_method(D_METHOD("get_tiers"), &HordeAgents::get_tiers);

	// T3 wire pack.
	ClassDB::bind_method(D_METHOD("pack_interest_snapshot", "server_tick", "client_pos", "bounds", "hot_radius", "mid_radius", "rate_mask"), &HordeAgents::pack_interest_snapshot);
	ClassDB::bind_method(D_METHOD("get_pack_time_usec"), &HordeAgents::get_pack_time_usec);

	// Metrics.
	ClassDB::bind_method(D_METHOD("get_tick_time_usec"), &HordeAgents::get_tick_time_usec);
	ClassDB::bind_method(D_METHOD("get_tier_time_usec", "tier"), &HordeAgents::get_tier_time_usec);
	ClassDB::bind_method(D_METHOD("get_tier_count", "tier"), &HordeAgents::get_tier_count);

	BIND_ENUM_CONSTANT(STATE_DORMANT);
	BIND_ENUM_CONSTANT(STATE_WAKE);
	BIND_ENUM_CONSTANT(STATE_ADVANCE);
	BIND_ENUM_CONSTANT(STATE_CHASE);
	BIND_ENUM_CONSTANT(STATE_ATTACK_PLAYER);
	BIND_ENUM_CONSTANT(STATE_GRAB);
	BIND_ENUM_CONSTANT(STATE_ATTACK_OBSTACLE);
	BIND_ENUM_CONSTANT(STATE_STAGGER);
	BIND_ENUM_CONSTANT(STATE_SCREAM);
	BIND_ENUM_CONSTANT(STATE_CRAWL);
	BIND_ENUM_CONSTANT(STATE_DEAD);
	BIND_ENUM_CONSTANT(STATE_KNOCKDOWN);
	BIND_ENUM_CONSTANT(STATE_GET_UP);
	BIND_ENUM_CONSTANT(STATE_MAX);

	BIND_ENUM_CONSTANT(TIER_HOT);
	BIND_ENUM_CONSTANT(TIER_WARM);
	BIND_ENUM_CONSTANT(TIER_COLD);
	BIND_ENUM_CONSTANT(TIER_MAX);

	BIND_ENUM_CONSTANT(REASON_NONE);
	BIND_ENUM_CONSTANT(REASON_TIMER);
	BIND_ENUM_CONSTANT(REASON_IN_RANGE);
	BIND_ENUM_CONSTANT(REASON_AT_GOAL);
	BIND_ENUM_CONSTANT(REASON_BLOCKED);
	BIND_ENUM_CONSTANT(REASON_DIED);
}
