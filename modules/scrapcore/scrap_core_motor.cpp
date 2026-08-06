/**************************************************************************/
/*  scrap_core_motor.cpp                                                  */
/**************************************************************************/

#include "scrap_core_motor.h"

#include "core/object/class_db.h"

#include <scrapcore/codec.h>
#include <scrapcore/movement_motor.h>

#include <cmath>

namespace {

// Trivial no-op pawn body for motor_smoke() ONLY: flat-floor answers, no
// probes valid, moves nowhere -- a link-level self-test of the compiled core.
// The live adapter is Box3DPawnBody.
class SmokeNullPawnBody final : public scrap::IPawnBody {
public:
	void apply_movement_state(const scrap::MovementState &, const scrap::MovementParams &) override {}
	void move_body(const scrap::Vec3 &, const scrap::MovementParams &) override {}
	scrap::Vec3 get_position() const override { return scrap::Vec3::zero(); }
	scrap::Vec3 get_velocity() const override { return scrap::Vec3::zero(); }
	void set_velocity(const scrap::Vec3 &) override {}
	bool is_grounded() const override { return true; }
	bool is_on_floor() const override { return true; }
	bool is_on_wall() const override { return false; }
	scrap::Vec3 get_wall_normal() const override { return scrap::Vec3::zero(); }
	// Flat floor, matching is_grounded() above (contract_smoke.cpp's stub).
	scrap::Vec3 get_floor_normal() const override { return scrap::Vec3::up(); }
	bool can_stand_up(scrap::Scalar, const scrap::MovementParams &) const override { return true; }
	void set_capsule_for_pose(scrap::Scalar, int32_t, const scrap::MovementParams &) override {}
	bool raycast_blocked(const scrap::Vec3 &, const scrap::Vec3 &, uint32_t) const override { return false; }
	scrap::MantleProbe check_mantle_opportunity(const scrap::Vec2 &, const scrap::MovementState &, const scrap::MovementParams &) const override { return {}; }
	scrap::MantleProbe swept_mantle_landing(const scrap::Vec3 &, const scrap::Vec3 &) const override { return {}; }
	scrap::LadderProbe check_ladder_opportunity(const scrap::Vec2 &, const scrap::MovementState &, const scrap::MovementParams &) const override { return {}; }
	scrap::WallJumpProbe check_wall_jump(const scrap::MovementState &, const scrap::Vec3 &, const scrap::MovementParams &) const override { return {}; }
	const scrap::ILadderVolume *get_ladder_by_id(int32_t) const override { return nullptr; }
};

bool vec_finite(const scrap::Vec3 &v) {
	return std::isfinite(double(v.x)) && std::isfinite(double(v.y)) && std::isfinite(double(v.z));
}

// Every MovementParams field by config name -- the boundary's membership
// record. MovementConfig.to_param_dict() DISCOVERS its keys from the script's
// property list, and _apply_params_dict errors on any name missing here, so a
// config tunable that never reaches the native motor is a loud failure on the
// day it is added, not a silent drift.
struct ScalarParamEntry {
	const char *name;
	scrap::Scalar scrap::MovementParams::*field;
};
struct MaskParamEntry {
	const char *name;
	uint32_t scrap::MovementParams::*field;
};

using MP = scrap::MovementParams;
const ScalarParamEntry SCALAR_PARAMS[] = {
	{ "step_height", &MP::step_height },
	{ "floor_snap_length", &MP::floor_snap_length },
	{ "walk_speed", &MP::walk_speed },
	{ "sprint_speed", &MP::sprint_speed },
	{ "crouch_speed", &MP::crouch_speed },
	{ "prone_speed", &MP::prone_speed },
	{ "backpedal_speed_mult", &MP::backpedal_speed_mult },
	{ "strafe_speed_mult", &MP::strafe_speed_mult },
	{ "ads_speed_mult", &MP::ads_speed_mult },
	{ "ground_accel", &MP::ground_accel },
	{ "ground_initial_accel", &MP::ground_initial_accel },
	{ "ground_friction", &MP::ground_friction },
	{ "air_accel", &MP::air_accel },
	{ "air_speed_mult", &MP::air_speed_mult },
	{ "air_wall_velocity_from_displacement", &MP::air_wall_velocity_from_displacement },
	{ "wall_ride_min_dot", &MP::wall_ride_min_dot },
	{ "wall_ride_accel", &MP::wall_ride_accel },
	{ "wall_ride_max_mult", &MP::wall_ride_max_mult },
	{ "wall_ride_min_speed_frac", &MP::wall_ride_min_speed_frac },
	{ "mantle_no_fire_height", &MP::mantle_no_fire_height },
	{ "jump_velocity", &MP::jump_velocity },
	{ "coyote_time", &MP::coyote_time },
	{ "jump_buffer", &MP::jump_buffer },
	{ "jump_recovery_time", &MP::jump_recovery_time },
	{ "jump_recovery_min_fall_speed", &MP::jump_recovery_min_fall_speed },
	{ "jump_recovery_full_fall_speed", &MP::jump_recovery_full_fall_speed },
	{ "jump_recovery_min_launch_mult", &MP::jump_recovery_min_launch_mult },
	{ "slide_entry_speed", &MP::slide_entry_speed },
	{ "slide_friction", &MP::slide_friction },
	{ "slide_max_time", &MP::slide_max_time },
	{ "slide_min_speed", &MP::slide_min_speed },
	{ "slide_arm_window", &MP::slide_arm_window },
	{ "slide_hop_grace_window", &MP::slide_hop_grace_window },
	{ "slide_slope_friction_gain", &MP::slide_slope_friction_gain },
	{ "slide_slope_uphill_gain", &MP::slide_slope_uphill_gain },
	{ "slide_slope_friction_min", &MP::slide_slope_friction_min },
	{ "slide_slope_time_gain", &MP::slide_slope_time_gain },
	{ "slide_slope_time_scale_min", &MP::slide_slope_time_scale_min },
	{ "wall_jump_entry_speed", &MP::wall_jump_entry_speed },
	{ "wall_jump_speed_floor_mult", &MP::wall_jump_speed_floor_mult },
	{ "wall_jump_speed_max_mult", &MP::wall_jump_speed_max_mult },
	{ "wall_jump_up", &MP::wall_jump_up },
	{ "wall_jump_push", &MP::wall_jump_push },
	{ "wall_jump_redirect", &MP::wall_jump_redirect },
	{ "wall_jump_check_distance", &MP::wall_jump_check_distance },
	{ "wall_jump_check_radius", &MP::wall_jump_check_radius },
	{ "wall_jump_max_normal_y", &MP::wall_jump_max_normal_y },
	{ "gravity", &MP::gravity },
	{ "floor_max_angle", &MP::floor_max_angle },
	{ "recoil_ceiling", &MP::recoil_ceiling },
	{ "recoil_return_rate", &MP::recoil_return_rate },
	{ "recoil_fire_window", &MP::recoil_fire_window },
	{ "capsule_radius", &MP::capsule_radius },
	{ "prone_radius", &MP::prone_radius },
	{ "stand_height", &MP::stand_height },
	{ "crouch_height", &MP::crouch_height },
	{ "prone_height", &MP::prone_height },
	{ "stand_eye_offset", &MP::stand_eye_offset },
	{ "crouch_eye_offset", &MP::crouch_eye_offset },
	{ "prone_eye_offset", &MP::prone_eye_offset },
	{ "pose_transition_rate", &MP::pose_transition_rate },
	{ "prone_dwell_band", &MP::prone_dwell_band },
	{ "prone_dwell_scale", &MP::prone_dwell_scale },
	{ "prone_drop_finish_scale", &MP::prone_drop_finish_scale },
	// (step_smooth_rate left MovementConfig 2026-08-05 -> PlayerFeelConfig:
	// presentation-only, and MovementParams no longer carries the field.)
	{ "lean_distance", &MP::lean_distance },
	{ "lean_roll", &MP::lean_roll },
	{ "lean_transition_rate", &MP::lean_transition_rate },
	{ "lean_obstruction_margin", &MP::lean_obstruction_margin },
	{ "jump_cost", &MP::jump_cost },
	{ "sprint_drain_per_sec", &MP::sprint_drain_per_sec },
	{ "regen_per_sec", &MP::regen_per_sec },
	{ "regen_delay", &MP::regen_delay },
	{ "sprint_min_start_stamina", &MP::sprint_min_start_stamina },
	{ "sprint_min_continue_stamina", &MP::sprint_min_continue_stamina },
	{ "ads_air_land_slow_duration", &MP::ads_air_land_slow_duration },
	{ "ads_air_land_min_speed_mult", &MP::ads_air_land_min_speed_mult },
	{ "jump_land_slow_duration", &MP::jump_land_slow_duration },
	{ "meaningful_landing_min_fall_speed", &MP::meaningful_landing_min_fall_speed },
	{ "jump_land_min_fall_speed", &MP::jump_land_min_fall_speed },
	{ "jump_land_full_fall_speed", &MP::jump_land_full_fall_speed },
	{ "jump_land_min_speed_mult", &MP::jump_land_min_speed_mult },
	{ "landing_hop_takeoff_pull", &MP::landing_hop_takeoff_pull },
	{ "landing_hop_sprint_excess_carry", &MP::landing_hop_sprint_excess_carry },
	{ "min_mantle_height", &MP::min_mantle_height },
	{ "max_mantle_height", &MP::max_mantle_height },
	{ "mantle_detect_distance", &MP::mantle_detect_distance },
	{ "mantle_crouch_max_height", &MP::mantle_crouch_max_height },
	{ "ladder_climb_speed", &MP::ladder_climb_speed },
	{ "ladder_strafe_speed", &MP::ladder_strafe_speed },
	{ "ladder_jump_push", &MP::ladder_jump_push },
	{ "ladder_jump_up", &MP::ladder_jump_up },
	{ "ladder_enter_facing_dot", &MP::ladder_enter_facing_dot },
	{ "ladder_forward_threshold", &MP::ladder_forward_threshold },
	{ "ladder_attach_margin", &MP::ladder_attach_margin },
	{ "ladder_ground_dismount_dot", &MP::ladder_ground_dismount_dot },
	{ "ladder_top_mount_band", &MP::ladder_top_mount_band },
	{ "ladder_transition_lockout", &MP::ladder_transition_lockout },
	{ "ladder_top_back_reach", &MP::ladder_top_back_reach },
	{ "ladder_top_mount_step_speed", &MP::ladder_top_mount_step_speed },
	{ "ladder_lip_clear_epsilon", &MP::ladder_lip_clear_epsilon },
	{ "ladder_top_exit_forward", &MP::ladder_top_exit_forward },
	{ "step_distance", &MP::step_distance },
	{ "step_distance_crouched", &MP::step_distance_crouched },
	{ "step_distance_sprint", &MP::step_distance_sprint },
	{ "mouse_sensitivity", &MP::mouse_sensitivity },
	{ "pitch_limit", &MP::pitch_limit },
	{ "fall_damage_min_velocity", &MP::fall_damage_min_velocity },
	{ "fall_damage_max", &MP::fall_damage_max },
	{ "fatal_fall_velocity", &MP::fatal_fall_velocity },
};
const MaskParamEntry MASK_PARAMS[] = {
	{ "wall_jump_static_mask", &MP::wall_jump_static_mask },
	{ "mantle_static_mask", &MP::mantle_static_mask },
	{ "lean_obstruction_mask", &MP::lean_obstruction_mask },
};

} // namespace

