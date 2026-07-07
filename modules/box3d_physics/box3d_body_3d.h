/**************************************************************************/
/*  box3d_body_3d.h — rigid/kinematic/static body wrapping a b3BodyId     */
/**************************************************************************/

#pragma once

#include "core/templates/local_vector.h"
#include "servers/physics_3d/physics_server_3d_dummy.h"

#include "box3d/box3d.h"

class Box3DShape3D;
class Box3DSpace3D;
class Box3DDirectBodyState3D;

class Box3DBody3D {
	friend class Box3DDirectBodyState3D;
	friend class Box3DSpace3D;

public:
	struct ShapeSlot {
		Box3DShape3D *shape = nullptr;
		Transform3D xform;
		bool disabled = false;
	};

private:
	RID rid;
	ObjectID instance_id;

	Box3DSpace3D *space = nullptr;
	b3BodyId body_id = {}; // Null id when not in a space.
	LocalVector<b3ShapeId> shape_ids;
	LocalVector<ShapeSlot> slots;

	PhysicsServer3D::BodyMode mode = PhysicsServer3D::BODY_MODE_RIGID;
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

	Callable state_sync_callback;
	Box3DDirectBodyState3D *direct_state = nullptr;

	bool in_dirty_list = false;
	bool has_kinematic_target = false;
	Transform3D kinematic_target;

	void _build_all_shapes();
	void _destroy_all_shapes();
	void _update_mass();
	b3BodyType _box3d_type() const;
	b3MotionLocks _motion_locks() const;

public:
	~Box3DBody3D();

	void set_rid(const RID &p_rid) { rid = p_rid; }
	RID get_rid() const { return rid; }
	void set_instance_id(ObjectID p_id) { instance_id = p_id; }
	ObjectID get_instance_id() const { return instance_id; }

	bool in_space() const;
	void set_space(Box3DSpace3D *p_space);
	Box3DSpace3D *get_space() const { return space; }

	void set_mode(PhysicsServer3D::BodyMode p_mode);
	PhysicsServer3D::BodyMode get_mode() const { return mode; }

	// Shape list management (server-facing).
	void add_shape(Box3DShape3D *p_shape, const Transform3D &p_xform, bool p_disabled);
	void remove_shape_at(int p_index);
	void remove_shape(Box3DShape3D *p_shape); // Removes every slot using p_shape.
	void clear_shapes();
	void set_shape(int p_index, Box3DShape3D *p_shape);
	void set_shape_transform(int p_index, const Transform3D &p_xform);
	void set_shape_disabled(int p_index, bool p_disabled);
	int get_shape_count() const { return slots.size(); }
	const ShapeSlot *get_shape_slot(int p_index) const;
	void shapes_changed(); // Rebuild all b3 shapes (shape data edited, filters changed, ...).

	void set_transform(const Transform3D &p_transform); // Godot BODY_STATE_TRANSFORM semantics per mode.
	Transform3D get_transform() const { return transform; }

	void set_linear_velocity(const Vector3 &p_velocity);
	Vector3 get_linear_velocity() const;
	void set_angular_velocity(const Vector3 &p_velocity);
	Vector3 get_angular_velocity() const;

	void set_sleep_state(bool p_sleeping);
	bool is_sleeping() const { return sleeping; }
	void set_can_sleep(bool p_can_sleep);
	bool get_can_sleep() const { return can_sleep; }

	void set_param(PhysicsServer3D::BodyParameter p_param, const Variant &p_value);
	Variant get_param(PhysicsServer3D::BodyParameter p_param) const;

	void set_collision_layer(uint32_t p_layer);
	uint32_t get_collision_layer() const { return collision_layer; }
	void set_collision_mask(uint32_t p_mask);
	uint32_t get_collision_mask() const { return collision_mask; }

	void set_state_sync_callback(const Callable &p_callable) { state_sync_callback = p_callable; }
	Box3DDirectBodyState3D *get_direct_state();

	// Space step integration.
	void sync_from_move_event(const b3WorldTransform &p_transform, bool p_fell_asleep);
	void apply_kinematic_target(float p_step);
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
	virtual Vector3 get_total_gravity() const override;
	virtual Vector3 get_velocity_at_local_position(const Vector3 &p_position) const override;
	virtual real_t get_step() const override;
};
