/**************************************************************************/
/*  box3d_space_3d.h — one Godot space == one b3World                     */
/**************************************************************************/

#pragma once

#include "core/templates/hash_set.h"
#include "core/templates/local_vector.h"
#include "servers/physics_3d/physics_server_3d.h"

#include "box3d/box3d.h"

class Box3DBody3D;

class Box3DSpace3D {
	b3WorldId world = {};
	real_t last_step = 0.0;

	// Default-area parameters (World3D routes these through area_set_param on the space RID).
	real_t default_gravity = 9.8;
	Vector3 default_gravity_vector = Vector3(0, -1, 0);
	real_t default_linear_damp = 0.1;
	real_t default_angular_damp = 0.1;

	LocalVector<Box3DBody3D *> dirty_bodies;
	HashSet<Box3DBody3D *> pending_kinematic;

	void _update_world_gravity();

public:
	Box3DSpace3D();
	~Box3DSpace3D();

	b3WorldId get_world() const { return world; }
	real_t get_last_step() const { return last_step; }
	Vector3 get_gravity() const { return default_gravity_vector * default_gravity; }

	void step(real_t p_step);
	void call_queries();

	void kinematic_target_queued(Box3DBody3D *p_body) { pending_kinematic.insert(p_body); }
	void body_removed(Box3DBody3D *p_body);

	void set_default_area_param(PhysicsServer3D::AreaParameter p_param, const Variant &p_value);
	Variant get_default_area_param(PhysicsServer3D::AreaParameter p_param) const;
};
