/**************************************************************************/
/*  box3d_body_3d.cpp                                                     */
/**************************************************************************/

#include "box3d_body_3d.h"

#include "box3d_conversions.h"
#include "box3d_direct_space_state_3d.h"
#include "box3d_shape_3d.h"
#include "box3d_space_3d.h"
#include "box3d_surface_materials.h"

#include "box3d/collision.h"
#include "box3d/math_functions.h"

static constexpr uint64_t BOX3D_QUERY_FILTER_BIT = UINT64_C(1) << 63;

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
		space->body_added(this);
		b3BodyDef def = b3DefaultBodyDef();
		def.type = _box3d_type();
		def.position = to_box3d(transform.origin);
		def.rotation = to_box3d(transform.basis.get_rotation_quaternion());
		def.linearVelocity = to_box3d(linear_velocity_cache);
		def.angularVelocity = to_box3d(angular_velocity_cache);
		def.gravityScale = (float)gravity_scale;
		def.enableSleep = can_sleep;
		def.isAwake = !sleeping;
		def.isBullet = continuous_cd;
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
	for (b3MeshData *mesh : instance_meshes) {
		if (mesh) {
			b3DestroyMesh(mesh);
		}
	}
	instance_meshes.clear();
}

static b3MeshData *_clone_mesh_with_transform(const b3MeshData *p_mesh, const Transform3D &p_transform) {
	const b3Vec3 *src_vertices = b3GetMeshVertices(p_mesh);
	const b3MeshTriangle *src_triangles = b3GetMeshTriangles(p_mesh);
	const uint8_t *src_material_indices = b3GetMeshMaterialIndices(p_mesh);
	ERR_FAIL_NULL_V(src_vertices, nullptr);
	ERR_FAIL_NULL_V(src_triangles, nullptr);

	LocalVector<b3Vec3> vertices;
	LocalVector<int32_t> indices;
	LocalVector<uint8_t> material_indices;
	vertices.resize(p_mesh->vertexCount);
	indices.resize(p_mesh->triangleCount * 3);
	if (src_material_indices != nullptr) {
		material_indices.resize(p_mesh->triangleCount);
	}

	for (int i = 0; i < p_mesh->vertexCount; i++) {
		vertices[i] = to_box3d(p_transform.xform(to_godot(src_vertices[i])));
	}
	for (int i = 0; i < p_mesh->triangleCount; i++) {
		indices[i * 3 + 0] = src_triangles[i].index1;
		indices[i * 3 + 1] = src_triangles[i].index2;
		indices[i * 3 + 2] = src_triangles[i].index3;
		if (src_material_indices != nullptr) {
			material_indices[i] = src_material_indices[i];
		}
	}

	b3MeshDef mesh_def = {};
	mesh_def.vertices = vertices.ptr();
	mesh_def.indices = indices.ptr();
	mesh_def.vertexCount = vertices.size();
	mesh_def.triangleCount = p_mesh->triangleCount;
	mesh_def.materialIndices = material_indices.size() == p_mesh->triangleCount ? material_indices.ptr() : nullptr;
	mesh_def.weldVertices = false;
	mesh_def.identifyEdges = true;

	return b3CreateMesh(&mesh_def, nullptr, 0);
}

bool Box3DBody3D::_slot_has_named_surface_material(uint32_t p_slot) const {
	if (p_slot >= slots.size()) {
		return false;
	}
	const ShapeSlot &slot = slots[p_slot];
	return slot.has_surface_material || (slot.shape != nullptr && slot.shape->has_named_surface_material());
}

