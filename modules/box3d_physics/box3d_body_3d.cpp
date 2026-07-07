/**************************************************************************/
/*  box3d_body_3d.cpp                                                     */
/**************************************************************************/

#include "box3d_body_3d.h"

#include "box3d_conversions.h"
#include "box3d_shape_3d.h"
#include "box3d_space_3d.h"

#include "box3d/collision.h"

Box3DBody3D::~Box3DBody3D() {
	set_space(nullptr);
	if (direct_state) {
		memdelete(direct_state);
	}
}

bool Box3DBody3D::in_space() const {
	return space != nullptr && B3_IS_NON_NULL(body_id);
}

b3BodyType Box3DBody3D::_box3d_type() const {
	switch (mode) {
		case PhysicsServer3D::BODY_MODE_STATIC:
			return b3_staticBody;
		case PhysicsServer3D::BODY_MODE_KINEMATIC:
			return b3_kinematicBody;
		default:
			return b3_dynamicBody;
	}
}

b3MotionLocks Box3DBody3D::_motion_locks() const {
	b3MotionLocks locks = {};
	if (mode == PhysicsServer3D::BODY_MODE_RIGID_LINEAR) {
		locks.angularX = true;
		locks.angularY = true;
		locks.angularZ = true;
	}
	return locks;
}

void Box3DBody3D::set_space(Box3DSpace3D *p_space) {
	if (space == p_space) {
		return;
	}

	if (space) {
		if (in_space()) {
			// Preserve dynamic state across space changes.
			linear_velocity_cache = get_linear_velocity();
			angular_velocity_cache = get_angular_velocity();
			_destroy_all_shapes();
			b3DestroyBody(body_id);
			body_id = b3_nullBodyId;
		}
		space->body_removed(this);
	}

	space = p_space;

	if (space) {
		b3BodyDef def = b3DefaultBodyDef();
		def.type = _box3d_type();
		def.position = to_box3d(transform.origin);
		def.rotation = to_box3d(transform.basis.get_rotation_quaternion());
		def.linearVelocity = to_box3d(linear_velocity_cache);
		def.angularVelocity = to_box3d(angular_velocity_cache);
		def.gravityScale = (float)gravity_scale;
		def.enableSleep = can_sleep;
		def.isAwake = !sleeping;
		def.motionLocks = _motion_locks();
		def.userData = this;
		body_id = b3CreateBody(space->get_world(), &def);
		_build_all_shapes();
	}
}

void Box3DBody3D::set_mode(PhysicsServer3D::BodyMode p_mode) {
	if (mode == p_mode) {
		return;
	}
	mode = p_mode;
	if (in_space()) {
		b3Body_SetType(body_id, _box3d_type());
		b3Body_SetMotionLocks(body_id, _motion_locks());
		_update_mass();
	}
}

void Box3DBody3D::_destroy_all_shapes() {
	for (const b3ShapeId &id : shape_ids) {
		if (B3_IS_NON_NULL(id)) {
			b3DestroyShape(id, false);
		}
	}
	shape_ids.clear();
}

