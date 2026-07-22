/**************************************************************************/
/*  box3d_physics_server_3d.h — PhysicsServer3D backed by Box3D           */
/*                                                                        */
/*  Milestone 1 (static vertical slice + rigid dynamics): spaces, bodies, */
/*  shapes, state sync. Subclasses PhysicsServer3DDummy so unimplemented  */
/*  surface area no-ops safely; methods migrate to real implementations   */
/*  per the scope order in <box3d repo>/llm/06-project-decisions.md.      */
/**************************************************************************/

#pragma once

#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"
#include "core/templates/rid_owner.h"
#include "servers/physics_3d/physics_server_3d_dummy.h"

#include "box3d_area_3d.h"
#include "box3d_body_3d.h"
#include "box3d_shape_3d.h"
#include "box3d_space_3d.h"
#include "joints/box3d_joint_3d.h"

// Box3D does not simulate soft bodies (see WARN_PRINT_ONCE in soft_body_set_space()).
// This placeholder exists only so soft_body_create() can hand out a real, owned RID —
// like every other Box3D RID — instead of the PhysicsServer3DDummy stub's bare RID(),
// which body_attach_object_instance_id() and free_rid() would otherwise reject as
// invalid. Mirrors GodotPhysicsServer3D's soft_body_owner minus simulation state.
class Box3DSoftBodyPlaceholder {
	RID rid;
	ObjectID instance_id;

public:
	void set_rid(const RID &p_rid) { rid = p_rid; }
	RID get_rid() const { return rid; }
	void set_instance_id(const ObjectID &p_id) { instance_id = p_id; }
	ObjectID get_instance_id() const { return instance_id; }
};

class Box3DPhysicsServer3D : public PhysicsServer3DDummy {
	GDCLASS(Box3DPhysicsServer3D, PhysicsServer3DDummy);

	inline static Box3DPhysicsServer3D *singleton = nullptr;

	mutable RID_PtrOwner<Box3DSpace3D> space_owner;
	mutable RID_PtrOwner<Box3DArea3D> area_owner;
	mutable RID_PtrOwner<Box3DBody3D> body_owner;
	mutable RID_PtrOwner<Box3DShape3D> shape_owner;
	mutable RID_PtrOwner<Box3DJoint3D> joint_owner;
	mutable RID_PtrOwner<Box3DSoftBodyPlaceholder> soft_body_owner;

	HashSet<Box3DSpace3D *> active_spaces;

	bool active = true;
	bool flushing_queries = false;
	bool using_threads = false;
	bool doing_sync = false;

	RID _create_shape(PS3DE::ShapeType p_type);
	bool _can_mutate_body_shapes(const Box3DBody3D *p_body) const;
	bool _can_mutate_shape_owners(const Box3DShape3D *p_shape) const;
	bool _can_mutate_joint(const Box3DJoint3D *p_joint) const;
	bool _can_mutate_joint_bodies(const Box3DBody3D *p_body_a, const Box3DBody3D *p_body_b) const;

public:
	enum Box3DJointParam {
		BOX3D_JOINT_CONSTRAINT_HERTZ = 100,
		BOX3D_JOINT_CONSTRAINT_DAMPING_RATIO,
		BOX3D_JOINT_FORCE_THRESHOLD,
		BOX3D_JOINT_TORQUE_THRESHOLD,
		BOX3D_JOINT_SPHERICAL_SPRING_HERTZ,
		BOX3D_JOINT_SPHERICAL_SPRING_DAMPING_RATIO,
		BOX3D_JOINT_SPHERICAL_MAX_MOTOR_TORQUE,
	};

	explicit Box3DPhysicsServer3D(bool p_using_threads = false);
	~Box3DPhysicsServer3D();

	static Box3DPhysicsServer3D *get_singleton() { return singleton; }
	Box3DSpace3D *get_space(RID p_rid) const { return space_owner.get_or_null(p_rid); }
	Box3DBody3D *get_body(RID p_rid) const { return body_owner.get_or_null(p_rid); }
	bool can_access_space(Box3DSpace3D *p_space) const { return p_space != nullptr && !(using_threads && !doing_sync) && !p_space->is_stepping(); }