bool ScrapCoreMotor::_apply_params_dict(const Dictionary &p_params, const char *p_context) {
	scrap::MovementParams next; // defaults, then every provided key over them
	for (const KeyValue<Variant, Variant> &kv : p_params) {
		const String name = kv.key;
		bool known = false;
		for (const ScalarParamEntry &entry : SCALAR_PARAMS) {
			if (name == entry.name) {
				next.*(entry.field) = scrap::Scalar(kv.value);
				known = true;
				break;
			}
		}
		if (!known) {
			for (const MaskParamEntry &entry : MASK_PARAMS) {
				if (name == entry.name) {
					next.*(entry.field) = uint32_t(int64_t(kv.value));
					known = true;
					break;
				}
			}
		}
		ERR_FAIL_COND_V_MSG(!known, false,
				vformat("%s: unknown param '%s' -- the native MovementParams has no such field. Add it to the module's param table (drift must be loud, never silent).", p_context, name));
	}
	// Symmetric completeness check: a MISSING key would silently run on the
	// C++ default -- the exact drift the unknown-key error exists to prevent,
	// from the other direction. The caller demands the complete set.
	for (const ScalarParamEntry &entry : SCALAR_PARAMS) {
		ERR_FAIL_COND_V_MSG(!p_params.has(entry.name), false,
				vformat("%s: param '%s' missing -- the complete MovementConfig.to_param_dict() set is required.", p_context, String(entry.name)));
	}
	for (const MaskParamEntry &entry : MASK_PARAMS) {
		ERR_FAIL_COND_V_MSG(!p_params.has(entry.name), false,
				vformat("%s: param '%s' missing -- the complete MovementConfig.to_param_dict() set is required.", p_context, String(entry.name)));
	}
	params = next;
	return true;
}

