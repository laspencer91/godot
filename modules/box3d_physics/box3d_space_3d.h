/**************************************************************************/
/*  box3d_space_3d.h — one Godot space == one b3World                     */
/**************************************************************************/

#pragma once

#include "core/templates/hash_set.h"
#include "core/templates/local_vector.h"
#include "core/templates/rid_owner.h"
#include "servers/physics_3d/physics_server_3d.h"

#include "box3d/box3d.h"

class Box3DBody3D;
class Box3DArea3D;
class Box3DDirectSpaceState3D;
class Box3DShape3D;

class Box3DSpace3D {
	b3WorldId world = {};
	b3Recording *recording = nullptr;
	real_t last_step = 0.0;
	bool stepping = false;
	bool recording_active = false;

	// Default-area parameters (World3D routes these through area_set_param on the space RID).
	real_t default_gravity = 9.8;
	Vector3 default_gravity_vector = Vector3(0, -1, 0);
	real_t default_linear_damp = 0.1;
	real_t default_angular_damp = 0.1;

	LocalVector<Box3DBody3D *> dirty_bodies;
	LocalVector<Box3DArea3D *> dirty_areas;
	HashSet<Box3DBody3D *> bodies;
	HashSet<Box3DArea3D *> areas;
	HashSet<Box3DBody3D *> pending_kinematic;
	Box3DDirectSpaceState3D *direct_state = nullptr;

	void _update_world_gravity();
	void _process_sensor_event(bool p_added, b3ShapeId p_sensor_shape, b3ShapeId p_visitor_shape);
	void _harvest_body_contacts();

public:
	Box3DSpace3D();
	~Box3DSpace3D();

	b3WorldId get_world() const { return world; }
	real_t get_last_step() const { return last_step; }
	Vector3 get_gravity() const { return default_gravity_vector * default_gravity; }
	real_t get_default_linear_damp() const { return default_linear_damp; }
	real_t get_default_angular_damp() const { return default_angular_damp; }
	bool is_stepping() const { return stepping; }
	Box3DDirectSpaceState3D *get_direct_state() const { return direct_state; }
	void setup_direct_state(RID_PtrOwner<Box3DShape3D> *p_shape_owner, RID_PtrOwner<Box3DBody3D> *p_body_owner);

	void step(real_t p_step);
	void call_queries();

	bool start_recording(int p_byte_capacity);
	PackedByteArray stop_recording();
	bool is_recording() const { return recording_active; }
	PackedByteArray get_recording_data() const;
	int get_recording_size() const;
	bool save_recording(const String &p_path) const;

	void kinematic_target_queued(Box3DBody3D *p_body) { pending_kinematic.insert(p_body); }
	void body_added(Box3DBody3D *p_body) { bodies.insert(p_body); }
	void body_removed(Box3DBody3D *p_body);
	void area_added(Box3DArea3D *p_area) { areas.insert(p_area); }
	void area_removed(Box3DArea3D *p_area);

	void set_default_area_param(PhysicsServer3D::AreaParameter p_param, const Variant &p_value);
	Variant get_default_area_param(PhysicsServer3D::AreaParameter p_param) const;
};