	// Shapes.
	virtual RID world_boundary_shape_create() override { return _create_shape(PS3DE::SHAPE_WORLD_BOUNDARY); }
	virtual RID separation_ray_shape_create() override { return _create_shape(PS3DE::SHAPE_SEPARATION_RAY); }
	virtual RID sphere_shape_create() override { return _create_shape(PS3DE::SHAPE_SPHERE); }
	virtual RID box_shape_create() override { return _create_shape(PS3DE::SHAPE_BOX); }
	virtual RID capsule_shape_create() override { return _create_shape(PS3DE::SHAPE_CAPSULE); }
	virtual RID cylinder_shape_create() override { return _create_shape(PS3DE::SHAPE_CYLINDER); }
	virtual RID convex_polygon_shape_create() override { return _create_shape(PS3DE::SHAPE_CONVEX_POLYGON); }
	virtual RID concave_polygon_shape_create() override { return _create_shape(PS3DE::SHAPE_CONCAVE_POLYGON); }
	virtual RID heightmap_shape_create() override { return _create_shape(PS3DE::SHAPE_HEIGHTMAP); }
	virtual RID custom_shape_create() override;
	virtual void shape_set_data(RID p_shape, const Variant &p_data) override;
	virtual Variant shape_get_data(RID p_shape) const override;
	virtual PS3DE::ShapeType shape_get_type(RID p_shape) const override;
	void shape_set_surface_material(RID p_shape, int p_material_id);
	void shape_set_surface_map(RID p_shape, const PackedInt64Array &p_material_ids, const PackedByteArray &p_triangle_indices);
	int shape_get_face_material_id(RID p_shape, int p_face_index) const;
	PackedByteArray shape_get_mesh_material_indices(RID p_shape) const;

	// Spaces.
	virtual RID space_create() override;
	virtual void space_set_active(RID p_space, bool p_active) override;
	virtual bool space_is_active(RID p_space) const override;
	virtual PhysicsDirectSpaceState3D *space_get_direct_state(RID p_space) override;
	bool space_start_recording(RID p_space, int p_byte_capacity);
	PackedByteArray space_stop_recording(RID p_space);
	bool space_is_recording(RID p_space) const;
	int space_get_recording_size(RID p_space) const;
	bool space_save_recording(RID p_space, const String &p_path) const;

	// Areas.
	virtual RID area_create() override;
	virtual void area_set_space(RID p_area, RID p_space) override;
	virtual RID area_get_space(RID p_area) const override;
	virtual void area_add_shape(RID p_area, RID p_shape, const Transform3D &p_transform = Transform3D(), bool p_disabled = false) override;
	virtual void area_set_shape(RID p_area, int p_shape_idx, RID p_shape) override;
	virtual void area_set_shape_transform(RID p_area, int p_shape_idx, const Transform3D &p_transform) override;
	virtual int area_get_shape_count(RID p_area) const override;
	virtual RID area_get_shape(RID p_area, int p_shape_idx) const override;
	virtual Transform3D area_get_shape_transform(RID p_area, int p_shape_idx) const override;
	virtual void area_remove_shape(RID p_area, int p_shape_idx) override;
	virtual void area_clear_shapes(RID p_area) override;
	virtual void area_set_shape_disabled(RID p_area, int p_shape_idx, bool p_disabled) override;
	virtual void area_attach_object_instance_id(RID p_area, ObjectID p_id) override;
	virtual ObjectID area_get_object_instance_id(RID p_area) const override;
	virtual void area_set_param(RID p_area, PS3DE::AreaParameter p_param, const Variant &p_value) override;
	virtual Variant area_get_param(RID p_area, PS3DE::AreaParameter p_param) const override;
	virtual void area_set_transform(RID p_area, const Transform3D &p_transform) override;
	virtual Transform3D area_get_transform(RID p_area) const override;
	virtual void area_set_collision_layer(RID p_area, uint32_t p_layer) override;
	virtual uint32_t area_get_collision_layer(RID p_area) const override;
	virtual void area_set_collision_mask(RID p_area, uint32_t p_mask) override;
	virtual uint32_t area_get_collision_mask(RID p_area) const override;
	virtual void area_set_monitorable(RID p_area, bool p_monitorable) override;
	virtual void area_set_monitor_callback(RID p_area, const Callable &p_callback) override;
	virtual void area_set_area_monitor_callback(RID p_area, const Callable &p_callback) override;
	virtual void area_set_ray_pickable(RID p_area, bool p_enable) override;

