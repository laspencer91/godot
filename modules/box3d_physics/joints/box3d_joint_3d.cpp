/**************************************************************************/
/*  box3d_joint_3d.cpp                                                     */
/**************************************************************************/

#include "box3d_joint_3d.h"

#include "../box3d_body_3d.h"
#include "../box3d_conversions.h"
#include "../box3d_physics_server_3d.h"
#include "../box3d_space_3d.h"

#include "core/config/engine.h"
#include "core/config/project_settings.h"

namespace {

constexpr real_t HINGE_DEFAULT_BIAS = 0.3;
constexpr real_t HINGE_DEFAULT_LIMIT_BIAS = 0.3;
constexpr real_t HINGE_DEFAULT_SOFTNESS = 0.9;
constexpr real_t HINGE_DEFAULT_RELAXATION = 1.0;

constexpr real_t SLIDER_DEFAULT_LINEAR_LIMIT_SOFTNESS = 1.0;
constexpr real_t SLIDER_DEFAULT_LINEAR_LIMIT_RESTITUTION = 0.7;
constexpr real_t SLIDER_DEFAULT_LINEAR_LIMIT_DAMPING = 1.0;
constexpr real_t SLIDER_DEFAULT_LINEAR_MOTION_SOFTNESS = 1.0;
constexpr real_t SLIDER_DEFAULT_LINEAR_MOTION_RESTITUTION = 0.7;
constexpr real_t SLIDER_DEFAULT_LINEAR_MOTION_DAMPING = 0.0;
constexpr real_t SLIDER_DEFAULT_LINEAR_ORTHO_SOFTNESS = 1.0;
constexpr real_t SLIDER_DEFAULT_LINEAR_ORTHO_RESTITUTION = 0.7;
constexpr real_t SLIDER_DEFAULT_LINEAR_ORTHO_DAMPING = 1.0;
constexpr real_t SLIDER_DEFAULT_ANGULAR_LIMIT = 0.0;
constexpr real_t SLIDER_DEFAULT_ANGULAR_LIMIT_SOFTNESS = 1.0;
constexpr real_t SLIDER_DEFAULT_ANGULAR_LIMIT_RESTITUTION = 0.7;
constexpr real_t SLIDER_DEFAULT_ANGULAR_LIMIT_DAMPING = 0.0;
constexpr real_t SLIDER_DEFAULT_ANGULAR_MOTION_SOFTNESS = 1.0;
constexpr real_t SLIDER_DEFAULT_ANGULAR_MOTION_RESTITUTION = 0.7;
constexpr real_t SLIDER_DEFAULT_ANGULAR_MOTION_DAMPING = 1.0;
constexpr real_t SLIDER_DEFAULT_ANGULAR_ORTHO_SOFTNESS = 1.0;
constexpr real_t SLIDER_DEFAULT_ANGULAR_ORTHO_RESTITUTION = 0.7;
constexpr real_t SLIDER_DEFAULT_ANGULAR_ORTHO_DAMPING = 1.0;

constexpr real_t CONE_TWIST_DEFAULT_BIAS = 0.3;
constexpr real_t CONE_TWIST_DEFAULT_SOFTNESS = 0.8;
constexpr real_t CONE_TWIST_DEFAULT_RELAXATION = 1.0;

Transform3D _frame_from_point(const Vector3 &p_point) {
	return Transform3D(Basis(), p_point);
}

real_t _estimate_physics_step() {
	Engine *engine = Engine::get_singleton();
	if (engine == nullptr) {
		return 1.0 / 60.0;
	}
	return (1.0 / engine->get_user_physics_ticks_per_second()) * engine->get_effective_time_scale();
}

bool _limit_locks(real_t p_lower, real_t p_upper) {
	return Math::is_zero_approx(p_lower) && Math::is_zero_approx(p_upper);
}

} // namespace