void ScrapCoreMotor::setup(Object *p_body, const Dictionary &p_params) {
	// Any refusal path leaves the motor NOT ready -- never a stale ready flag
	// over a stale body pointer.
	ready = false;
	CharacterBody3D *character = Object::cast_to<CharacterBody3D>(p_body);
	ERR_FAIL_NULL_MSG(character, "ScrapCoreMotor.setup: body must be a CharacterBody3D.");
	ERR_FAIL_COND_MSG(!character->is_inside_tree(), "ScrapCoreMotor.setup: body must be inside the tree (the mover needs its world space).");
	if (!_apply_params_dict(p_params, "ScrapCoreMotor.setup")) {
		return;
	}
	body.configure(character, params);
	body.set_ladders(&ladders);
	if (!body.is_configured()) {
		return;
	}
	// Seed state from the live pawn, mirroring the controller's _ready
	// (player_controller.gd:218): a simulate() before any reset_state() starts
	// where the pawn stands instead of teleporting it to origin.
	state = scrap::MovementState();
	const Vector3 seed_position = character->get_global_position();
	const Vector3 seed_velocity = character->get_velocity();
	state.position = scrap::Vec3{ seed_position.x, seed_position.y, seed_position.z };
	state.velocity = scrap::Vec3{ seed_velocity.x, seed_velocity.y, seed_velocity.z };
	state.yaw = double(character->get_rotation().y);
	state.pitch = 0.0;
	state.current_height = params.stand_height;
	state.eye_y = params.stand_eye_offset;
	state.was_on_floor = true;
	// The sticky injection input survives re-initialization (it belongs to the
	// equipped weapon, not the timeline).
	state.ads_move_speed_mult = ads_move_speed_mult_input;
	// A (re)setup starts a fresh timeline, which owns its outputs: the old
	// timeline's undrained events and unpresented step pops die with it.
	// (configure() above already zeroed the body's step mailbox.)
	pending_events.clear();
	ready = true;
}