	// Bodies.
	virtual RID body_create() override;
	virtual void body_set_space(RID p_body, RID p_space) override;
	virtual RID body_get_space(RID p_body) const override;
	virtual void body_set_mode(RID p_body, PS3DE::BodyMode p_mode) override;
	virtual PS3DE::BodyMode body_get_mode(RID p_body) const override;
	virtual void body_add_shape(RID p_body, RID p_shape, const Transform3D &p_transform = Transform3D(), bool p_disabled = false) override;
	virtual void body_set_shape(RID p_body, int p_shape_idx, RID p_shape) override;
	virtual void body_set_shape_transform(RID p_body, int p_shape_idx, const Transform3D &p_transform) override;
	virtual void body_set_shape_disabled(RID p_body, int p_shape_idx, bool p_disabled) override;
	void body_set_surface_material(RID p_body, int p_shape_idx, int p_material_id);
	virtual int body_get_shape_count(RID p_body) const override;
	virtual RID body_get_shape(RID p_body, int p_shape_idx) const override;
	virtual Transform3D body_get_shape_transform(RID p_body, int p_shape_idx) const override;
	virtual void body_remove_shape(RID p_body, int p_shape_idx) override;
	virtual void body_clear_shapes(RID p_body) override;
	virtual void body_attach_object_instance_id(RID p_body, ObjectID p_id) override;
	virtual ObjectID body_get_object_instance_id(RID p_body) const override;
	virtual void body_set_collision_layer(RID p_body, uint32_t p_layer) override;
	virtual uint32_t body_get_collision_layer(RID p_body) const override;
	virtual void body_set_collision_mask(RID p_body, uint32_t p_mask) override;
	virtual uint32_t body_get_collision_mask(RID p_body) const override;
	virtual void body_set_ray_pickable(RID p_body, bool p_enable) override;
	virtual void body_add_collision_exception(RID p_body, RID p_body_b) override;
	virtual void body_remove_collision_exception(RID p_body, RID p_body_b) override;
	virtual void body_get_collision_exceptions(RID p_body, List<RID> *p_exceptions) override;
	virtual void body_set_enable_continuous_collision_detection(RID p_body, bool p_enable) override;
	virtual void body_set_param(RID p_body, PS3DE::BodyParameter p_param, const Variant &p_value) override;
	virtual Variant body_get_param(RID p_body, PS3DE::BodyParameter p_param) const override;
	virtual void body_set_state(RID p_body, PS3DE::BodyState p_state, const Variant &p_variant) override;
	virtual Variant body_get_state(RID p_body, PS3DE::BodyState p_state) const override;
	virtual void body_apply_central_impulse(RID p_body, const Vector3 &p_impulse) override;
	virtual void body_apply_impulse(RID p_body, const Vector3 &p_impulse, const Vector3 &p_position = Vector3()) override;
	virtual void body_apply_torque_impulse(RID p_body, const Vector3 &p_impulse) override;
	virtual void body_apply_central_force(RID p_body, const Vector3 &p_force) override;
	virtual void body_apply_force(RID p_body, const Vector3 &p_force, const Vector3 &p_position = Vector3()) override;
	virtual void body_apply_torque(RID p_body, const Vector3 &p_torque) override;
	virtual void body_add_constant_central_force(RID p_body, const Vector3 &p_force) override;
	virtual void body_add_constant_force(RID p_body, const Vector3 &p_force, const Vector3 &p_position = Vector3()) override;
	virtual void body_add_constant_torque(RID p_body, const Vector3 &p_torque) override;
	virtual void body_set_constant_force(RID p_body, const Vector3 &p_force) override;
	virtual Vector3 body_get_constant_force(RID p_body) const override;
	virtual void body_set_constant_torque(RID p_body, const Vector3 &p_torque) override;
	virtual Vector3 body_get_constant_torque(RID p_body) const override;
	virtual void body_set_max_contacts_reported(RID p_body, int p_contacts) override;
	virtual int body_get_max_contacts_reported(RID p_body) const override;
	virtual void body_set_contacts_reported_depth_threshold(RID p_body, real_t p_threshold) override;
	virtual real_t body_get_contacts_reported_depth_threshold(RID p_body) const override;
	virtual void body_set_omit_force_integration(RID p_body, bool p_omit) override;
	virtual bool body_is_omitting_force_integration(RID p_body) const override;
	virtual void body_set_state_sync_callback(RID p_body, const Callable &p_callable) override;
	virtual void body_set_force_integration_callback(RID p_body, const Callable &p_callable, const Variant &p_udata = Variant()) override;
	virtual PhysicsDirectBodyState3D *body_get_direct_state(RID p_body) override;
	virtual bool body_test_motion(RID p_body, const PS3DT::MotionParameters &p_parameters, PS3DT::MotionResult *r_result = nullptr) override;