Box3DJoint3D::Box3DJoint3D() {
	pin_params[PS3DE::PIN_JOINT_BIAS] = 0.3;
	pin_params[PS3DE::PIN_JOINT_DAMPING] = 1.0;
	pin_params[PS3DE::PIN_JOINT_IMPULSE_CLAMP] = 0.0;

	hinge_params[PS3DE::HINGE_JOINT_BIAS] = HINGE_DEFAULT_BIAS;
	hinge_params[PS3DE::HINGE_JOINT_LIMIT_UPPER] = 0.0;
	hinge_params[PS3DE::HINGE_JOINT_LIMIT_LOWER] = 0.0;
	hinge_params[PS3DE::HINGE_JOINT_LIMIT_BIAS] = HINGE_DEFAULT_LIMIT_BIAS;
	hinge_params[PS3DE::HINGE_JOINT_LIMIT_SOFTNESS] = HINGE_DEFAULT_SOFTNESS;
	hinge_params[PS3DE::HINGE_JOINT_LIMIT_RELAXATION] = HINGE_DEFAULT_RELAXATION;
	hinge_params[PS3DE::HINGE_JOINT_MOTOR_TARGET_VELOCITY] = 0.0;
	hinge_params[PS3DE::HINGE_JOINT_MOTOR_MAX_IMPULSE] = 0.0;

	slider_params[PS3DE::SLIDER_JOINT_LINEAR_LIMIT_UPPER] = 0.0;
	slider_params[PS3DE::SLIDER_JOINT_LINEAR_LIMIT_LOWER] = 0.0;
	slider_params[PS3DE::SLIDER_JOINT_LINEAR_LIMIT_SOFTNESS] = SLIDER_DEFAULT_LINEAR_LIMIT_SOFTNESS;
	slider_params[PS3DE::SLIDER_JOINT_LINEAR_LIMIT_RESTITUTION] = SLIDER_DEFAULT_LINEAR_LIMIT_RESTITUTION;
	slider_params[PS3DE::SLIDER_JOINT_LINEAR_LIMIT_DAMPING] = SLIDER_DEFAULT_LINEAR_LIMIT_DAMPING;
	slider_params[PS3DE::SLIDER_JOINT_LINEAR_MOTION_SOFTNESS] = SLIDER_DEFAULT_LINEAR_MOTION_SOFTNESS;
	slider_params[PS3DE::SLIDER_JOINT_LINEAR_MOTION_RESTITUTION] = SLIDER_DEFAULT_LINEAR_MOTION_RESTITUTION;
	slider_params[PS3DE::SLIDER_JOINT_LINEAR_MOTION_DAMPING] = SLIDER_DEFAULT_LINEAR_MOTION_DAMPING;
	slider_params[PS3DE::SLIDER_JOINT_LINEAR_ORTHOGONAL_SOFTNESS] = SLIDER_DEFAULT_LINEAR_ORTHO_SOFTNESS;
	slider_params[PS3DE::SLIDER_JOINT_LINEAR_ORTHOGONAL_RESTITUTION] = SLIDER_DEFAULT_LINEAR_ORTHO_RESTITUTION;
	slider_params[PS3DE::SLIDER_JOINT_LINEAR_ORTHOGONAL_DAMPING] = SLIDER_DEFAULT_LINEAR_ORTHO_DAMPING;
	slider_params[PS3DE::SLIDER_JOINT_ANGULAR_LIMIT_UPPER] = SLIDER_DEFAULT_ANGULAR_LIMIT;
	slider_params[PS3DE::SLIDER_JOINT_ANGULAR_LIMIT_LOWER] = SLIDER_DEFAULT_ANGULAR_LIMIT;
	slider_params[PS3DE::SLIDER_JOINT_ANGULAR_LIMIT_SOFTNESS] = SLIDER_DEFAULT_ANGULAR_LIMIT_SOFTNESS;
	slider_params[PS3DE::SLIDER_JOINT_ANGULAR_LIMIT_RESTITUTION] = SLIDER_DEFAULT_ANGULAR_LIMIT_RESTITUTION;
	slider_params[PS3DE::SLIDER_JOINT_ANGULAR_LIMIT_DAMPING] = SLIDER_DEFAULT_ANGULAR_LIMIT_DAMPING;
	slider_params[PS3DE::SLIDER_JOINT_ANGULAR_MOTION_SOFTNESS] = SLIDER_DEFAULT_ANGULAR_MOTION_SOFTNESS;
	slider_params[PS3DE::SLIDER_JOINT_ANGULAR_MOTION_RESTITUTION] = SLIDER_DEFAULT_ANGULAR_MOTION_RESTITUTION;
	slider_params[PS3DE::SLIDER_JOINT_ANGULAR_MOTION_DAMPING] = SLIDER_DEFAULT_ANGULAR_MOTION_DAMPING;
	slider_params[PS3DE::SLIDER_JOINT_ANGULAR_ORTHOGONAL_SOFTNESS] = SLIDER_DEFAULT_ANGULAR_ORTHO_SOFTNESS;
	slider_params[PS3DE::SLIDER_JOINT_ANGULAR_ORTHOGONAL_RESTITUTION] = SLIDER_DEFAULT_ANGULAR_ORTHO_RESTITUTION;
	slider_params[PS3DE::SLIDER_JOINT_ANGULAR_ORTHOGONAL_DAMPING] = SLIDER_DEFAULT_ANGULAR_ORTHO_DAMPING;

	cone_params[PS3DE::CONE_TWIST_JOINT_SWING_SPAN] = 0.0;
	cone_params[PS3DE::CONE_TWIST_JOINT_TWIST_SPAN] = 0.0;
	cone_params[PS3DE::CONE_TWIST_JOINT_BIAS] = CONE_TWIST_DEFAULT_BIAS;
	cone_params[PS3DE::CONE_TWIST_JOINT_SOFTNESS] = CONE_TWIST_DEFAULT_SOFTNESS;
	cone_params[PS3DE::CONE_TWIST_JOINT_RELAXATION] = CONE_TWIST_DEFAULT_RELAXATION;
}

Box3DJoint3D::~Box3DJoint3D() {
	clear();
}

