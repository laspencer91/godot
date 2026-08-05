/**************************************************************************/
/*  box3d_area_3d.h                                                        */
/**************************************************************************/

#pragma once

#include "box3d_collision_object_3d.h"

#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "servers/physics_3d/physics_server_3d.h"

#include "box3d/box3d.h"

class Box3DShape3D;

class Box3DArea3D : public Box3DCollisionObject3D {
public:
	struct ShapeSlot {
		RID rid;
		Box3DShape3D *shape = nullptr;
		Transform3D xform;
		bool disabled = false;
	};

	struct MonitorKey {
		RID rid;
		ObjectID instance_id;
		uint32_t collider_shape = 0;
		uint32_t self_shape = 0;

		static uint32_t hash(const MonitorKey &p_key);
		bool operator==(const MonitorKey &p_key) const {
			return rid == p_key.rid && instance_id == p_key.instance_id && collider_shape == p_key.collider_shape && self_shape == p_key.self_shape;
		}
	};

	struct MonitorState {
		int state = 0;
	};

private:
	RID rid;
	RID space_rid;
	ObjectID instance_id;
	Box3DSpace3D *space = nullptr;
	b3BodyId body_id = {};
	LocalVector<b3ShapeId> shape_ids;
	LocalVector<b3MeshData *> instance_meshes;
	LocalVector<ShapeSlot> slots;

	Transform3D transform;
	Vector3 scale = Vector3(1.0, 1.0, 1.0);
	uint32_t collision_layer = 1;
	uint32_t collision_mask = 1;
	bool monitorable = false;

	PS3DE::AreaSpaceOverrideMode gravity_override_mode = PS3DE::AREA_SPACE_OVERRIDE_DISABLED;
	PS3DE::AreaSpaceOverrideMode linear_damp_override_mode = PS3DE::AREA_SPACE_OVERRIDE_DISABLED;
	PS3DE::AreaSpaceOverrideMode angular_damp_override_mode = PS3DE::AREA_SPACE_OVERRIDE_DISABLED;
	real_t gravity = 9.80665;
	Vector3 gravity_vector = Vector3(0, -1, 0);
	bool gravity_is_point = false;
	real_t gravity_point_unit_distance = 0.0;
	real_t linear_damp = 0.1;
	real_t angular_damp = 0.1;
	real_t wind_force_magnitude = 0.0;
	Vector3 wind_source;
	Vector3 wind_direction;
	real_t wind_attenuation_factor = 0.0;
	int priority = 0;

	Callable monitor_callback;
	Callable area_monitor_callback;
	HashMap<MonitorKey, MonitorState, MonitorKey> monitored_bodies;
	HashMap<MonitorKey, MonitorState, MonitorKey> monitored_areas;

	void _build_all_shapes();
	void _destroy_all_shapes();

public:
	Box3DArea3D();
	~Box3DArea3D();

	void set_rid(const RID &p_rid) { rid = p_rid; }
	virtual RID get_rid() const override { return rid; }
	void set_space_rid(const RID &p_rid) { space_rid = p_rid; }
	RID get_space_rid() const { return space_rid; }
	void set_instance_id(ObjectID p_id) { instance_id = p_id; }
	virtual ObjectID get_instance_id() const override { return instance_id; }
	b3BodyId get_body_id() const { return body_id; }

	bool in_space() const;
	void set_space(Box3DSpace3D *p_space);
	virtual Box3DSpace3D *get_space() const override { return space; }

	void add_shape(RID p_shape_rid, Box3DShape3D *p_shape, const Transform3D &p_xform, bool p_disabled);
	void set_shape(int p_index, RID p_shape_rid, Box3DShape3D *p_shape);
	void set_shape_transform(int p_index, const Transform3D &p_xform);
	void set_shape_disabled(int p_index, bool p_disabled);
	void remove_shape_at(int p_index);
	void remove_shape(Box3DShape3D *p_shape);
	void clear_shapes();
	int get_shape_count() const { return slots.size(); }
	const ShapeSlot *get_shape_slot(int p_index) const;
	virtual void shapes_changed() override;

	void set_transform(const Transform3D &p_transform);
	Transform3D get_transform() const { return transform; }
	void set_collision_layer(uint32_t p_layer);
	uint32_t get_collision_layer() const { return collision_layer; }
	void set_collision_mask(uint32_t p_mask);
	uint32_t get_collision_mask() const { return collision_mask; }
	void set_monitorable(bool p_monitorable);
	bool is_monitorable() const { return monitorable; }
	void set_monitor_callback(const Callable &p_callback) { monitor_callback = p_callback; }
	void set_area_monitor_callback(const Callable &p_callback) { area_monitor_callback = p_callback; }

	void set_param(PS3DE::AreaParameter p_param, const Variant &p_value);
	Variant get_param(PS3DE::AreaParameter p_param) const;
	int get_priority() const { return priority; }
	PS3DE::AreaSpaceOverrideMode get_gravity_override_mode() const { return gravity_override_mode; }
	PS3DE::AreaSpaceOverrideMode get_linear_damp_override_mode() const { return linear_damp_override_mode; }
	PS3DE::AreaSpaceOverrideMode get_angular_damp_override_mode() const { return angular_damp_override_mode; }
	real_t get_linear_damp() const { return linear_damp; }
	real_t get_angular_damp() const { return angular_damp; }
	void compute_gravity(const Vector3 &p_position, Vector3 &r_gravity) const;

	void queue_body_event(bool p_added, RID p_body, ObjectID p_instance_id, int p_body_shape, int p_area_shape);
	void queue_area_event(bool p_added, RID p_area, ObjectID p_instance_id, int p_area_shape, int p_self_shape);
	void call_queries();
};