void Box3DBody3D::_build_all_shapes() {
	if (!in_space()) {
		return;
	}
	_destroy_all_shapes();

	for (uint32_t i = 0; i < slots.size(); i++) {
		shape_ids.push_back(b3_nullShapeId);
		instance_meshes.push_back(nullptr);

		const ShapeSlot &slot = slots[i];
		if (slot.disabled || slot.shape == nullptr) {
			continue;
		}

		Box3DShape3D *s = slot.shape;
		b3ShapeDef def = b3DefaultShapeDef();
		def.baseMaterial.friction = (float)friction;
		def.baseMaterial.restitution = (float)bounce;
		def.filter.categoryBits = (uint64_t)collision_layer;
		def.filter.maskBits = (uint64_t)collision_mask | BOX3D_QUERY_FILTER_BIT;
		def.userData = (void *)(uintptr_t)i; // Godot shape index for query results.

		Box3DPhysics *box3d_physics = Box3DPhysics::get_singleton();
		LocalVector<b3SurfaceMaterial> mesh_materials;
		if (box3d_physics != nullptr) {
			if (slot.has_surface_material) {
				def.baseMaterial = box3d_physics->get_box3d_material(slot.surface_material_id);
			} else if (s->has_named_surface_material()) {
				def.baseMaterial = box3d_physics->get_box3d_material(s->get_surface_material_id());
			}

			const PackedInt64Array &mesh_material_ids = s->get_mesh_material_ids();
			if (mesh_material_ids.size() > 0) {
				mesh_materials.resize(mesh_material_ids.size());
				for (int j = 0; j < mesh_material_ids.size(); j++) {
					mesh_materials[j] = box3d_physics->get_box3d_material((int)mesh_material_ids[j]);
				}
				def.materials = mesh_materials.ptr();
				def.materialCount = mesh_materials.size();
			}
		}

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
					if (mode != PhysicsServer3D::BODY_MODE_STATIC && mode != PhysicsServer3D::BODY_MODE_KINEMATIC) {
						WARN_PRINT("Box3D: concave (trimesh) shapes are only supported on static/kinematic bodies.");
						continue;
					}
					b3MeshData *mesh = s->mesh;
					if (!slot.xform.is_equal_approx(Transform3D())) {
						mesh = _clone_mesh_with_transform(s->mesh, slot.xform);
						if (mesh == nullptr) {
							WARN_PRINT("Box3D: failed to bake trimesh instance transform.");
							continue;
						}
						instance_meshes[i] = mesh;
					}
					shape_id = b3CreateMeshShape(body_id, &def, mesh, unit_scale);
				}
			} break;

			case PhysicsServer3D::SHAPE_HEIGHTMAP: {
				if (s->mesh) {
					if (mode != PhysicsServer3D::BODY_MODE_STATIC && mode != PhysicsServer3D::BODY_MODE_KINEMATIC) {
						WARN_PRINT("Box3D: heightmap shapes are only supported on static/kinematic bodies.");
						continue;
					}
					b3MeshData *mesh = s->mesh;
					if (!slot.xform.is_equal_approx(Transform3D())) {
						mesh = _clone_mesh_with_transform(s->mesh, slot.xform);
						if (mesh == nullptr) {
							WARN_PRINT("Box3D: failed to bake heightmap instance transform.");
							continue;
						}
						instance_meshes[i] = mesh;
					}
					shape_id = b3CreateMeshShape(body_id, &def, mesh, unit_scale);
				}
			} break;

			default: {
				// Unsupported types warned at set_data time; skip silently here.
			} break;
		}

		if (B3_IS_NON_NULL(shape_id)) {
			shape_ids[i] = shape_id;
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

void Box3DBody3D::add_shape(RID p_shape_rid, Box3DShape3D *p_shape, const Transform3D &p_xform, bool p_disabled) {
	ShapeSlot slot;
	slot.rid = p_shape_rid;
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

void Box3DBody3D::set_shape(int p_index, RID p_shape_rid, Box3DShape3D *p_shape) {
	ERR_FAIL_INDEX(p_index, (int)slots.size());
	if (slots[p_index].shape) {
		slots[p_index].shape->remove_owner(this);
	}
	slots[p_index].rid = p_shape_rid;
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

void Box3DBody3D::set_surface_material(int p_shape_idx, int p_material_id) {
	if (p_shape_idx == -1) {
		for (ShapeSlot &slot : slots) {
			slot.has_surface_material = p_material_id > 0;
			slot.surface_material_id = MAX(0, p_material_id);
		}
		shapes_changed();
		return;
	}
	ERR_FAIL_INDEX(p_shape_idx, (int)slots.size());
	slots[p_shape_idx].has_surface_material = p_material_id > 0;
	slots[p_shape_idx].surface_material_id = MAX(0, p_material_id);
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

void Box3DBody3D::apply_constant_forces() {
	if (!in_space() || mode == PhysicsServer3D::BODY_MODE_STATIC || mode == PhysicsServer3D::BODY_MODE_KINEMATIC) {
		return;
	}
	if (!constant_force.is_zero_approx()) {
		b3Body_ApplyForceToCenter(body_id, to_box3d(constant_force), true);
	}
	if (!constant_torque.is_zero_approx()) {
		b3Body_ApplyTorque(body_id, to_box3d(constant_torque), true);
	}
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
			for (uint32_t i = 0; i < shape_ids.size(); i++) {
				if (_slot_has_named_surface_material(i)) {
					WARN_PRINT_ONCE("Box3D: BODY_PARAM_FRICTION ignored on shapes with named Box3D surface materials.");
					continue;
				}
				b3Shape_SetFriction(shape_ids[i], (float)friction);
			}
		} break;
		case PhysicsServer3D::BODY_PARAM_BOUNCE: {
			bounce = p_value;
			for (uint32_t i = 0; i < shape_ids.size(); i++) {
				if (_slot_has_named_surface_material(i)) {
					WARN_PRINT_ONCE("Box3D: BODY_PARAM_BOUNCE ignored on shapes with named Box3D surface materials.");
					continue;
				}
				b3Shape_SetRestitution(shape_ids[i], (float)bounce);
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

void Box3DBody3D::set_enable_continuous_collision_detection(bool p_enable) {
	continuous_cd = p_enable;
	if (in_space()) {
		b3Body_SetBullet(body_id, p_enable);
	}
}

void Box3DBody3D::apply_central_impulse(const Vector3 &p_impulse) {
	if (in_space()) {
		b3Body_ApplyLinearImpulseToCenter(body_id, to_box3d(p_impulse), true);
	} else if (mass > 0.0) {
		linear_velocity_cache += p_impulse / mass;
	}
}

void Box3DBody3D::apply_impulse(const Vector3 &p_impulse, const Vector3 &p_position) {
	if (in_space()) {
		const b3Pos world_point = b3OffsetPos(b3Body_GetPosition(body_id), to_box3d(p_position));
		b3Body_ApplyLinearImpulse(body_id, to_box3d(p_impulse), world_point, true);
	} else if (mass > 0.0) {
		linear_velocity_cache += p_impulse / mass;
	}
}

void Box3DBody3D::apply_torque_impulse(const Vector3 &p_impulse) {
	if (in_space()) {
		b3Body_ApplyAngularImpulse(body_id, to_box3d(p_impulse), true);
	}
}

void Box3DBody3D::apply_central_force(const Vector3 &p_force) {
	if (in_space()) {
		b3Body_ApplyForceToCenter(body_id, to_box3d(p_force), true);
	}
}

void Box3DBody3D::apply_force(const Vector3 &p_force, const Vector3 &p_position) {
	if (in_space()) {
		const b3Pos world_point = b3OffsetPos(b3Body_GetPosition(body_id), to_box3d(p_position));
		b3Body_ApplyForce(body_id, to_box3d(p_force), world_point, true);
	}
}

void Box3DBody3D::apply_torque(const Vector3 &p_torque) {
	if (in_space()) {
		b3Body_ApplyTorque(body_id, to_box3d(p_torque), true);
	}
}

void Box3DBody3D::add_constant_central_force(const Vector3 &p_force) {
	constant_force += p_force;
}

void Box3DBody3D::add_constant_force(const Vector3 &p_force, const Vector3 &p_position) {
	constant_force += p_force;
	Vector3 com_relative = p_position;
	if (in_space()) {
		com_relative -= to_godot(b3Body_GetWorldVector(body_id, b3Body_GetLocalCenter(body_id)));
	}
	constant_torque += com_relative.cross(p_force);
}

void Box3DBody3D::add_constant_torque(const Vector3 &p_torque) {
	constant_torque += p_torque;
}

void Box3DBody3D::set_constant_force(const Vector3 &p_force) {
	constant_force = p_force;
}

void Box3DBody3D::set_constant_torque(const Vector3 &p_torque) {
	constant_torque = p_torque;
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

void Box3DDirectBodyState3D::apply_central_impulse(const Vector3 &p_impulse) {
	body->apply_central_impulse(p_impulse);
}

void Box3DDirectBodyState3D::apply_impulse(const Vector3 &p_impulse, const Vector3 &p_position) {
	body->apply_impulse(p_impulse, p_position);
}

void Box3DDirectBodyState3D::apply_torque_impulse(const Vector3 &p_impulse) {
	body->apply_torque_impulse(p_impulse);
}

void Box3DDirectBodyState3D::apply_central_force(const Vector3 &p_force) {
	body->apply_central_force(p_force);
}

void Box3DDirectBodyState3D::apply_force(const Vector3 &p_force, const Vector3 &p_position) {
	body->apply_force(p_force, p_position);
}

void Box3DDirectBodyState3D::apply_torque(const Vector3 &p_torque) {
	body->apply_torque(p_torque);
}

void Box3DDirectBodyState3D::add_constant_central_force(const Vector3 &p_force) {
	body->add_constant_central_force(p_force);
}

void Box3DDirectBodyState3D::add_constant_force(const Vector3 &p_force, const Vector3 &p_position) {
	body->add_constant_force(p_force, p_position);
}

void Box3DDirectBodyState3D::add_constant_torque(const Vector3 &p_torque) {
	body->add_constant_torque(p_torque);
}

void Box3DDirectBodyState3D::set_constant_force(const Vector3 &p_force) {
	body->set_constant_force(p_force);
}

Vector3 Box3DDirectBodyState3D::get_constant_force() const {
	return body->get_constant_force();
}

void Box3DDirectBodyState3D::set_constant_torque(const Vector3 &p_torque) {
	body->set_constant_torque(p_torque);
}

Vector3 Box3DDirectBodyState3D::get_constant_torque() const {
	return body->get_constant_torque();
}

void Box3DDirectBodyState3D::set_sleep_state(bool p_sleep) {
	body->set_sleep_state(p_sleep);
}

void Box3DDirectBodyState3D::set_collision_layer(uint32_t p_layer) {
	body->set_collision_layer(p_layer);
}

uint32_t Box3DDirectBodyState3D::get_collision_layer() const {
	return body->get_collision_layer();
}

void Box3DDirectBodyState3D::set_collision_mask(uint32_t p_mask) {
	body->set_collision_mask(p_mask);
}

uint32_t Box3DDirectBodyState3D::get_collision_mask() const {
	return body->get_collision_mask();
}

real_t Box3DDirectBodyState3D::get_step() const {
	return body->get_space() ? body->get_space()->get_last_step() : 0.0;
}

RequiredResult<PhysicsDirectSpaceState3D> Box3DDirectBodyState3D::get_space_state() {
	return body->get_space() ? body->get_space()->get_direct_state() : nullptr;
}
