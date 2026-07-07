/**************************************************************************/
/*  box3d_space_3d.cpp                                                    */
/**************************************************************************/

#include "box3d_space_3d.h"

#include "box3d_body_3d.h"
#include "box3d_conversions.h"
#include "box3d_direct_space_state_3d.h"

// TODO(box3d): substep count + worker count become physics/box3d/* project settings.
static const int BOX3D_SUBSTEPS = 4;

Box3DSpace3D::Box3DSpace3D() {
	b3WorldDef def = b3DefaultWorldDef();
	def.workerCount = 1; // Single-threaded until task-system integration lands.
	world = b3CreateWorld(&def);
	_update_world_gravity();
}

Box3DSpace3D::~Box3DSpace3D() {
	// Bodies must already be detached (Godot frees bodies before their space).
	if (direct_state) {
		memdelete(direct_state);
	}
	b3DestroyWorld(world);
}

void Box3DSpace3D::_update_world_gravity() {
	b3World_SetGravity(world, to_box3d(default_gravity_vector * default_gravity));
}

void Box3DSpace3D::body_removed(Box3DBody3D *p_body) {
	bodies.erase(p_body);
	pending_kinematic.erase(p_body);
	dirty_bodies.erase(p_body);
}

void Box3DSpace3D::setup_direct_state(RID_PtrOwner<Box3DShape3D> *p_shape_owner, RID_PtrOwner<Box3DBody3D> *p_body_owner) {
	if (!direct_state) {
		direct_state = memnew(Box3DDirectSpaceState3D);
	}
	direct_state->setup(this, p_shape_owner, p_body_owner);
}

void Box3DSpace3D::step(real_t p_step) {
	for (Box3DBody3D *body : pending_kinematic) {
		body->apply_kinematic_target((float)p_step);
	}
	pending_kinematic.clear();

	for (Box3DBody3D *body : bodies) {
		body->apply_constant_forces();
	}

	stepping = true;
	b3World_Step(world, (float)p_step, BOX3D_SUBSTEPS);
	stepping = false;
	last_step = p_step;

	// Pull move events; buffer state-sync work for flush_queries().
	b3BodyEvents events = b3World_GetBodyEvents(world);
	for (int i = 0; i < events.moveCount; i++) {
		const b3BodyMoveEvent &e = events.moveEvents[i];
		Box3DBody3D *body = static_cast<Box3DBody3D *>(e.userData);
		if (body == nullptr) {
			continue;
		}
		body->sync_from_move_event(e.transform, e.fellAsleep);
		if (!body->in_dirty_list) {
			body->in_dirty_list = true;
			dirty_bodies.push_back(body);
		}
	}
}

void Box3DSpace3D::call_queries() {
	for (Box3DBody3D *body : dirty_bodies) {
		body->in_dirty_list = false;
		body->call_state_sync();
	}
	dirty_bodies.clear();
}

void Box3DSpace3D::set_default_area_param(PhysicsServer3D::AreaParameter p_param, const Variant &p_value) {
	switch (p_param) {
		case PhysicsServer3D::AREA_PARAM_GRAVITY: {
			default_gravity = p_value;
			_update_world_gravity();
		} break;
		case PhysicsServer3D::AREA_PARAM_GRAVITY_VECTOR: {
			default_gravity_vector = p_value;
			_update_world_gravity();
		} break;
		case PhysicsServer3D::AREA_PARAM_LINEAR_DAMP: {
			default_linear_damp = p_value; // Applied per body once damping lands (milestone 2).
		} break;
		case PhysicsServer3D::AREA_PARAM_ANGULAR_DAMP: {
			default_angular_damp = p_value;
		} break;
		default: {
			// Override modes / priority / wind are meaningless on the default area for now.
		} break;
	}
}

Variant Box3DSpace3D::get_default_area_param(PhysicsServer3D::AreaParameter p_param) const {
	switch (p_param) {
		case PhysicsServer3D::AREA_PARAM_GRAVITY:
			return default_gravity;
		case PhysicsServer3D::AREA_PARAM_GRAVITY_VECTOR:
			return default_gravity_vector;
		case PhysicsServer3D::AREA_PARAM_LINEAR_DAMP:
			return default_linear_damp;
		case PhysicsServer3D::AREA_PARAM_ANGULAR_DAMP:
			return default_angular_damp;
		default:
			return Variant();
	}
}