void ScrapCoreMotor::update_params(const Dictionary &p_params) {
	// Live tuning: params only. _apply_params_dict validates the complete set
	// and commits only on success; state, events, and the mailbox are untouched.
	ERR_FAIL_COND_MSG(!ready, "ScrapCoreMotor.update_params: call setup() first.");
	_apply_params_dict(p_params, "ScrapCoreMotor.update_params");
}

void ScrapCoreMotor::register_ladder(int p_id, const Vector3 &p_position, const Vector3 &p_outward_normal, const Vector3 &p_side_dir, double p_height, double p_half_width, double p_attach_depth) {
	ScrapLadderVolume ladder;
	// PlayerPawn._ladder_runtime_id (player_pawn.gd:464-475): the authored id is
	// kept VERBATIM, and there is NO index fallback any more. An unbaked ladder
	// arrives as id 0, stays 0, and is simply not mountable (get_ladder_by_id
	// rejects <= 0) -- the caller has already pushed the "run the map bake"
	// error. The old `id <= 0 -> index + 1` fallback was a per-machine guess at
	// a value that rides the wire, i.e. a desync; do not reintroduce it.
	// Registration order IS the contract (node-path-sorted, like
	// _sorted_ladders) -- no re-sorting here, because authored non-sequential
	// ids must not reorder the probe walk.
	// p_position is LadderVolume.bottom_center(): the gameplay origin every
	// ScrapLadderVolume read measures from, and the point ScrapCore's
	// ILadderVolume::global_position() means (top of ladder = position.y +
	// height). It is NOT the node's global_position for a box-authored ladder.
	ladder.id = p_id;
	ladder.position = p_position;
	ladder.outward = p_outward_normal;
	ladder.side = p_side_dir;
	ladder.ladder_height = p_height;
	ladder.ladder_half_width = p_half_width;
	ladder.ladder_attach_depth = p_attach_depth;
	ladders.push_back(ladder);
}