void Box3DJoint3D::_set_bodies(Box3DBody3D *p_body_a, Box3DBody3D *p_body_b) {
	destroy_box3d_joint();
	if (body_a != nullptr) {
		body_a->remove_joint(this);
	}
	if (body_b != nullptr) {
		body_b->remove_joint(this);
	}
	body_a = p_body_a;
	body_b = p_body_b;
	body_a_rid = body_a ? body_a->get_rid() : RID();
	body_b_rid = body_b ? body_b->get_rid() : RID();
	if (body_a != nullptr) {
		body_a->add_joint(this);
	}
	if (body_b != nullptr) {
		body_b->add_joint(this);
	}
}

bool Box3DJoint3D::_can_build() const {
	if (type == PS3DE::JOINT_TYPE_MAX || body_a == nullptr || body_b == nullptr) {
		return false;
	}
	return body_a->in_space() && body_b->in_space() && body_a->get_space() == body_b->get_space();
}

Box3DSpace3D *Box3DJoint3D::_get_joint_space() const {
	if (!_can_build()) {
		return nullptr;
	}
	return body_a->get_space();
}

void Box3DJoint3D::_fill_base_def(b3JointDef &r_def) {
	_fill_base_def(r_def, local_frame_a, local_frame_b);
}

void Box3DJoint3D::_fill_base_def(b3JointDef &r_def, const Transform3D &p_frame_a, const Transform3D &p_frame_b) {
	r_def.bodyIdA = body_a->get_body_id();
	r_def.bodyIdB = body_b->get_body_id();
	r_def.localFrameA = to_box3d(p_frame_a);
	r_def.localFrameB = to_box3d(p_frame_b);
	r_def.collideConnected = !collision_disabled;
	r_def.constraintHertz = (float)(constraint_hertz >= 0.0 ? constraint_hertz : (real_t)GLOBAL_GET("physics/box3d/joints/constraint_hertz"));
	r_def.constraintDampingRatio = (float)(constraint_damping_ratio >= 0.0 ? constraint_damping_ratio : (real_t)GLOBAL_GET("physics/box3d/joints/constraint_damping_ratio"));
	r_def.forceThreshold = (float)force_threshold;
	r_def.torqueThreshold = (float)torque_threshold;
	r_def.userData = this;
}

float Box3DJoint3D::_max_motor_torque_from_impulse(real_t p_impulse) const {
	if (p_impulse <= 0.0) {
		return 0.0f;
	}
	real_t step = 0.0;
	if (body_a != nullptr && body_a->get_space() != nullptr) {
		step = body_a->get_space()->get_last_step();
	}
	if (step <= 0.0) {
		step = _estimate_physics_step();
	}
	return (float)(p_impulse / MAX(step, CMP_EPSILON));
}

float Box3DJoint3D::_hinge_box3d_lower_limit() const {
	return (float)-hinge_params[PS3DE::HINGE_JOINT_LIMIT_UPPER];
}

float Box3DJoint3D::_hinge_box3d_upper_limit() const {
	return (float)-hinge_params[PS3DE::HINGE_JOINT_LIMIT_LOWER];
}

float Box3DJoint3D::_hinge_box3d_motor_speed() const {
	return (float)-hinge_params[PS3DE::HINGE_JOINT_MOTOR_TARGET_VELOCITY];
}

Transform3D Box3DJoint3D::_remap_twist_x_to_z(const Transform3D &p_frame) const {
	Basis x_to_z;
	x_to_z.set_column(0, Vector3(0, 1, 0));
	x_to_z.set_column(1, Vector3(0, 0, 1));
	x_to_z.set_column(2, Vector3(1, 0, 0));
	return Transform3D(p_frame.basis * x_to_z, p_frame.origin);
}

Transform3D Box3DJoint3D::_remap_linear_axis_to_x(const Transform3D &p_frame, int p_axis) const {
	if (p_axis == Vector3::AXIS_X) {
		return p_frame;
	}

	Basis remap;
	if (p_axis == Vector3::AXIS_Y) {
		remap.set_column(0, Vector3(0, 1, 0));
		remap.set_column(1, Vector3(0, 0, 1));
		remap.set_column(2, Vector3(1, 0, 0));
	} else {
		remap.set_column(0, Vector3(0, 0, 1));
		remap.set_column(1, Vector3(1, 0, 0));
		remap.set_column(2, Vector3(0, 1, 0));
	}
	return Transform3D(p_frame.basis * remap, p_frame.origin);
}

b3JointId Box3DJoint3D::_create_pin() {
	Box3DSpace3D *space = _get_joint_space();
	ERR_FAIL_NULL_V(space, b3_nullJointId);
	b3SphericalJointDef def = b3DefaultSphericalJointDef();
	_fill_base_def(def.base);
	return b3CreateSphericalJoint(space->get_world(), &def);
}