void Box3DBody3D::_build_all_shapes() {
	if (!in_space()) {
		return;
	}
	_destroy_all_shapes();

	b3ShapeDef def = b3DefaultShapeDef();
	def.baseMaterial.friction = (float)friction;
	def.baseMaterial.restitution = (float)bounce;
	def.filter.categoryBits = (uint64_t)collision_layer;
	def.filter.maskBits = (uint64_t)collision_mask;

	for (uint32_t i = 0; i < slots.size(); i++) {
		const ShapeSlot &slot = slots[i];
		if (slot.disabled || slot.shape == nullptr) {
			continue;
		}
		def.userData = (void *)(uintptr_t)i; // Godot shape index for query results.

		Box3DShape3D *s = slot.shape;
		b3ShapeId shape_id = b3_nullShapeId;
		const b3Vec3 unit_scale = b3Vec3{ 1.0f, 1.0f, 1.0f };

		switch (s->type) {
			case PhysicsServer3D::SHAPE_SPHERE: {
				b3Sphere sphere;
				sphere.center = to_box3d(slot.xform.origin);
				sphere.radius = s->sphere_radius;
				shape_id = b3CreateSphereShape(body_id, &def, &sphere);
			} break;

			case PhysicsServer3D::SHAPE_CAPSULE: {
				const float half_cylinder = MAX(0.0f, 0.5f * s->capsule_height - s->capsule_radius);
				b3Capsule capsule;
				capsule.center1 = to_box3d(slot.xform.xform(Vector3(0, half_cylinder, 0)));
				capsule.center2 = to_box3d(slot.xform.xform(Vector3(0, -half_cylinder, 0)));
				capsule.radius = s->capsule_radius;
				shape_id = b3CreateCapsuleShape(body_id, &def, &capsule);
			} break;

			case PhysicsServer3D::SHAPE_BOX: {
				if (s->box_built) {
					shape_id = b3CreateTransformedHullShape(body_id, &def, &s->box_hull.base, to_box3d(slot.xform), unit_scale);
				}
			} break;

			case PhysicsServer3D::SHAPE_CYLINDER:
			case PhysicsServer3D::SHAPE_CONVEX_POLYGON: {
				if (s->hull) {
					shape_id = b3CreateTransformedHullShape(body_id, &def, s->hull, to_box3d(slot.xform), unit_scale);
				}
			} break;

			case PhysicsServer3D::SHAPE_CONCAVE_POLYGON: {
				if (s->mesh) {
					ERR_FAIL_COND_MSG(mode != PhysicsServer3D::BODY_MODE_STATIC && mode != PhysicsServer3D::BODY_MODE_KINEMATIC,
							"Box3D: concave (trimesh) shapes are only supported on static/kinematic bodies.");
					if (!slot.xform.is_equal_approx(Transform3D())) {
						WARN_PRINT_ONCE("Box3D: per-instance transforms on trimesh shapes are not baked yet; shape placed at body origin.");
					}
					shape_id = b3CreateMeshShape(body_id, &def, s->mesh, unit_scale);
				}
			} break;

			default: {
				// Unsupported types warned at set_data time; skip silently here.
			} break;
		}

		if (B3_IS_NON_NULL(shape_id)) {
			shape_ids.push_back(shape_id);
		}
	}

	_update_mass();
}

void Box3DBody3D::_update_mass() {
	if (!in_space() || _box3d_type() != b3_dynamicBody) {
		return;
	}
	b3MassData mass_data = b3Body_GetMassData(body_id);
	if (mass_data.mass <= 0.0f) {
		return; // Shapeless dynamic body; Box3D handles the fallback.
	}
	// Godot bodies have an explicit mass; rescale the shape-derived mass/inertia to match.
	const float factor = (float)mass / mass_data.mass;
	mass_data.mass = (float)mass;
	mass_data.inertia.cx = b3MulSV(factor, mass_data.inertia.cx);
	mass_data.inertia.cy = b3MulSV(factor, mass_data.inertia.cy);
	mass_data.inertia.cz = b3MulSV(factor, mass_data.inertia.cz);
	b3Body_SetMassData(body_id, mass_data);
}

void Box3DBody3D::add_shape(Box3DShape3D *p_shape, const Transform3D &p_xform, bool p_disabled) {
	ShapeSlot slot;
	slot.shape = p_shape;
	slot.xform = p_xform;
	slot.disabled = p_disabled;
	slots.push_back(slot);
	p_shape->add_owner(this);
	shapes_changed();
}

void Box3DBody3D::remove_shape_at(int p_index) {
	ERR_FAIL_INDEX(p_index, (int)slots.size());
	Box3DShape3D *removed = slots[p_index].shape;
	slots.remove_at(p_index);
	bool still_used = false;
	for (const ShapeSlot &slot : slots) {
		if (slot.shape == removed) {
			still_used = true;
			break;
		}
	}
	if (!still_used && removed) {
		removed->remove_owner(this);
	}
	shapes_changed();
}

