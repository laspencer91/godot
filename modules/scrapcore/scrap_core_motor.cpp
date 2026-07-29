/**************************************************************************/
/*  scrap_core_motor.cpp                                                  */
/**************************************************************************/

#include "scrap_core_motor.h"

#include "core/object/class_db.h"

#include <scrapcore/movement_motor.h>

#include <cmath>

namespace {

// Trivial no-op pawn body for the Gate-A smoke tick ONLY: flat-floor answers,
// no probes valid, moves nowhere. The real adapter (Box3DPawnBody, driving
// Box3DCharacterMover + PhysicsServer3D directly) is Gate B; this exists so
// motor_smoke() can run one real simulate() through the linked motor object
// code without any physics world.
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

} // namespace

int ScrapCoreMotor::pack_version() const {
	return int(scrap::MovementState::PACK_VERSION);
}

bool ScrapCoreMotor::motor_smoke() const {
	scrap::MovementState state;
	state.current_height = 1.8;
	state.eye_y = 1.6;
	scrap::MovementParams params;
	scrap::InputCommand command;
	SmokeNullPawnBody body;
	scrap::MovementMotor motor;
	scrap::MovementResult out;
	motor.simulate(body, state, command, params, /*tick*/ 0, /*dt*/ 1.0 / 128.0, out);
	return vec_finite(state.position) && vec_finite(state.velocity) &&
			std::isfinite(state.stamina) && std::isfinite(state.current_height) &&
			std::isfinite(state.eye_y) && std::isfinite(state.lean_amount);
}

void ScrapCoreMotor::_bind_methods() {
	ClassDB::bind_method(D_METHOD("pack_version"), &ScrapCoreMotor::pack_version);
	ClassDB::bind_method(D_METHOD("motor_smoke"), &ScrapCoreMotor::motor_smoke);
}