b3JointId Box3DJoint3D::_create_hinge() {
	Box3DSpace3D *space = _get_joint_space();
	ERR_FAIL_NULL_V(space, b3_nullJointId);
	b3RevoluteJointDef def = b3DefaultRevoluteJointDef();
	_fill_base_def(def.base);
	def.enableLimit = hinge_flags[PS3DE::HINGE_JOINT_FLAG_USE_LIMIT];
	def.lowerAngle = _hinge_box3d_lower_limit();
	def.upperAngle = _hinge_box3d_upper_limit();
	def.enableMotor = hinge_flags[PS3DE::HINGE_JOINT_FLAG_ENABLE_MOTOR];
	def.motorSpeed = _hinge_box3d_motor_speed();
	def.maxMotorTorque = _max_motor_torque_from_impulse(hinge_params[PS3DE::HINGE_JOINT_MOTOR_MAX_IMPULSE]);
	return b3CreateRevoluteJoint(space->get_world(), &def);
}

b3JointId Box3DJoint3D::_create_slider() {
	Box3DSpace3D *space = _get_joint_space();
	ERR_FAIL_NULL_V(space, b3_nullJointId);
	b3PrismaticJointDef def = b3DefaultPrismaticJointDef();
	_fill_base_def(def.base);
	def.enableLimit = true;
	def.lowerTranslation = (float)slider_params[PS3DE::SLIDER_JOINT_LINEAR_LIMIT_LOWER];
	def.upperTranslation = (float)slider_params[PS3DE::SLIDER_JOINT_LINEAR_LIMIT_UPPER];
	return b3CreatePrismaticJoint(space->get_world(), &def);
}

b3JointId Box3DJoint3D::_create_cone_twist() {
	Box3DSpace3D *space = _get_joint_space();
	ERR_FAIL_NULL_V(space, b3_nullJointId);
	b3SphericalJointDef def = b3DefaultSphericalJointDef();
	_fill_base_def(def.base, _remap_twist_x_to_z(local_frame_a), _remap_twist_x_to_z(local_frame_b));
	const float swing = CLAMP((float)cone_params[PS3DE::CONE_TWIST_JOINT_SWING_SPAN], 0.0f, (float)Math::PI);
	const float twist = CLAMP((float)cone_params[PS3DE::CONE_TWIST_JOINT_TWIST_SPAN], 0.0f, (float)Math::PI * 0.99f);
	def.enableConeLimit = true;
	def.coneAngle = swing;
	def.enableTwistLimit = true;
	def.lowerTwistAngle = -twist;
	def.upperTwistAngle = twist;
	return b3CreateSphericalJoint(space->get_world(), &def);
}