void Box3DBody3D::remove_shape(Box3DShape3D *p_shape) {
	for (int i = (int)slots.size() - 1; i >= 0; i--) {
		if (slots[i].shape == p_shape) {
			slots.remove_at(i);
		}
	}
	p_shape->remove_owner(this);
	shapes_changed();
}

void Box3DBody3D::clear_shapes() {
	for (const ShapeSlot &slot : slots) {
		if (slot.shape) {
			slot.shape->remove_owner(this);
		}
	}
	slots.clear();
	shapes_changed();
}

void Box3DBody3D::set_shape(int p_index, Box3DShape3D *p_shape) {
	ERR_FAIL_INDEX(p_index, (int)slots.size());
	if (slots[p_index].shape) {
		slots[p_index].shape->remove_owner(this);
	}
	slots[p_index].shape = p_shape;
	p_shape->add_owner(this);
	shapes_changed();
}

void Box3DBody3D::set_shape_transform(int p_index, const Transform3D &p_xform) {
	ERR_FAIL_INDEX(p_index, (int)slots.size());
	slots[p_index].xform = p_xform;
	shapes_changed();
}

void Box3DBody3D::set_shape_disabled(int p_index, bool p_disabled) {
	ERR_FAIL_INDEX(p_index, (int)slots.size());
	slots[p_index].disabled = p_disabled;
	shapes_changed();
}

const Box3DBody3D::ShapeSlot *Box3DBody3D::get_shape_slot(int p_index) const {
	ERR_FAIL_INDEX_V(p_index, (int)slots.size(), nullptr);
	return &slots[p_index];
}

void Box3DBody3D::shapes_changed() {
	if (in_space()) {
		_build_all_shapes();
	}
}

void Box3DBody3D::set_transform(const Transform3D &p_transform) {
	transform = p_transform;
	if (!in_space()) {
		return;
	}
	if (mode == PhysicsServer3D::BODY_MODE_KINEMATIC) {
		// Deferred: applied as a velocity-consistent target next step (moving platforms).
		kinematic_target = p_transform;
		has_kinematic_target = true;
		space->kinematic_target_queued(this);
	} else {
		b3Body_SetTransform(body_id, to_box3d(p_transform.origin), to_box3d(p_transform.basis.get_rotation_quaternion()));
	}
}

void Box3DBody3D::apply_kinematic_target(float p_step) {
	if (!has_kinematic_target || !in_space()) {
		return;
	}
	b3Transform target = to_box3d(kinematic_target);
	b3Body_SetTargetTransform(body_id, target, p_step, true);
	has_kinematic_target = false;
}

void Box3DBody3D::set_linear_velocity(const Vector3 &p_velocity) {
	linear_velocity_cache = p_velocity;
	if (in_space()) {
		b3Body_SetLinearVelocity(body_id, to_box3d(p_velocity));
	}
}

Vector3 Box3DBody3D::get_linear_velocity() const {
	if (in_space()) {
		return to_godot(b3Body_GetLinearVelocity(body_id));
	}
	return linear_velocity_cache;
}

void Box3DBody3D::set_angular_velocity(const Vector3 &p_velocity) {
	angular_velocity_cache = p_velocity;
	if (in_space()) {
		b3Body_SetAngularVelocity(body_id, to_box3d(p_velocity));
	}
}

Vector3 Box3DBody3D::get_angular_velocity() const {
	if (in_space()) {
		return to_godot(b3Body_GetAngularVelocity(body_id));
	}
	return angular_velocity_cache;
}

void Box3DBody3D::set_sleep_state(bool p_sleeping) {
	sleeping = p_sleeping;
	if (in_space()) {
		b3Body_SetAwake(body_id, !p_sleeping);
	}
}

void Box3DBody3D::set_can_sleep(bool p_can_sleep) {
	can_sleep = p_can_sleep;
	if (in_space()) {
		b3Body_EnableSleep(body_id, p_can_sleep);
	}
}