void ScrapCoreMotor::clear_ladders() {
	ladders.clear();
}

void ScrapCoreMotor::set_ads_move_speed_mult(double p_mult) {
	// Sticky: stored, then re-applied before every tick and after every state
	// reset -- a reconciliation can never silently revert to the 0.75 default.
	ads_move_speed_mult_input = p_mult;
	state.ads_move_speed_mult = p_mult;
}

void ScrapCoreMotor::add_recoil_kick(double p_pitch, double p_yaw, double p_recovery_rate) {
	// player_controller.gd _apply_recoil_kick_to, same ops in the same order so
	// live apply and reconcile re-apply stay bit-exact by construction.
	state.recoil_offset_pitch += p_pitch;
	state.recoil_offset_yaw += p_yaw;
	state.recoil_debt_pitch += p_pitch;
	state.recoil_debt_yaw += p_yaw;
	state.recoil_recovery_rate = p_recovery_rate;
	state.recoil_fire_timer = params.recoil_fire_window;
}

void ScrapCoreMotor::set_active_slot(int p_slot) {
	state.active_slot = p_slot;
}

void ScrapCoreMotor::simulate(int p_tick, double p_delta, const PackedByteArray &p_command) {
	ERR_FAIL_COND_MSG(!ready, "ScrapCoreMotor.simulate: call setup() first.");
	ERR_FAIL_COND_MSG(!body.body_valid(), "ScrapCoreMotor.simulate: the bound body is gone or left the tree.");
	ERR_FAIL_COND_MSG(p_command.size() != scrap::InputCommand::PACKED_SIZE,
			vformat("ScrapCoreMotor.simulate: command must be the %d-byte packed InputCommand (got %d bytes).", scrap::InputCommand::PACKED_SIZE, p_command.size()));
	scrap::codec::ByteReader reader(p_command.ptr(), size_t(p_command.size()));
	const scrap::InputCommand command = scrap::codec::unpack_input_command(reader);
	// The frozen ingest guard: a NaN/Inf is never honest. Drop the tick, never
	// clamp -- exactly what the server-side wire check would have dropped.
	ERR_FAIL_COND_MSG(!command.wire_valid(), "ScrapCoreMotor.simulate: command failed wire_valid (non-finite aim/move) -- tick dropped.");
	// The controller's per-sim refresh (player_controller.gd:827): the sticky
	// injection input lands in state before every tick.
	state.ads_move_speed_mult = ads_move_speed_mult_input;
	motor.simulate(body, state, command, params, int32_t(p_tick), p_delta, result);
	for (const scrap::MovementEvent &event : result.events) {
		pending_events.push_back(event);
	}
}

Vector3 ScrapCoreMotor::get_position() const {
	return Vector3(state.position.x, state.position.y, state.position.z);
}

Vector3 ScrapCoreMotor::get_velocity() const {
	return Vector3(state.velocity.x, state.velocity.y, state.velocity.z);
}