b3JointId Box3DJoint3D::_create_generic_6dof() {
	Box3DSpace3D *space = _get_joint_space();
	ERR_FAIL_NULL_V(space, b3_nullJointId);

	bool linear_locked[3] = {};
	bool angular_locked[3] = {};
	int free_linear_count = 0;
	int free_linear_axis = -1;
	bool any_constraint_enabled = false;
	for (int axis = 0; axis < 3; axis++) {
		for (int flag = 0; flag < PS3DE::G6DOF_JOINT_FLAG_MAX; flag++) {
			any_constraint_enabled = any_constraint_enabled || g6dof_flags[axis][flag];
		}
		linear_locked[axis] = g6dof_flags[axis][PS3DE::G6DOF_JOINT_FLAG_ENABLE_LINEAR_LIMIT] &&
				_limit_locks(g6dof_params[axis][PS3DE::G6DOF_JOINT_LINEAR_LOWER_LIMIT], g6dof_params[axis][PS3DE::G6DOF_JOINT_LINEAR_UPPER_LIMIT]);
		angular_locked[axis] = g6dof_flags[axis][PS3DE::G6DOF_JOINT_FLAG_ENABLE_ANGULAR_LIMIT] &&
				_limit_locks(g6dof_params[axis][PS3DE::G6DOF_JOINT_ANGULAR_LOWER_LIMIT], g6dof_params[axis][PS3DE::G6DOF_JOINT_ANGULAR_UPPER_LIMIT]);
		if (!linear_locked[axis]) {
			free_linear_count++;
			free_linear_axis = axis;
		}
	}
	if (!any_constraint_enabled) {
		return b3_nullJointId;
	}

	const bool all_linear_locked = linear_locked[0] && linear_locked[1] && linear_locked[2];
	const bool all_angular_locked = angular_locked[0] && angular_locked[1] && angular_locked[2];

	if (all_linear_locked && all_angular_locked) {
		b3WeldJointDef def = b3DefaultWeldJointDef();
		_fill_base_def(def.base);
		return b3CreateWeldJoint(space->get_world(), &def);
	}

	if (all_angular_locked && free_linear_count == 1) {
		b3PrismaticJointDef def = b3DefaultPrismaticJointDef();
		_fill_base_def(def.base, _remap_linear_axis_to_x(local_frame_a, free_linear_axis), _remap_linear_axis_to_x(local_frame_b, free_linear_axis));
		const bool limited = g6dof_flags[free_linear_axis][PS3DE::G6DOF_JOINT_FLAG_ENABLE_LINEAR_LIMIT];
		def.enableLimit = limited;
		def.lowerTranslation = (float)g6dof_params[free_linear_axis][PS3DE::G6DOF_JOINT_LINEAR_LOWER_LIMIT];
		def.upperTranslation = (float)g6dof_params[free_linear_axis][PS3DE::G6DOF_JOINT_LINEAR_UPPER_LIMIT];
		def.enableSpring = g6dof_flags[free_linear_axis][PS3DE::G6DOF_JOINT_FLAG_ENABLE_LINEAR_SPRING];
		def.hertz = (float)g6dof_params[free_linear_axis][PS3DE::G6DOF_JOINT_LINEAR_SPRING_STIFFNESS];
		def.dampingRatio = (float)g6dof_params[free_linear_axis][PS3DE::G6DOF_JOINT_LINEAR_SPRING_DAMPING];
		def.enableMotor = g6dof_flags[free_linear_axis][PS3DE::G6DOF_JOINT_FLAG_ENABLE_LINEAR_MOTOR];
		def.motorSpeed = (float)g6dof_params[free_linear_axis][PS3DE::G6DOF_JOINT_LINEAR_MOTOR_TARGET_VELOCITY];
		def.maxMotorForce = (float)g6dof_params[free_linear_axis][PS3DE::G6DOF_JOINT_LINEAR_MOTOR_FORCE_LIMIT];
		return b3CreatePrismaticJoint(space->get_world(), &def);
	}

	if (all_linear_locked) {
		b3SphericalJointDef def = b3DefaultSphericalJointDef();
		_fill_base_def(def.base, _remap_twist_x_to_z(local_frame_a), _remap_twist_x_to_z(local_frame_b));
		const bool has_cone_limits = g6dof_flags[Vector3::AXIS_Y][PS3DE::G6DOF_JOINT_FLAG_ENABLE_ANGULAR_LIMIT] ||
				g6dof_flags[Vector3::AXIS_Z][PS3DE::G6DOF_JOINT_FLAG_ENABLE_ANGULAR_LIMIT];
		if (has_cone_limits) {
			const real_t y = MAX(Math::abs(g6dof_params[Vector3::AXIS_Y][PS3DE::G6DOF_JOINT_ANGULAR_LOWER_LIMIT]), Math::abs(g6dof_params[Vector3::AXIS_Y][PS3DE::G6DOF_JOINT_ANGULAR_UPPER_LIMIT]));
			const real_t z = MAX(Math::abs(g6dof_params[Vector3::AXIS_Z][PS3DE::G6DOF_JOINT_ANGULAR_LOWER_LIMIT]), Math::abs(g6dof_params[Vector3::AXIS_Z][PS3DE::G6DOF_JOINT_ANGULAR_UPPER_LIMIT]));
			def.enableConeLimit = true;
			def.coneAngle = CLAMP((float)MAX(y, z), 0.0f, (float)Math::PI);
		}
		if (g6dof_flags[Vector3::AXIS_X][PS3DE::G6DOF_JOINT_FLAG_ENABLE_ANGULAR_LIMIT]) {
			def.enableTwistLimit = true;
			def.lowerTwistAngle = (float)-g6dof_params[Vector3::AXIS_X][PS3DE::G6DOF_JOINT_ANGULAR_UPPER_LIMIT];
			def.upperTwistAngle = (float)-g6dof_params[Vector3::AXIS_X][PS3DE::G6DOF_JOINT_ANGULAR_LOWER_LIMIT];
		}
		return b3CreateSphericalJoint(space->get_world(), &def);
	}

	WARN_PRINT_ONCE("Box3D: unsupported Generic6DOF joint configuration lowered to weld.");
	b3WeldJointDef def = b3DefaultWeldJointDef();
	_fill_base_def(def.base);
	return b3CreateWeldJoint(space->get_world(), &def);
}

void Box3DJoint3D::_apply_runtime_settings() {
	if (B3_IS_NON_NULL(joint_id)) {
		b3Joint_SetCollideConnected(joint_id, !collision_disabled);
		b3Joint_SetConstraintTuning(joint_id, (float)(constraint_hertz >= 0.0 ? constraint_hertz : (real_t)GLOBAL_GET("physics/box3d/joints/constraint_hertz")),
				(float)(constraint_damping_ratio >= 0.0 ? constraint_damping_ratio : (real_t)GLOBAL_GET("physics/box3d/joints/constraint_damping_ratio")));
		b3Joint_SetForceThreshold(joint_id, (float)force_threshold);
		b3Joint_SetTorqueThreshold(joint_id, (float)torque_threshold);
		if (b3Joint_GetType(joint_id) == b3_sphericalJoint) {
			b3SphericalJoint_EnableSpring(joint_id, spherical_spring_hertz > 0.0);
			b3SphericalJoint_SetSpringHertz(joint_id, (float)spherical_spring_hertz);
			b3SphericalJoint_SetSpringDampingRatio(joint_id, (float)spherical_spring_damping_ratio);
			b3SphericalJoint_SetTargetRotation(joint_id, to_box3d(spherical_target_rotation));
			const bool motor_enabled = spherical_max_motor_torque > 0.0;
			b3SphericalJoint_EnableMotor(joint_id, motor_enabled);
			b3SphericalJoint_SetMotorVelocity(joint_id, to_box3d(spherical_motor_velocity));
			b3SphericalJoint_SetMaxMotorTorque(joint_id, (float)spherical_max_motor_torque);
		}
	}
}