void Box3DBody3D::set_param(PhysicsServer3D::BodyParameter p_param, const Variant &p_value) {
	switch (p_param) {
		case PhysicsServer3D::BODY_PARAM_FRICTION: {
			friction = p_value;
			for (const b3ShapeId &id : shape_ids) {
				b3Shape_SetFriction(id, (float)friction);
			}
		} break;
		case PhysicsServer3D::BODY_PARAM_BOUNCE: {
			bounce = p_value;
			for (const b3ShapeId &id : shape_ids) {
				b3Shape_SetRestitution(id, (float)bounce);
			}
		} break;
		case PhysicsServer3D::BODY_PARAM_MASS: {
			mass = p_value;
			_update_mass();
		} break;
		case PhysicsServer3D::BODY_PARAM_GRAVITY_SCALE: {
			gravity_scale = p_value;
			if (in_space()) {
				b3Body_SetGravityScale(body_id, (float)gravity_scale);
			}
		} break;
		default: {
			// Remaining parameters land with areas/damping work (milestone 2).
		} break;
	}
}

Variant Box3DBody3D::get_param(PhysicsServer3D::BodyParameter p_param) const {
	switch (p_param) {
		case PhysicsServer3D::BODY_PARAM_FRICTION:
			return friction;
		case PhysicsServer3D::BODY_PARAM_BOUNCE:
			return bounce;
		case PhysicsServer3D::BODY_PARAM_MASS:
			return mass;
		case PhysicsServer3D::BODY_PARAM_GRAVITY_SCALE:
			return gravity_scale;
		default:
			return Variant();
	}
}

void Box3DBody3D::set_collision_layer(uint32_t p_layer) {
	collision_layer = p_layer;
	shapes_changed();
}

void Box3DBody3D::set_collision_mask(uint32_t p_mask) {
	collision_mask = p_mask;
	shapes_changed();
}

Box3DDirectBodyState3D *Box3DBody3D::get_direct_state() {
	if (!direct_state) {
		direct_state = memnew(Box3DDirectBodyState3D);
		direct_state->body = this;
	}
	return direct_state;
}

void Box3DBody3D::sync_from_move_event(const b3WorldTransform &p_transform, bool p_fell_asleep) {
	transform = to_godot(p_transform);
	if (p_fell_asleep) {
		sleeping = true;
	} else if (sleeping) {
		sleeping = false;
	}
}

void Box3DBody3D::call_state_sync() {
	if (state_sync_callback.is_valid()) {
		state_sync_callback.call(get_direct_state());
	}
}

// --- Box3DDirectBodyState3D ---

Transform3D Box3DDirectBodyState3D::get_transform() const {
	return body->get_transform();
}

void Box3DDirectBodyState3D::set_transform(const Transform3D &p_transform) {
	body->set_transform(p_transform);
}

Vector3 Box3DDirectBodyState3D::get_linear_velocity() const {
	return body->get_linear_velocity();
}

void Box3DDirectBodyState3D::set_linear_velocity(const Vector3 &p_velocity) {
	body->set_linear_velocity(p_velocity);
}

Vector3 Box3DDirectBodyState3D::get_angular_velocity() const {
	return body->get_angular_velocity();
}

void Box3DDirectBodyState3D::set_angular_velocity(const Vector3 &p_velocity) {
	body->set_angular_velocity(p_velocity);
}

bool Box3DDirectBodyState3D::is_sleeping() const {
	return body->is_sleeping();
}

real_t Box3DDirectBodyState3D::get_inverse_mass() const {
	if (!body->in_space()) {
		return 0.0;
	}
	const float m = b3Body_GetMass(body->body_id);
	return m > 0.0f ? 1.0 / m : 0.0;
}

Vector3 Box3DDirectBodyState3D::get_total_gravity() const {
	if (!body->get_space()) {
		return Vector3();
	}
	return body->get_space()->get_gravity() * body->gravity_scale;
}

Vector3 Box3DDirectBodyState3D::get_velocity_at_local_position(const Vector3 &p_position) const {
	if (!body->in_space()) {
		return Vector3();
	}
	const Vector3 world_point = body->get_transform().xform(p_position);
	return to_godot(b3Body_GetWorldPointVelocity(body->body_id, to_box3d(world_point)));
}

real_t Box3DDirectBodyState3D::get_step() const {
	return body->get_space() ? body->get_space()->get_last_step() : 0.0;
}
