/**************************************************************************/
/*  box3d_body_3d.h — rigid/kinematic/static body wrapping a b3BodyId     */
/**************************************************************************/

#pragma once

#include "core/templates/hash_set.h"
#include "core/templates/local_vector.h"
#include "core/templates/vector.h"
#include "servers/physics_3d/physics_server_3d_dummy.h"

#include "box3d_collision_object_3d.h"

#include "box3d/box3d.h"

class Box3DArea3D;
class Box3DShape3D;
class Box3DSpace3D;
class Box3DDirectBodyState3D;
class Box3DDirectSpaceState3D;
class Box3DJoint3D;

class Box3DBody3D : public Box3DCollisionObject3D {
	friend class Box3DDirectBodyState3D;
	friend class Box3DDirectSpaceState3D;
	friend class Box3DSpace3D;

public:
	struct ShapeSlot {
		RID rid;
		Box3DShape3D *shape = nullptr;
		Transform3D xform;
		bool disabled = false;
		bool has_surface_material = false;
		int surface_material_id = 0;
	};

private:
	RID rid;
	RID space_rid;
	ObjectID instance_id;

	Box3DSpace3D *space = nullptr;
	b3BodyId body_id = {}; // Null id when not in a space.
	LocalVector<b3ShapeId> shape_ids;
	LocalVector<b3MeshData *> instance_meshes;
	LocalVector<ShapeSlot> slots;

	PS3DE::BodyMode mode = PS3DE::BODY_MODE_RIGID;
	Transform3D transform;
	Vector3 linear_velocity_cache;
	Vector3 angular_velocity_cache;
	bool sleeping = false;
	bool can_sleep = true;

	real_t friction = 1.0;
	real_t bounce = 0.0;
	real_t mass = 1.0;
	real_t gravity_scale = 1.0;
	uint32_t collision_layer = 1;
	uint32_t collision_mask = 1;
	int collision_group_index = 0;
	HashSet<RID> collision_exceptions;
	HashSet<Box3DJoint3D *> joints;
	bool continuous_cd = false;

	Vector3 constant_force;
	Vector3 constant_torque;
	real_t linear_damp = 0.0;
	real_t angular_damp = 0.0;
	PS3DE::BodyDampMode linear_damp_mode = PS3DE::BODY_DAMP_MODE_COMBINE;
	PS3DE::BodyDampMode angular_damp_mode = PS3DE::BODY_DAMP_MODE_COMBINE;
	bool omit_force_integration = false;
	real_t total_linear_damp = 0.0;
	real_t total_angular_damp = 0.0;
	Vector3 total_gravity;

	struct AreaRef {
		Box3DArea3D *area = nullptr;
		int ref_count = 0;

		bool operator<(const AreaRef &p_ref) const;
	};
	LocalVector<AreaRef> areas;

	struct Contact {
		Vector3 local_pos;
		Vector3 local_normal;
		Vector3 impulse;
		int local_shape = 0;
		Vector3 local_velocity_at_pos;
		RID collider;
		Vector3 collider_pos;
		ObjectID collider_instance_id;
		int collider_shape = 0;
		Vector3 collider_velocity_at_pos;
		real_t depth = 0.0;
	};
	Vector<Contact> contacts;
	int contact_count = 0;
	real_t contacts_reported_depth_threshold = 0.0;

	Callable state_sync_callback;
	Callable force_integration_callback;
	Variant force_integration_udata;
	Box3DDirectBodyState3D *direct_state = nullptr;

	bool in_dirty_list = false;
	bool has_kinematic_target = false;
	Transform3D kinematic_target;

	void _build_all_shapes();
	void _destroy_all_shapes();
	void _update_mass();
	void _update_shape_filters();
	uint64_t _effective_mask_bits() const;
	b3BodyType _box3d_type() const;
	b3MotionLocks _motion_locks() const;
	bool _slot_has_named_surface_material(uint32_t p_slot) const;

public:
	Box3DBody3D();
	~Box3DBody3D();

	void set_rid(const RID &p_rid) { rid = p_rid; }
	virtual RID get_rid() const override { return rid; }
	void set_space_rid(const RID &p_rid) { space_rid = p_rid; }
	RID get_space_rid() const { return space_rid; }
	b3BodyId get_body_id() const { return body_id; }
	void set_instance_id(ObjectID p_id) { instance_id = p_id; }
	virtual ObjectID get_instance_id() const override { return instance_id; }