void Box3DJoint3D::set_collision_disabled(bool p_disabled) {
	if (collision_disabled == p_disabled) {
		return;
	}
	collision_disabled = p_disabled;
	if (B3_IS_NON_NULL(joint_id)) {
		b3Joint_SetCollideConnected(joint_id, !collision_disabled);
	}
}

void Box3DJoint3D::set_solver_priority(int p_priority) {
	solver_priority = p_priority;
	if (p_priority != 1) {
		WARN_PRINT_ONCE("Box3D: joint solver priority is not supported and will be ignored.");
	}
}

void Box3DJoint3D::set_box3d_param(int p_param, real_t p_value) {
	switch (p_param) {
		case Box3DPhysicsServer3D::BOX3D_JOINT_CONSTRAINT_HERTZ:
			constraint_hertz = p_value;
			break;
		case Box3DPhysicsServer3D::BOX3D_JOINT_CONSTRAINT_DAMPING_RATIO:
			constraint_damping_ratio = p_value;
			break;
		case Box3DPhysicsServer3D::BOX3D_JOINT_FORCE_THRESHOLD:
			force_threshold = p_value;
			break;
		case Box3DPhysicsServer3D::BOX3D_JOINT_TORQUE_THRESHOLD:
			torque_threshold = p_value;
			break;
		case Box3DPhysicsServer3D::BOX3D_JOINT_SPHERICAL_SPRING_HERTZ:
			spherical_spring_hertz = p_value;
			break;
		case Box3DPhysicsServer3D::BOX3D_JOINT_SPHERICAL_SPRING_DAMPING_RATIO:
			spherical_spring_damping_ratio = p_value;
			break;
		case Box3DPhysicsServer3D::BOX3D_JOINT_SPHERICAL_MAX_MOTOR_TORQUE:
			spherical_max_motor_torque = p_value;
			break;
		default:
			ERR_FAIL_MSG("Box3D: unknown Box3D joint parameter.");
	}
	_apply_runtime_settings();
}

real_t Box3DJoint3D::get_box3d_param(int p_param) const {
	switch (p_param) {
		case Box3DPhysicsServer3D::BOX3D_JOINT_CONSTRAINT_HERTZ:
			return constraint_hertz;
		case Box3DPhysicsServer3D::BOX3D_JOINT_CONSTRAINT_DAMPING_RATIO:
			return constraint_damping_ratio;
		case Box3DPhysicsServer3D::BOX3D_JOINT_FORCE_THRESHOLD:
			return force_threshold;
		case Box3DPhysicsServer3D::BOX3D_JOINT_TORQUE_THRESHOLD:
			return torque_threshold;
		case Box3DPhysicsServer3D::BOX3D_JOINT_SPHERICAL_SPRING_HERTZ:
			return spherical_spring_hertz;
		case Box3DPhysicsServer3D::BOX3D_JOINT_SPHERICAL_SPRING_DAMPING_RATIO:
			return spherical_spring_damping_ratio;
		case Box3DPhysicsServer3D::BOX3D_JOINT_SPHERICAL_MAX_MOTOR_TORQUE:
			return spherical_max_motor_torque;
		default:
			ERR_FAIL_V_MSG(0.0, "Box3D: unknown Box3D joint parameter.");
	}
}

void Box3DJoint3D::set_box3d_target_rotation(const Quaternion &p_target_rotation) {
	spherical_target_rotation = p_target_rotation;
	_apply_runtime_settings();
}

void Box3DJoint3D::set_box3d_motor_velocity(const Vector3 &p_velocity) {
	spherical_motor_velocity = p_velocity;
	_apply_runtime_settings();
}

void Box3DJoint3D::destroy_box3d_joint() {
	if (B3_IS_NON_NULL(joint_id)) {
		b3DestroyJoint(joint_id, true);
		joint_id = b3_nullJointId;
	}
}

void Box3DJoint3D::rebuild() {
	destroy_box3d_joint();
	if (!_can_build()) {
		return;
	}
	switch (type) {
		case PS3DE::JOINT_TYPE_PIN:
			joint_id = _create_pin();
			break;
		case PS3DE::JOINT_TYPE_HINGE:
			joint_id = _create_hinge();
			break;
		case PS3DE::JOINT_TYPE_SLIDER:
			joint_id = _create_slider();
			break;
		case PS3DE::JOINT_TYPE_CONE_TWIST:
			joint_id = _create_cone_twist();
			break;
		case PS3DE::JOINT_TYPE_6DOF:
			joint_id = _create_generic_6dof();
			break;
		default:
			break;
	}
	_apply_runtime_settings();
}

void Box3DJoint3D::clear() {
	destroy_box3d_joint();
	if (body_a != nullptr) {
		body_a->remove_joint(this);
	}
	if (body_b != nullptr) {
		body_b->remove_joint(this);
	}
	body_a = nullptr;
	body_b = nullptr;
	body_a_rid = RID();
	body_b_rid = RID();
	collision_disabled = false;
	type = PS3DE::JOINT_TYPE_MAX;
	local_frame_a = Transform3D();
	local_frame_b = Transform3D();
}

