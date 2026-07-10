/**************************************************************************/
/*  box3d_joint_3d.h                                                       */
/**************************************************************************/

#pragma once

#include "servers/physics_3d/physics_server_3d.h"

#include "box3d/box3d.h"

class Box3DBody3D;
class Box3DSpace3D;

class Box3DJoint3D {
	RID rid;
	PhysicsServer3D::JointType type = PhysicsServer3D::JOINT_TYPE_MAX;

	Box3DBody3D *body_a = nullptr;
	Box3DBody3D *body_b = nullptr;
	RID body_a_rid;
	RID body_b_rid;

	Transform3D local_frame_a;
	Transform3D local_frame_b;
	b3JointId joint_id = b3_nullJointId;

	bool collision_disabled = false;
	int solver_priority = 1;
	real_t constraint_hertz = -1.0;
	real_t constraint_damping_ratio = -1.0;
	real_t force_threshold = 0.0;
	real_t torque_threshold = 0.0;
	real_t spherical_spring_hertz = 0.0;
	real_t spherical_spring_damping_ratio = 0.7;
	Quaternion spherical_target_rotation;
	Vector3 spherical_motor_velocity;
	real_t spherical_max_motor_torque = 0.0;

	static constexpr int PIN_JOINT_PARAM_COUNT = 3;
	real_t pin_params[PIN_JOINT_PARAM_COUNT] = {};
	real_t hinge_params[PhysicsServer3D::HINGE_JOINT_MAX] = {};
	bool hinge_flags[PhysicsServer3D::HINGE_JOINT_FLAG_MAX] = {};
	real_t slider_params[PhysicsServer3D::SLIDER_JOINT_MAX] = {};
	real_t cone_params[PhysicsServer3D::CONE_TWIST_MAX] = {};
	real_t g6dof_params[3][PhysicsServer3D::G6DOF_JOINT_MAX] = {};
	bool g6dof_flags[3][PhysicsServer3D::G6DOF_JOINT_FLAG_MAX] = {};

	void _set_bodies(Box3DBody3D *p_body_a, Box3DBody3D *p_body_b);
	bool _can_build() const;
	Box3DSpace3D *_get_joint_space() const;
	void _fill_base_def(b3JointDef &r_def);
	void _fill_base_def(b3JointDef &r_def, const Transform3D &p_frame_a, const Transform3D &p_frame_b);
	float _max_motor_torque_from_impulse(real_t p_impulse) const;
	float _hinge_box3d_lower_limit() const;
	float _hinge_box3d_upper_limit() const;
	float _hinge_box3d_motor_speed() const;
	Transform3D _remap_twist_x_to_z(const Transform3D &p_frame) const;
	Transform3D _remap_linear_axis_to_x(const Transform3D &p_frame, int p_axis) const;

	b3JointId _create_pin();
	b3JointId _create_hinge();
	b3JointId _create_slider();
	b3JointId _create_cone_twist();
	b3JointId _create_generic_6dof();

	void _apply_runtime_settings();

public:
	Box3DJoint3D();
	~Box3DJoint3D();

	void set_rid(const RID &p_rid) { rid = p_rid; }
	RID get_rid() const { return rid; }

	PhysicsServer3D::JointType get_type() const { return type; }
	Box3DBody3D *get_body_a() const { return body_a; }
	Box3DBody3D *get_body_b() const { return body_b; }
	bool has_body(const Box3DBody3D *p_body) const { return body_a == p_body || body_b == p_body; }

	bool is_collision_disabled() const { return collision_disabled; }
	void set_collision_disabled(bool p_disabled);

	int get_solver_priority() const { return solver_priority; }
	void set_solver_priority(int p_priority);
	void set_box3d_param(int p_param, real_t p_value);
	real_t get_box3d_param(int p_param) const;
	void set_box3d_target_rotation(const Quaternion &p_target_rotation);
	Quaternion get_box3d_target_rotation() const { return spherical_target_rotation; }
	void set_box3d_motor_velocity(const Vector3 &p_velocity);
	Vector3 get_box3d_motor_velocity() const { return spherical_motor_velocity; }

	void destroy_box3d_joint();
	void rebuild();
	void clear();

	void make_pin(Box3DBody3D *p_body_a, Box3DBody3D *p_body_b, const Vector3 &p_local_a, const Vector3 &p_local_b);
	void make_hinge(Box3DBody3D *p_body_a, Box3DBody3D *p_body_b, const Transform3D &p_frame_a, const Transform3D &p_frame_b);
	void make_slider(Box3DBody3D *p_body_a, Box3DBody3D *p_body_b, const Transform3D &p_frame_a, const Transform3D &p_frame_b);
	void make_cone_twist(Box3DBody3D *p_body_a, Box3DBody3D *p_body_b, const Transform3D &p_frame_a, const Transform3D &p_frame_b);
	void make_generic_6dof(Box3DBody3D *p_body_a, Box3DBody3D *p_body_b, const Transform3D &p_frame_a, const Transform3D &p_frame_b);

	void pin_set_param(PhysicsServer3D::PinJointParam p_param, real_t p_value);
	real_t pin_get_param(PhysicsServer3D::PinJointParam p_param) const;
	void pin_set_local_a(const Vector3 &p_local_a);
	Vector3 pin_get_local_a() const;
	void pin_set_local_b(const Vector3 &p_local_b);
	Vector3 pin_get_local_b() const;

	void hinge_set_param(PhysicsServer3D::HingeJointParam p_param, real_t p_value);
	real_t hinge_get_param(PhysicsServer3D::HingeJointParam p_param) const;
	void hinge_set_flag(PhysicsServer3D::HingeJointFlag p_flag, bool p_enabled);
	bool hinge_get_flag(PhysicsServer3D::HingeJointFlag p_flag) const;

	void slider_set_param(PhysicsServer3D::SliderJointParam p_param, real_t p_value);
	real_t slider_get_param(PhysicsServer3D::SliderJointParam p_param) const;

	void cone_twist_set_param(PhysicsServer3D::ConeTwistJointParam p_param, real_t p_value);
	real_t cone_twist_get_param(PhysicsServer3D::ConeTwistJointParam p_param) const;

	void generic_6dof_set_param(Vector3::Axis p_axis, PhysicsServer3D::G6DOFJointAxisParam p_param, real_t p_value);
	real_t generic_6dof_get_param(Vector3::Axis p_axis, PhysicsServer3D::G6DOFJointAxisParam p_param) const;
	void generic_6dof_set_flag(Vector3::Axis p_axis, PhysicsServer3D::G6DOFJointAxisFlag p_flag, bool p_enabled);
	bool generic_6dof_get_flag(Vector3::Axis p_axis, PhysicsServer3D::G6DOFJointAxisFlag p_flag) const;
};
