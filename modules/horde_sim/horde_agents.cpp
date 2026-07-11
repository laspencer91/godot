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
	active.resize(cap);
	pending_reason.resize(cap);
	blocked_contact.resize(cap);
	flow_octant.resize(cap);
	spatial_next.resize(cap);

	for (uint32_t i = 0; i < cap; i++) {
		active[i] = 0;
		epoch[i] = 0;
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

void HordeAgents::_init_slot(int p_slot, int p_archetype, const Vector3 &p_pos, int p_state, float p_hp) {
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
	active[p_slot] = 1;
	pending_reason[p_slot] = (uint8_t)REASON_NONE;
	blocked_contact[p_slot] = 0;
	flow_octant[p_slot] = (uint8_t)HordeFlowField::OCTANT_NONE;
}

int HordeAgents::spawn(int p_archetype, const Vector3 &p_position, int p_state, float p_hp) {
	ERR_FAIL_INDEX_V(p_state, STATE_MAX, -1);
	if (free_slots.is_empty()) {
		return -1; // At the alive cap (R3.4).
	}
	const int slot = free_slots[free_slots.size() - 1];
	free_slots.remove_at(free_slots.size() - 1);
	const float base_hp = p_hp >= 0.0f ? p_hp : _combat_rule(p_archetype).max_hp;
	_init_slot(slot, p_archetype, p_position, p_state, base_hp);
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

HordeNavGrid *HordeAgents::_resolve_field() const {
	// The flow field is only needed by flow-driven states; a null result
	// disables only the flow branch of movement, never the tick.
	if (flow_field.is_null() || !flow_field->has_field()) {
		return nullptr;
	}
	return flow_field->get_grid().ptr();
}

void HordeAgents::_step_movement(double p_delta) {
	HordeNavGrid *g = _resolve_field();
	const bool have_field = g != nullptr;

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
			_move_agent(i, p_delta, g, have_field);
		}
		tier_usec[pass] = OS::get_singleton()->get_ticks_usec() - t0;
	}
}

void HordeAgents::_move_agent(int i, double p_delta, HordeNavGrid *grid, bool have_field) {
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
	const float speed = _state_speed(archetype[i], st);
	const int mode = _movement_mode(archetype[i], st);
	Vector2 disp; // Planar displacement this step.
	bool moved_link = false;

	// Refreshed below when the state is flow-driven; exit evaluation reads it.
	flow_octant[i] = (uint8_t)HordeFlowField::OCTANT_NONE;
	// Refreshed below by the wall-contact sweep (Hot/Warm only, further down);
	// exit evaluation reads it to arm REASON_BLOCKED. Cold never runs the
	// sweep branch, so this stays 0 for Cold agents every tick.
	blocked_contact[i] = 0;

	if (speed > 0.0f && have_field && mode == HordeFSMConfig::MOVE_FLOW) {
		const Vector3 wp(pos_x[i], pos_y[i], pos_z[i]);
		const Vector3i cell = grid->world_to_cell(wp);
		const int32_t idx = grid->cell_index(cell.x, cell.y, cell.z);
		const int oct = flow_field->octant_at_index(idx);
		flow_octant[i] = (uint8_t)oct;
		if (oct == HordeFlowField::OCTANT_LINK) {
			// Step through an inter-floor link (OCTANT_LINK vocabulary): move
			// toward the target cell in 3D, snapping (changing floor) on arrival.
			const int32_t tgt = flow_field->link_target_at_index(idx);
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
		return;
	}

	// Reynolds separation, Hot tier only (R3.3 / A2.3).
	if (t == TIER_HOT && (disp != Vector2() || mode != HordeFSMConfig::MOVE_STATIONARY)) {
		disp += _separation_offset(i) * (separation_strength * eff_dt);
	}

	// Blunt-knockback stumble (P2.4): consume up to `divisor` decay ticks this
	// step. Per-tick steps decay linearly and telescope to exactly the applied
	// displacement (step = remaining * 2 / (ticks + 1)). Added after the
	// separation block on purpose -- a shove is a forced displacement, not
	// steering -- and it rides the wall sweep below like any other movement.
	// Corpses never slide: the death event impulse drives the ragdoll instead.
	const bool steered = disp != Vector2(); // A pure shove must not turn the agent.
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
			const float safe = MAX(0.0f, frac * len - CONTACT_SKIN);
			disp = len > 1e-6f ? disp * (safe / len) : Vector2();
			// Deterministic (the sweep fraction is deterministic): record the
			// contact so _evaluate_exits() can arm REASON_BLOCKED for this slot.
			blocked_contact[i] = 1;
		}
	}
	pos_x[i] += disp.x;
	pos_z[i] += disp.y;
	if (steered && disp.length_squared() > 1e-8f) {
		yaw[i] = Math::atan2(disp.x, disp.y);
	}
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