void Box3DJoint3D::make_pin(Box3DBody3D *p_body_a, Box3DBody3D *p_body_b, const Vector3 &p_local_a, const Vector3 &p_local_b) {
	type = PS3DE::JOINT_TYPE_PIN;
	local_frame_a = _frame_from_point(p_local_a);
	local_frame_b = _frame_from_point(p_local_b);
	_set_bodies(p_body_a, p_body_b);
	rebuild();
}

void Box3DJoint3D::make_hinge(Box3DBody3D *p_body_a, Box3DBody3D *p_body_b, const Transform3D &p_frame_a, const Transform3D &p_frame_b) {
	type = PS3DE::JOINT_TYPE_HINGE;
	local_frame_a = p_frame_a;
	local_frame_b = p_frame_b;
	_set_bodies(p_body_a, p_body_b);
	rebuild();
}

void Box3DJoint3D::make_slider(Box3DBody3D *p_body_a, Box3DBody3D *p_body_b, const Transform3D &p_frame_a, const Transform3D &p_frame_b) {
	type = PS3DE::JOINT_TYPE_SLIDER;
	local_frame_a = p_frame_a;
	local_frame_b = p_frame_b;
	_set_bodies(p_body_a, p_body_b);
	rebuild();
}

void Box3DJoint3D::make_cone_twist(Box3DBody3D *p_body_a, Box3DBody3D *p_body_b, const Transform3D &p_frame_a, const Transform3D &p_frame_b) {
	type = PS3DE::JOINT_TYPE_CONE_TWIST;
	local_frame_a = p_frame_a;
	local_frame_b = p_frame_b;
	_set_bodies(p_body_a, p_body_b);
	rebuild();
}

void Box3DJoint3D::make_generic_6dof(Box3DBody3D *p_body_a, Box3DBody3D *p_body_b, const Transform3D &p_frame_a, const Transform3D &p_frame_b) {
	type = PS3DE::JOINT_TYPE_6DOF;
	local_frame_a = p_frame_a;
	local_frame_b = p_frame_b;
	_set_bodies(p_body_a, p_body_b);
	rebuild();
}

void Box3DJoint3D::pin_set_param(PS3DE::PinJointParam p_param, real_t p_value) {
	ERR_FAIL_INDEX(p_param, PIN_JOINT_PARAM_COUNT);
	pin_params[p_param] = p_value;
	WARN_PRINT_ONCE("Box3D: PinJoint bias/damping/impulse clamp are not supported and will be ignored.");
}

real_t Box3DJoint3D::pin_get_param(PS3DE::PinJointParam p_param) const {
	ERR_FAIL_INDEX_V(p_param, PIN_JOINT_PARAM_COUNT, 0.0);
	return pin_params[p_param];
}

void Box3DJoint3D::pin_set_local_a(const Vector3 &p_local_a) {
	local_frame_a.origin = p_local_a;
	rebuild();
}

Vector3 Box3DJoint3D::pin_get_local_a() const {
	return local_frame_a.origin;
}

void Box3DJoint3D::pin_set_local_b(const Vector3 &p_local_b) {
	local_frame_b.origin = p_local_b;
	rebuild();
}

Vector3 Box3DJoint3D::pin_get_local_b() const {
	return local_frame_b.origin;
}

void Box3DJoint3D::hinge_set_param(PS3DE::HingeJointParam p_param, real_t p_value) {
	ERR_FAIL_INDEX(p_param, PS3DE::HINGE_JOINT_MAX);
	hinge_params[p_param] = p_value;
	switch (p_param) {
		case PS3DE::HINGE_JOINT_LIMIT_UPPER:
		case PS3DE::HINGE_JOINT_LIMIT_LOWER:
			if (B3_IS_NON_NULL(joint_id)) {
				b3RevoluteJoint_SetLimits(joint_id, _hinge_box3d_lower_limit(), _hinge_box3d_upper_limit());
			}
			break;
		case PS3DE::HINGE_JOINT_MOTOR_TARGET_VELOCITY:
			if (B3_IS_NON_NULL(joint_id)) {
				b3RevoluteJoint_SetMotorSpeed(joint_id, _hinge_box3d_motor_speed());
			}
			break;
		case PS3DE::HINGE_JOINT_MOTOR_MAX_IMPULSE:
			if (B3_IS_NON_NULL(joint_id)) {
				b3RevoluteJoint_SetMaxMotorTorque(joint_id, _max_motor_torque_from_impulse(p_value));
			}
			break;
		default:
			WARN_PRINT_ONCE("Box3D: HingeJoint softness/bias/relaxation parameters are not supported and will be ignored.");
			break;
	}
}

real_t Box3DJoint3D::hinge_get_param(PS3DE::HingeJointParam p_param) const {
	ERR_FAIL_INDEX_V(p_param, PS3DE::HINGE_JOINT_MAX, 0.0);
	return hinge_params[p_param];
}