	// Joints.
	virtual RID joint_create() override;
	virtual void joint_clear(RID p_joint) override;
	virtual PS3DE::JointType joint_get_type(RID p_joint) const override;
	virtual void joint_set_solver_priority(RID p_joint, int p_priority) override;
	virtual int joint_get_solver_priority(RID p_joint) const override;
	virtual void joint_disable_collisions_between_bodies(RID p_joint, bool p_disable) override;
	virtual bool joint_is_disabled_collisions_between_bodies(RID p_joint) const override;
	virtual void joint_make_pin(RID p_joint, RID p_body_A, const Vector3 &p_local_A, RID p_body_B, const Vector3 &p_local_B) override;
	virtual void pin_joint_set_param(RID p_joint, PS3DE::PinJointParam p_param, real_t p_value) override;
	virtual real_t pin_joint_get_param(RID p_joint, PS3DE::PinJointParam p_param) const override;
	virtual void pin_joint_set_local_a(RID p_joint, const Vector3 &p_A) override;
	virtual Vector3 pin_joint_get_local_a(RID p_joint) const override;
	virtual void pin_joint_set_local_b(RID p_joint, const Vector3 &p_B) override;
	virtual Vector3 pin_joint_get_local_b(RID p_joint) const override;
	virtual void joint_make_hinge(RID p_joint, RID p_body_A, const Transform3D &p_hinge_A, RID p_body_B, const Transform3D &p_hinge_B) override;
	virtual void joint_make_hinge_simple(RID p_joint, RID p_body_A, const Vector3 &p_pivot_A, const Vector3 &p_axis_A, RID p_body_B, const Vector3 &p_pivot_B, const Vector3 &p_axis_B) override;
	virtual void hinge_joint_set_param(RID p_joint, PS3DE::HingeJointParam p_param, real_t p_value) override;
	virtual real_t hinge_joint_get_param(RID p_joint, PS3DE::HingeJointParam p_param) const override;
	virtual void hinge_joint_set_flag(RID p_joint, PS3DE::HingeJointFlag p_flag, bool p_enabled) override;
	virtual bool hinge_joint_get_flag(RID p_joint, PS3DE::HingeJointFlag p_flag) const override;
	virtual void joint_make_slider(RID p_joint, RID p_body_A, const Transform3D &p_local_frame_A, RID p_body_B, const Transform3D &p_local_frame_B) override;
	virtual void slider_joint_set_param(RID p_joint, PS3DE::SliderJointParam p_param, real_t p_value) override;
	virtual real_t slider_joint_get_param(RID p_joint, PS3DE::SliderJointParam p_param) const override;
	virtual void joint_make_cone_twist(RID p_joint, RID p_body_A, const Transform3D &p_local_frame_A, RID p_body_B, const Transform3D &p_local_frame_B) override;
	virtual void cone_twist_joint_set_param(RID p_joint, PS3DE::ConeTwistJointParam p_param, real_t p_value) override;
	virtual real_t cone_twist_joint_get_param(RID p_joint, PS3DE::ConeTwistJointParam p_param) const override;
	virtual void joint_make_generic_6dof(RID p_joint, RID p_body_A, const Transform3D &p_local_frame_A, RID p_body_B, const Transform3D &p_local_frame_B) override;
	virtual void generic_6dof_joint_set_param(RID p_joint, Vector3::Axis p_axis, PS3DE::G6DOFJointAxisParam p_param, real_t p_value) override;
	virtual real_t generic_6dof_joint_get_param(RID p_joint, Vector3::Axis p_axis, PS3DE::G6DOFJointAxisParam p_param) const override;
	virtual void generic_6dof_joint_set_flag(RID p_joint, Vector3::Axis p_axis, PS3DE::G6DOFJointAxisFlag p_flag, bool p_enable) override;
	virtual bool generic_6dof_joint_get_flag(RID p_joint, Vector3::Axis p_axis, PS3DE::G6DOFJointAxisFlag p_flag) const override;
	void joint_set_box3d_param(RID p_joint, Box3DJointParam p_param, real_t p_value);
	real_t joint_get_box3d_param(RID p_joint, Box3DJointParam p_param) const;
	void joint_set_box3d_target_rotation(RID p_joint, const Quaternion &p_target_rotation);
	Quaternion joint_get_box3d_target_rotation(RID p_joint) const;
	void joint_set_box3d_motor_velocity(RID p_joint, const Vector3 &p_velocity);
	Vector3 joint_get_box3d_motor_velocity(RID p_joint) const;

	// Soft bodies. Not simulated; see Box3DSoftBodyPlaceholder above.
	virtual RID soft_body_create() override;
	virtual void soft_body_set_space(RID p_body, RID p_space) override;

	// Lifecycle.
	virtual void free_rid(RID p_rid) override;
	virtual void set_active(bool p_active) override { active = p_active; }
	virtual void step(real_t p_step) override;
	virtual void sync() override { doing_sync = true; }
	virtual void flush_queries() override;
	virtual void end_sync() override { doing_sync = false; }
	virtual bool is_flushing_queries() const override { return flushing_queries; }
};

VARIANT_ENUM_CAST(Box3DPhysicsServer3D::Box3DJointParam);