Dictionary HordeAgents::raycast_agents(const Vector3 &p_from, const Vector3 &p_dir, float p_max_dist) const {
	Dictionary hit;
	const float dir_len = p_dir.length();
	if (dir_len < 1e-6f || p_max_dist <= 0.0f) {
		return hit;
	}
	const Vector3 dir = p_dir / dir_len;
	const float r = agent_radius;
	const float axis_top = MAX(r, agent_height - r); // Same capsule as the mover sweep.
	const float reach = agent_height + r; // Bounding radius around the feet.

	// Flat SoA scan, nearest-first pruning via best_t. Ascending slot order and
	// a strict '<' keep ties deterministic (L6). NOTE: deliberately NOT the hot
	// spatial hash -- it only indexes Hot-tier agents, and a rifle out-ranges
	// the Hot radius (a Warm agent must still be hittable); 250 capsule tests
	// with the cheap reject below sit far under the 50 us budget anyway.
	float best_t = p_max_dist;
	int best_slot = -1;
	for (int i = 0; i < capacity; i++) {
		if (active[i] == 0 || state[i] == (uint8_t)STATE_DEAD) {
			continue; // Corpses never hit; freed slots can't resurface (epoch).
		}
		// Bounding-sphere reject (feet-centered, radius `reach`) before the
		// exact capsule test: behind the origin, past the current best, or
		// farther from the ray line than the whole agent could span.
		const Vector3 to(pos_x[i] - p_from.x, pos_y[i] - p_from.y, pos_z[i] - p_from.z);
		const float proj = to.dot(dir);
		if (proj < -reach || proj > best_t + reach) {
			continue;
		}
		if (to.length_squared() - proj * proj > reach * reach) {
			continue;
		}
		float t;
		if (_ray_capsule_t(p_from, dir, Vector3(pos_x[i], pos_y[i] + r, pos_z[i]), pos_y[i] + axis_top, r, t) && t < best_t) {
			best_t = t;
			best_slot = i;
		}
	}
	if (best_slot < 0) {
		return hit;
	}
	const Vector3 hit_pos = p_from + dir * best_t;
	hit["id"] = _make_id(best_slot);
	hit["hit_pos"] = hit_pos;
	hit["height_frac"] = CLAMP((hit_pos.y - pos_y[best_slot]) / agent_height, 0.0f, 1.0f);
	return hit;
}

int HordeAgents::apply_damage(int p_id, float p_amount, const Vector3 &p_impulse_dir, int p_killer_hint, float p_knockback) {
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

	// Server-confirmed stagger (COMBAT_FEEL section 3 tier B): one hit at
	// >= stagger_damage_frac of max HP halts the agent for the config window,
	// then the prior state resumes natively (see _move_agent).
	if (p_amount >= cr.stagger_damage_frac * cr.max_hp && cr.stagger_duration_ticks > 0) {
		if (state[slot] != (uint8_t)STATE_STAGGER) {
			prev_state[slot] = state[slot]; // A re-stagger keeps the original resume state.
		}
		_enter_state(slot, STATE_STAGGER);
		stagger_ticks_left[slot] = (uint16_t)MIN(cr.stagger_duration_ticks, 0xFFFF);
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
	ClassDB::bind_method(D_METHOD("set_collision_mask", "mask"), &HordeAgents::set_collision_mask);
	ClassDB::bind_method(D_METHOD("get_collision_mask"), &HordeAgents::get_collision_mask);

	ClassDB::bind_method(D_METHOD("set_flow_field", "flow_field"), &HordeAgents::set_flow_field);
	ClassDB::bind_method(D_METHOD("get_flow_field"), &HordeAgents::get_flow_field);
	ClassDB::bind_method(D_METHOD("set_fsm_config", "config"), &HordeAgents::set_fsm_config);
	ClassDB::bind_method(D_METHOD("get_fsm_config"), &HordeAgents::get_fsm_config);

	ClassDB::bind_method(D_METHOD("set_physics_space", "space"), &HordeAgents::set_physics_space);
	ClassDB::bind_method(D_METHOD("clear_physics_space"), &HordeAgents::clear_physics_space);
	ClassDB::bind_method(D_METHOD("has_physics_space"), &HordeAgents::has_physics_space);

	// Spawn / despawn / recycle.
	ClassDB::bind_method(D_METHOD("spawn", "archetype", "position", "state", "hp"), &HordeAgents::spawn, DEFVAL(STATE_DORMANT), DEFVAL(-1.0));
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
	ClassDB::bind_method(D_METHOD("raycast_agents", "from", "dir", "max_dist"), &HordeAgents::raycast_agents);
	ClassDB::bind_method(D_METHOD("apply_damage", "id", "amount", "impulse_dir", "killer_hint", "knockback"), &HordeAgents::apply_damage, DEFVAL(0.0));
	ClassDB::bind_method(D_METHOD("get_death_info", "id"), &HordeAgents::get_death_info);

	// Agent access.
	ClassDB::bind_method(D_METHOD("get_agent_state", "id"), &HordeAgents::get_agent_state);
	ClassDB::bind_method(D_METHOD("get_agent_tier", "id"), &HordeAgents::get_agent_tier);
	ClassDB::bind_method(D_METHOD("get_agent_archetype", "id"), &HordeAgents::get_agent_archetype);
	ClassDB::bind_method(D_METHOD("get_agent_position", "id"), &HordeAgents::get_agent_position);
	ClassDB::bind_method(D_METHOD("get_agent_yaw", "id"), &HordeAgents::get_agent_yaw);
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