	bool in_space() const;
	void set_space(Box3DSpace3D *p_space);
	virtual Box3DSpace3D *get_space() const override { return space; }

	void set_mode(PS3DE::BodyMode p_mode);
	PS3DE::BodyMode get_mode() const { return mode; }

	// Shape list management (server-facing).
	void add_shape(RID p_shape_rid, Box3DShape3D *p_shape, const Transform3D &p_xform, bool p_disabled);
	void remove_shape_at(int p_index);
	void remove_shape(Box3DShape3D *p_shape); // Removes every slot using p_shape.
	void clear_shapes();
	void set_shape(int p_index, RID p_shape_rid, Box3DShape3D *p_shape);
	void set_shape_transform(int p_index, const Transform3D &p_xform);
	void set_shape_disabled(int p_index, bool p_disabled);
	void set_surface_material(int p_shape_idx, int p_material_id);
	int get_shape_count() const { return slots.size(); }
	const ShapeSlot *get_shape_slot(int p_index) const;
	virtual void shapes_changed() override; // Rebuild all b3 shapes (shape data edited, filters changed, ...).

	void set_transform(const Transform3D &p_transform); // Godot PS3DE::BODY_STATE_TRANSFORM semantics per mode.
	Transform3D get_transform() const { return transform; }

	void set_linear_velocity(const Vector3 &p_velocity);
	Vector3 get_linear_velocity() const;
	void set_angular_velocity(const Vector3 &p_velocity);
	Vector3 get_angular_velocity() const;

	void set_sleep_state(bool p_sleeping);
	bool is_sleeping() const { return sleeping; }
	void set_can_sleep(bool p_can_sleep);
	bool get_can_sleep() const { return can_sleep; }

	void set_param(PS3DE::BodyParameter p_param, const Variant &p_value);
	Variant get_param(PS3DE::BodyParameter p_param) const;

	void set_collision_layer(uint32_t p_layer);
	uint32_t get_collision_layer() const { return collision_layer; }
	void set_collision_mask(uint32_t p_mask);
	uint32_t get_collision_mask() const { return collision_mask; }
	void set_collision_group_index(int p_group_index);
	int get_collision_group_index() const { return collision_group_index; }
	void add_collision_exception(RID p_body);
	void remove_collision_exception(RID p_body);
	bool has_collision_exception(RID p_body) const { return collision_exceptions.has(p_body); }
	const HashSet<RID> &get_collision_exceptions() const { return collision_exceptions; }
	void set_enable_continuous_collision_detection(bool p_enable);
	void add_joint(Box3DJoint3D *p_joint) { joints.insert(p_joint); }
	void remove_joint(Box3DJoint3D *p_joint) { joints.erase(p_joint); }
	const HashSet<Box3DJoint3D *> &get_joints() const { return joints; }

	void apply_central_impulse(const Vector3 &p_impulse);
	void apply_impulse(const Vector3 &p_impulse, const Vector3 &p_position);
	void apply_torque_impulse(const Vector3 &p_impulse);
	void apply_central_force(const Vector3 &p_force);
	void apply_force(const Vector3 &p_force, const Vector3 &p_position);
	void apply_torque(const Vector3 &p_torque);
	void add_constant_central_force(const Vector3 &p_force);
	void add_constant_force(const Vector3 &p_force, const Vector3 &p_position);
	void add_constant_torque(const Vector3 &p_torque);
	void set_constant_force(const Vector3 &p_force);
	Vector3 get_constant_force() const { return constant_force; }
	void set_constant_torque(const Vector3 &p_torque);
	Vector3 get_constant_torque() const { return constant_torque; }

	void set_state_sync_callback(const Callable &p_callable) { state_sync_callback = p_callable; }
	void set_force_integration_callback(const Callable &p_callable, const Variant &p_udata);
	void set_omit_force_integration(bool p_omit);
	bool is_omitting_force_integration() const { return omit_force_integration; }
	void set_max_contacts_reported(int p_count);
	int get_max_contacts_reported() const { return contacts.size(); }
	void set_contacts_reported_depth_threshold(real_t p_threshold) { contacts_reported_depth_threshold = p_threshold; }
	real_t get_contacts_reported_depth_threshold() const { return contacts_reported_depth_threshold; }
	bool reports_contacts() const { return !contacts.is_empty(); }
	void clear_reported_contacts();
	void add_contact(const Contact &p_contact);
	Box3DDirectBodyState3D *get_direct_state();