void Box3DJoint3D::hinge_set_flag(PS3DE::HingeJointFlag p_flag, bool p_enabled) {
	ERR_FAIL_INDEX(p_flag, PS3DE::HINGE_JOINT_FLAG_MAX);
	hinge_flags[p_flag] = p_enabled;
	if (B3_IS_NON_NULL(joint_id)) {
		if (p_flag == PS3DE::HINGE_JOINT_FLAG_USE_LIMIT) {
			b3RevoluteJoint_EnableLimit(joint_id, p_enabled);
		} else if (p_flag == PS3DE::HINGE_JOINT_FLAG_ENABLE_MOTOR) {
			b3RevoluteJoint_EnableMotor(joint_id, p_enabled);
		}
	}
}

bool Box3DJoint3D::hinge_get_flag(PS3DE::HingeJointFlag p_flag) const {
	ERR_FAIL_INDEX_V(p_flag, PS3DE::HINGE_JOINT_FLAG_MAX, false);
	return hinge_flags[p_flag];
}

void Box3DJoint3D::slider_set_param(PS3DE::SliderJointParam p_param, real_t p_value) {
	ERR_FAIL_INDEX(p_param, PS3DE::SLIDER_JOINT_MAX);
	slider_params[p_param] = p_value;
	if (p_param == PS3DE::SLIDER_JOINT_LINEAR_LIMIT_LOWER || p_param == PS3DE::SLIDER_JOINT_LINEAR_LIMIT_UPPER) {
		if (B3_IS_NON_NULL(joint_id)) {
			b3PrismaticJoint_SetLimits(joint_id, (float)slider_params[PS3DE::SLIDER_JOINT_LINEAR_LIMIT_LOWER], (float)slider_params[PS3DE::SLIDER_JOINT_LINEAR_LIMIT_UPPER]);
		}
	} else if (p_param >= PS3DE::SLIDER_JOINT_ANGULAR_LIMIT_UPPER) {
		WARN_PRINT_ONCE("Box3D: SliderJoint angular parameters are not supported and will be ignored.");
	}
}

real_t Box3DJoint3D::slider_get_param(PS3DE::SliderJointParam p_param) const {
	ERR_FAIL_INDEX_V(p_param, PS3DE::SLIDER_JOINT_MAX, 0.0);
	return slider_params[p_param];
}

void Box3DJoint3D::cone_twist_set_param(PS3DE::ConeTwistJointParam p_param, real_t p_value) {
	ERR_FAIL_INDEX(p_param, PS3DE::CONE_TWIST_MAX);
	cone_params[p_param] = p_value;
	if (p_param == PS3DE::CONE_TWIST_JOINT_SWING_SPAN || p_param == PS3DE::CONE_TWIST_JOINT_TWIST_SPAN) {
		if (type == PS3DE::JOINT_TYPE_CONE_TWIST) {
			rebuild();
		}
	} else {
		WARN_PRINT_ONCE("Box3D: ConeTwistJoint bias/softness/relaxation parameters are not supported and will be ignored.");
	}
}

real_t Box3DJoint3D::cone_twist_get_param(PS3DE::ConeTwistJointParam p_param) const {
	ERR_FAIL_INDEX_V(p_param, PS3DE::CONE_TWIST_MAX, 0.0);
	return cone_params[p_param];
}

void Box3DJoint3D::generic_6dof_set_param(Vector3::Axis p_axis, PS3DE::G6DOFJointAxisParam p_param, real_t p_value) {
	ERR_FAIL_INDEX(p_axis, 3);
	ERR_FAIL_INDEX(p_param, PS3DE::G6DOF_JOINT_MAX);
	g6dof_params[p_axis][p_param] = p_value;
	if (type == PS3DE::JOINT_TYPE_6DOF) {
		rebuild();
	}
}

real_t Box3DJoint3D::generic_6dof_get_param(Vector3::Axis p_axis, PS3DE::G6DOFJointAxisParam p_param) const {
	ERR_FAIL_INDEX_V(p_axis, 3, 0.0);
	ERR_FAIL_INDEX_V(p_param, PS3DE::G6DOF_JOINT_MAX, 0.0);
	return g6dof_params[p_axis][p_param];
}

void Box3DJoint3D::generic_6dof_set_flag(Vector3::Axis p_axis, PS3DE::G6DOFJointAxisFlag p_flag, bool p_enabled) {
	ERR_FAIL_INDEX(p_axis, 3);
	ERR_FAIL_INDEX(p_flag, PS3DE::G6DOF_JOINT_FLAG_MAX);
	g6dof_flags[p_axis][p_flag] = p_enabled;
	if (type == PS3DE::JOINT_TYPE_6DOF) {
		rebuild();
	}
}

bool Box3DJoint3D::generic_6dof_get_flag(Vector3::Axis p_axis, PS3DE::G6DOFJointAxisFlag p_flag) const {
	ERR_FAIL_INDEX_V(p_axis, 3, false);
	ERR_FAIL_INDEX_V(p_flag, PS3DE::G6DOF_JOINT_FLAG_MAX, false);
	return g6dof_flags[p_axis][p_flag];
}