bool ScrapCoreMotor::grounded_contact() const {
	// Pure cached read (Box3DPawnBody::last_result.on_floor); no physics query.
	return body.is_grounded();
}

void ScrapCoreMotor::place_at(const Vector3 &p_position, double p_yaw) {
	ERR_FAIL_COND_MSG(!ready, "ScrapCoreMotor.place_at: call setup() first.");
	ERR_FAIL_COND_MSG(!body.body_valid(), "ScrapCoreMotor.place_at: the bound body is gone or left the tree.");
	state.position = scrap::Vec3{ p_position.x, p_position.y, p_position.z };
	state.velocity = scrap::Vec3::zero();
	state.yaw = p_yaw;
	body.teleport_to_state(state, params);
}

PackedByteArray ScrapCoreMotor::state_packed() const {
	const scrap::codec::Bytes packed = scrap::codec::pack_movement_state(state);
	PackedByteArray out;
	out.resize(packed.size());
	memcpy(out.ptrw(), packed.data(), packed.size());
	return out;
}

void ScrapCoreMotor::reset_state(const PackedByteArray &p_packed, bool p_refresh_ground_contact) {
	ERR_FAIL_COND_MSG(!ready, "ScrapCoreMotor.reset_state: call setup() first.");
	ERR_FAIL_COND_MSG(!body.body_valid(), "ScrapCoreMotor.reset_state: the bound body is gone or left the tree.");
	// Decode into a local and commit only on a clean read -- a truncated
	// payload must not leave split-brain state.
	scrap::codec::ByteReader reader(p_packed.ptr(), size_t(p_packed.size()));
	const scrap::MovementState decoded = scrap::codec::unpack_movement_state(reader);
	ERR_FAIL_COND_MSG(!reader.ok(), "ScrapCoreMotor.reset_state: truncated packed state -- state unchanged.");
	state = decoded;
	// The controller's pre-reconcile re-inject (player_controller.gd:1778):
	// the packed form never carries the multiplier, so the sticky input
	// re-lands here or a reconciliation would silently run at the default.
	state.ads_move_speed_mult = ads_move_speed_mult_input;
	// A reset discards the prediction any undrained events belonged to (the
	// GDScript replay path counts replayed events but never presents them).
	pending_events.clear();
	// PlayerPawn.teleport_to_state: mailbox cleared, state pushed, physics
	// interpolation reset.
	body.teleport_to_state(state, params);
	if (p_refresh_ground_contact) {
		// The replay entry (player_prediction_runner.gd:91): rebuild contact at
		// the authoritative position, then copy the snapped position/velocity
		// back into state so the replay proceeds from what the body settled to.
		body.refresh_ground_contact_after_teleport(params);
		state.position = body.get_position();
		state.velocity = body.get_velocity();
	}
}

double ScrapCoreMotor::consume_collision_step_delta_y() {
	return body.consume_step_delta_y();
}

Array ScrapCoreMotor::drain_events() {
	Array out;
	for (const scrap::MovementEvent &event : pending_events) {
		Dictionary entry;
		entry["kind"] = event.kind;
		entry["tick"] = event.tick;
		entry["input_seq"] = event.input_seq;
		// data keyed exactly like player_movement_event.gd's per-kind payloads,
		// rebuilt from the typed EventData slots. WALL_JUMPED/LADDER_JUMPED
		// carry impulse (and position) in the GDScript dictionary that the
		// typed envelope has no slots for; the keys the envelope CAN carry are
		// emitted, nothing is fabricated.
		Dictionary data;
		switch (scrap::EventKind(event.kind)) {
			case scrap::EventKind::LANDED:
				data["impact_velocity"] = event.data.scalar_a;
				break;
			case scrap::EventKind::FOOTSTEP:
				data["position"] = Vector3(event.data.vec.x, event.data.vec.y, event.data.vec.z);
				break;
			case scrap::EventKind::POSE_CHANGED:
				data["pose"] = event.data.int_a;
				break;
			case scrap::EventKind::MANTLE_STARTED:
				data["target_pitch"] = event.data.scalar_a;
				data["duration"] = event.data.scalar_b;
				data["climb_weight"] = double(event.data.vec.x);
				break;
			case scrap::EventKind::WALL_JUMPED:
			case scrap::EventKind::LADDER_JUMPED:
				data["normal"] = Vector3(event.data.vec.x, event.data.vec.y, event.data.vec.z);
				break;
			default:
				break;
		}
		entry["data"] = data;
		entry["replay_safe"] = event.replay_safe;
		entry["network_relevant"] = event.network_relevant;
		out.push_back(entry);
	}
	pending_events.clear();
	return out;
}