	// Space step integration.
	void sync_from_move_event(const b3WorldTransform &p_transform, bool p_fell_asleep);
	void apply_kinematic_target(float p_step);
	void add_area(Box3DArea3D *p_area);
	void remove_area(Box3DArea3D *p_area);
	bool has_area_overrides() const { return areas.size() > 0; }
	void apply_environment_forces(float p_step);
	void call_state_sync();
};

// Minimal direct body state: enough for RigidBody3D::_body_state_changed
// (transform, velocities, sleep). Grows with the module.
class Box3DDirectBodyState3D : public PhysicsDirectBodyState3DDummy {
	GDCLASS(Box3DDirectBodyState3D, PhysicsDirectBodyState3DDummy);

	friend class Box3DBody3D;
	Box3DBody3D *body = nullptr;

public:
	// TODO(box3d): pass a real direct space state once queries land (milestone 2).
	Box3DDirectBodyState3D() :
			PhysicsDirectBodyState3DDummy(nullptr) {}

	virtual Transform3D get_transform() const override;
	virtual void set_transform(const Transform3D &p_transform) override;
	virtual Vector3 get_linear_velocity() const override;
	virtual void set_linear_velocity(const Vector3 &p_velocity) override;
	virtual Vector3 get_angular_velocity() const override;
	virtual void set_angular_velocity(const Vector3 &p_velocity) override;
	virtual bool is_sleeping() const override;
	virtual real_t get_inverse_mass() const override;
	virtual Vector3 get_inverse_inertia() const override;
	virtual Basis get_inverse_inertia_tensor() const override;
	virtual Basis get_principal_inertia_axes() const override;
	virtual Vector3 get_center_of_mass() const override;
	virtual Vector3 get_center_of_mass_local() const override;
	virtual Vector3 get_total_gravity() const override;
	virtual real_t get_total_linear_damp() const override;
	virtual real_t get_total_angular_damp() const override;
	virtual Vector3 get_velocity_at_local_position(const Vector3 &p_position) const override;
	virtual void apply_central_impulse(const Vector3 &p_impulse) override;
	virtual void apply_impulse(const Vector3 &p_impulse, const Vector3 &p_position = Vector3()) override;
	virtual void apply_torque_impulse(const Vector3 &p_impulse) override;
	virtual void apply_central_force(const Vector3 &p_force) override;
	virtual void apply_force(const Vector3 &p_force, const Vector3 &p_position = Vector3()) override;
	virtual void apply_torque(const Vector3 &p_torque) override;
	virtual void add_constant_central_force(const Vector3 &p_force) override;
	virtual void add_constant_force(const Vector3 &p_force, const Vector3 &p_position = Vector3()) override;
	virtual void add_constant_torque(const Vector3 &p_torque) override;
	virtual void set_constant_force(const Vector3 &p_force) override;
	virtual Vector3 get_constant_force() const override;
	virtual void set_constant_torque(const Vector3 &p_torque) override;
	virtual Vector3 get_constant_torque() const override;
	virtual void set_sleep_state(bool p_sleep) override;
	virtual void set_collision_layer(uint32_t p_layer) override;
	virtual uint32_t get_collision_layer() const override;
	virtual void set_collision_mask(uint32_t p_mask) override;
	virtual uint32_t get_collision_mask() const override;
	virtual int get_contact_count() const override;
	virtual Vector3 get_contact_local_position(int p_contact_idx) const override;
	virtual Vector3 get_contact_local_normal(int p_contact_idx) const override;
	virtual Vector3 get_contact_impulse(int p_contact_idx) const override;
	virtual int get_contact_local_shape(int p_contact_idx) const override;
	virtual Vector3 get_contact_local_velocity_at_position(int p_contact_idx) const override;
	virtual RID get_contact_collider(int p_contact_idx) const override;
	virtual Vector3 get_contact_collider_position(int p_contact_idx) const override;
	virtual ObjectID get_contact_collider_id(int p_contact_idx) const override;
	virtual int get_contact_collider_shape(int p_contact_idx) const override;
	virtual Vector3 get_contact_collider_velocity_at_position(int p_contact_idx) const override;
	virtual real_t get_step() const override;
	virtual RequiredResult<PhysicsDirectSpaceState3D> get_space_state() override;
};