int ScrapCoreMotor::pack_version() const {
	return int(scrap::MovementState::PACK_VERSION);
}

bool ScrapCoreMotor::motor_smoke() const {
	scrap::MovementState smoke_state;
	smoke_state.current_height = 1.8;
	smoke_state.eye_y = 1.6;
	scrap::MovementParams smoke_params;
	scrap::InputCommand command;
	SmokeNullPawnBody null_body;
	scrap::MovementMotor smoke_motor;
	scrap::MovementResult out;
	smoke_motor.simulate(null_body, smoke_state, command, smoke_params, /*tick*/ 0, /*dt*/ 1.0 / 128.0, out);
	return vec_finite(smoke_state.position) && vec_finite(smoke_state.velocity) &&
			std::isfinite(smoke_state.stamina) && std::isfinite(smoke_state.current_height) &&
			std::isfinite(smoke_state.eye_y) && std::isfinite(smoke_state.lean_amount);
}

void ScrapCoreMotor::_bind_methods() {
	ClassDB::bind_method(D_METHOD("setup", "body", "params"), &ScrapCoreMotor::setup);
	ClassDB::bind_method(D_METHOD("update_params", "params"), &ScrapCoreMotor::update_params);
	ClassDB::bind_method(D_METHOD("register_ladder", "id", "position", "outward_normal", "side_dir", "height", "half_width", "attach_depth"), &ScrapCoreMotor::register_ladder);
	ClassDB::bind_method(D_METHOD("clear_ladders"), &ScrapCoreMotor::clear_ladders);
	ClassDB::bind_method(D_METHOD("set_ads_move_speed_mult", "mult"), &ScrapCoreMotor::set_ads_move_speed_mult);
	ClassDB::bind_method(D_METHOD("add_recoil_kick", "pitch", "yaw", "recovery_rate"), &ScrapCoreMotor::add_recoil_kick);
	ClassDB::bind_method(D_METHOD("set_active_slot", "slot"), &ScrapCoreMotor::set_active_slot);
	ClassDB::bind_method(D_METHOD("simulate", "tick", "delta", "command"), &ScrapCoreMotor::simulate);
	ClassDB::bind_method(D_METHOD("get_position"), &ScrapCoreMotor::get_position);
	ClassDB::bind_method(D_METHOD("get_velocity"), &ScrapCoreMotor::get_velocity);
	ClassDB::bind_method(D_METHOD("grounded_contact"), &ScrapCoreMotor::grounded_contact);
	ClassDB::bind_method(D_METHOD("place_at", "position", "yaw"), &ScrapCoreMotor::place_at);
	ClassDB::bind_method(D_METHOD("state_packed"), &ScrapCoreMotor::state_packed);
	ClassDB::bind_method(D_METHOD("reset_state", "packed", "refresh_ground_contact"), &ScrapCoreMotor::reset_state, DEFVAL(false));
	ClassDB::bind_method(D_METHOD("consume_collision_step_delta_y"), &ScrapCoreMotor::consume_collision_step_delta_y);
	ClassDB::bind_method(D_METHOD("drain_events"), &ScrapCoreMotor::drain_events);
	ClassDB::bind_method(D_METHOD("pack_version"), &ScrapCoreMotor::pack_version);
	ClassDB::bind_method(D_METHOD("motor_smoke"), &ScrapCoreMotor::motor_smoke);
}
