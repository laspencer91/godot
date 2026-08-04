/**************************************************************************/
/*  box3d_body_3d.cpp                                                     */
/**************************************************************************/

#include "box3d_body_3d.h"

#include "box3d_area_3d.h"
#include "box3d_conversions.h"
#include "box3d_direct_space_state_3d.h"
#include "joints/box3d_joint_3d.h"
#include "box3d_shape_3d.h"
#include "box3d_space_3d.h"
#include "box3d_surface_materials.h"

#include "servers/physics_3d/physics_server_3d_constants.h"

#include "box3d/collision.h"
#include "box3d/math_functions.h"

static constexpr uint64_t BOX3D_QUERY_FILTER_BIT = UINT64_C(1) << 63;

Box3DBody3D::Box3DBody3D() :
		Box3DCollisionObject3D(TYPE_BODY) {
}

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
		case PS3DE::BODY_MODE_STATIC:
			return b3_staticBody;
		case PS3DE::BODY_MODE_KINEMATIC:
			return b3_kinematicBody;
		default:
			return b3_dynamicBody;
	}
}

uint64_t Box3DBody3D::_effective_mask_bits() const {
	// Godot decides a contact with a non-dynamic body solely by the DYNAMIC side's mask against
	// this body's layer — a static/kinematic body's own mask never vetoes the pair (it has no
	// contact response for the mask to gate; upstream's pair test is an OR of both directions).
	// Box3D's filter is a symmetric AND, so "my mask never vetoes" is expressed as all bits.
	// Static map geometry ships with collision_mask = 0, which under a literal translation made
	// every rigid body fall straight through it. The dynamic side's own mask still filters, so
	// this cannot create a contact that upstream Godot would not.
	if (_box3d_type() != b3_dynamicBody) {
		return UINT64_MAX;
	}
	return (uint64_t)collision_mask | BOX3D_QUERY_FILTER_BIT;
}

b3MotionLocks Box3DBody3D::_motion_locks() const {
	b3MotionLocks locks = {};
	if (mode == PS3DE::BODY_MODE_RIGID_LINEAR) {
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
			for (Box3DJoint3D *joint : joints) {
				joint->destroy_box3d_joint();
			}
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
		def.linearDamping = (float)linear_damp;
		def.angularDamping = (float)angular_damp;
		def.enableSleep = can_sleep;
		def.isAwake = !sleeping;
		def.isBullet = continuous_cd;
		def.motionLocks = _motion_locks();
		def.userData = this;
		body_id = b3CreateBody(space->get_world(), &def);
		_build_all_shapes();
		for (Box3DJoint3D *joint : joints) {
			joint->rebuild();
		}
	}
}

void Box3DBody3D::set_mode(PS3DE::BodyMode p_mode) {
	if (mode == p_mode) {
		return;
	}
	const uint64_t prev_mask_bits = _effective_mask_bits();
	mode = p_mode;
	if (mode != PS3DE::BODY_MODE_KINEMATIC) {
		has_kinematic_target = false;
	}
	if (in_space()) {
		b3Body_SetType(body_id, _box3d_type());
		b3Body_SetMotionLocks(body_id, _motion_locks());
		_update_mass();
		// The effective mask depends on dynamic-ness, and mode changes at runtime without a shape
		// rebuild — RigidBody3D.freeze IS a mode change (STATIC/KINEMATIC), so a frozen-then-thawed
		// body would otherwise keep the all-bits mask and start hitting things its mask excludes.
		if (_effective_mask_bits() != prev_mask_bits) {
			_update_shape_filters();
		}
	}
}

void Box3DBody3D::_update_shape_filters() {
	b3Filter filter = b3DefaultFilter();
	filter.categoryBits = (uint64_t)collision_layer;
	filter.maskBits = _effective_mask_bits();
	filter.groupIndex = collision_group_index;
	for (const b3ShapeId &id : shape_ids) {
		if (B3_IS_NON_NULL(id)) {
			b3Shape_SetFilter(id, filter, true);
		}
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
		def.filter.maskBits = _effective_mask_bits();
		def.filter.groupIndex = collision_group_index;
		def.enableCustomFiltering = !collision_exceptions.is_empty();
		def.enableSensorEvents = true;
		def.enableContactEvents = reports_contacts();
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
			case PS3DE::SHAPE_SPHERE: {
				b3Sphere sphere;
				sphere.center = to_box3d(slot.xform.origin);
				sphere.radius = s->sphere_radius;
				shape_id = b3CreateSphereShape(body_id, &def, &sphere);
			} break;

			case PS3DE::SHAPE_CAPSULE: {
				const float half_cylinder = MAX(0.0f, 0.5f * s->capsule_height - s->capsule_radius);
				b3Capsule capsule;
				capsule.center1 = to_box3d(slot.xform.xform(Vector3(0, half_cylinder, 0)));
				capsule.center2 = to_box3d(slot.xform.xform(Vector3(0, -half_cylinder, 0)));
				capsule.radius = s->capsule_radius;
				shape_id = b3CreateCapsuleShape(body_id, &def, &capsule);
			} break;

			case PS3DE::SHAPE_BOX: {
				if (s->box_built) {
					shape_id = b3CreateTransformedHullShape(body_id, &def, &s->box_hull.base, to_box3d(slot.xform), unit_scale);
				}
			} break;

			case PS3DE::SHAPE_CYLINDER:
			case PS3DE::SHAPE_CONVEX_POLYGON: {
				if (s->hull) {
					shape_id = b3CreateTransformedHullShape(body_id, &def, s->hull, to_box3d(slot.xform), unit_scale);
				}
			} break;

			case PS3DE::SHAPE_CONCAVE_POLYGON: {
				if (s->mesh) {
					if (mode != PS3DE::BODY_MODE_STATIC && mode != PS3DE::BODY_MODE_KINEMATIC) {
						WARN_PRINT("Box3D: concave (trimesh) shapes are only supported on static/kinematic bodies.");
						continue;
					}
					def.enableSensorEvents = false;
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

			case PS3DE::SHAPE_HEIGHTMAP: {
				if (s->mesh) {
					if (mode != PS3DE::BODY_MODE_STATIC && mode != PS3DE::BODY_MODE_KINEMATIC) {
						WARN_PRINT("Box3D: heightmap shapes are only supported on static/kinematic bodies.");
						continue;
					}
					def.enableSensorEvents = false;
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
	if (mode == PS3DE::BODY_MODE_KINEMATIC) {
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

bool Box3DBody3D::AreaRef::operator<(const AreaRef &p_ref) const {
	const int a_priority = area ? area->get_priority() : 0;
	const int b_priority = p_ref.area ? p_ref.area->get_priority() : 0;
	if (a_priority == b_priority) {
		return area < p_ref.area;
	}
	return a_priority < b_priority;
}

static void _combine_area_scalar(PS3DE::AreaSpaceOverrideMode p_mode, real_t p_value, real_t &r_total, bool &r_done) {
	switch (p_mode) {
		case PS3DE::AREA_SPACE_OVERRIDE_COMBINE:
		case PS3DE::AREA_SPACE_OVERRIDE_COMBINE_REPLACE:
			r_total += p_value;
			r_done = p_mode == PS3DE::AREA_SPACE_OVERRIDE_COMBINE_REPLACE;
			break;
		case PS3DE::AREA_SPACE_OVERRIDE_REPLACE:
		case PS3DE::AREA_SPACE_OVERRIDE_REPLACE_COMBINE:
			r_total = p_value;
			r_done = p_mode == PS3DE::AREA_SPACE_OVERRIDE_REPLACE;
			break;
		default:
			break;
	}
}

void Box3DBody3D::apply_environment_forces(float p_step) {
	if (!in_space() || mode == PS3DE::BODY_MODE_STATIC || mode == PS3DE::BODY_MODE_KINEMATIC) {
		return;
	}
	total_gravity = Vector3();
	total_linear_damp = 0.0;
	total_angular_damp = 0.0;

	bool gravity_done = false;
	bool linear_damp_done = false;
	bool angular_damp_done = false;

	if (!areas.is_empty()) {
		areas.sort();
		for (int i = (int)areas.size() - 1; i >= 0; i--) {
			Box3DArea3D *area = areas[i].area;
			if (area == nullptr) {
				continue;
			}
			if (!gravity_done) {
				const PS3DE::AreaSpaceOverrideMode area_override_mode = area->get_gravity_override_mode();
				if (area_override_mode != PS3DE::AREA_SPACE_OVERRIDE_DISABLED) {
					Vector3 area_gravity;
					area->compute_gravity(get_transform().origin, area_gravity);
					switch (area_override_mode) {
						case PS3DE::AREA_SPACE_OVERRIDE_COMBINE:
						case PS3DE::AREA_SPACE_OVERRIDE_COMBINE_REPLACE:
							total_gravity += area_gravity;
							gravity_done = area_override_mode == PS3DE::AREA_SPACE_OVERRIDE_COMBINE_REPLACE;
							break;
						case PS3DE::AREA_SPACE_OVERRIDE_REPLACE:
						case PS3DE::AREA_SPACE_OVERRIDE_REPLACE_COMBINE:
							total_gravity = area_gravity;
							gravity_done = area_override_mode == PS3DE::AREA_SPACE_OVERRIDE_REPLACE;
							break;
						default:
							break;
					}
				}
			}
			if (!linear_damp_done) {
				_combine_area_scalar(area->get_linear_damp_override_mode(), area->get_linear_damp(), total_linear_damp, linear_damp_done);
			}
			if (!angular_damp_done) {
				_combine_area_scalar(area->get_angular_damp_override_mode(), area->get_angular_damp(), total_angular_damp, angular_damp_done);
			}
			if (gravity_done && linear_damp_done && angular_damp_done) {
				break;
			}
		}
	}

	if (!gravity_done) {
		total_gravity += space->get_gravity();
	}
	if (!linear_damp_done) {
		total_linear_damp += space->get_default_linear_damp();
	}
	if (!angular_damp_done) {
		total_angular_damp += space->get_default_angular_damp();
	}

	total_gravity *= gravity_scale;
	total_linear_damp = linear_damp_mode == PS3DE::BODY_DAMP_MODE_REPLACE ? linear_damp : total_linear_damp + linear_damp;
	total_angular_damp = angular_damp_mode == PS3DE::BODY_DAMP_MODE_REPLACE ? angular_damp : total_angular_damp + angular_damp;

	// Totals above stay valid for the direct-state getters even when the user integrates
	// forces themselves (stock behavior: omit only skips the APPLICATION, custom integrators
	// still read state.total_gravity / total_*_damp).
	if (omit_force_integration) {
		b3Body_SetGravityScale(body_id, 0.0f);
		b3Body_SetLinearDamping(body_id, 0.0f);
		b3Body_SetAngularDamping(body_id, 0.0f);
		return;
	}

	const bool use_world_gravity = areas.is_empty();
	b3Body_SetGravityScale(body_id, use_world_gravity ? (float)gravity_scale : 0.0f);
	b3Body_SetLinearDamping(body_id, (float)total_linear_damp);
	b3Body_SetAngularDamping(body_id, (float)total_angular_damp);

	if (!use_world_gravity && !total_gravity.is_zero_approx()) {
		// wake=false: a sleeping body inside an override area stays asleep, matching how world
		// gravity treats sleepers. Waking here every step would make these bodies never sleep.
		b3Body_ApplyForceToCenter(body_id, to_box3d(total_gravity * b3Body_GetMass(body_id)), false);
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

void Box3DBody3D::set_param(PS3DE::BodyParameter p_param, const Variant &p_value) {
	switch (p_param) {
		case PS3DE::BODY_PARAM_FRICTION: {
			friction = p_value;
			for (uint32_t i = 0; i < shape_ids.size(); i++) {
				if (_slot_has_named_surface_material(i)) {
					WARN_PRINT_ONCE("Box3D: PS3DE::BODY_PARAM_FRICTION ignored on shapes with named Box3D surface materials.");
					continue;
				}
				b3Shape_SetFriction(shape_ids[i], (float)friction);
			}
		} break;
		case PS3DE::BODY_PARAM_BOUNCE: {
			bounce = p_value;
			for (uint32_t i = 0; i < shape_ids.size(); i++) {
				if (_slot_has_named_surface_material(i)) {
					WARN_PRINT_ONCE("Box3D: PS3DE::BODY_PARAM_BOUNCE ignored on shapes with named Box3D surface materials.");
					continue;
				}
				b3Shape_SetRestitution(shape_ids[i], (float)bounce);
			}
		} break;
		case PS3DE::BODY_PARAM_MASS: {
			mass = p_value;
			_update_mass();
		} break;
		case PS3DE::BODY_PARAM_GRAVITY_SCALE: {
			gravity_scale = p_value;
			if (in_space()) {
				b3Body_SetGravityScale(body_id, (float)gravity_scale);
			}
		} break;
		case PS3DE::BODY_PARAM_LINEAR_DAMP_MODE: {
			linear_damp_mode = (PS3DE::BodyDampMode)(int)p_value;
		} break;
		case PS3DE::BODY_PARAM_ANGULAR_DAMP_MODE: {
			angular_damp_mode = (PS3DE::BodyDampMode)(int)p_value;
		} break;
		case PS3DE::BODY_PARAM_LINEAR_DAMP: {
			linear_damp = p_value;
			if (in_space()) {
				b3Body_SetLinearDamping(body_id, (float)linear_damp);
			}
		} break;
		case PS3DE::BODY_PARAM_ANGULAR_DAMP: {
			angular_damp = p_value;
			if (in_space()) {
				b3Body_SetAngularDamping(body_id, (float)angular_damp);
			}
		} break;
		default: {
		} break;
	}
}

Variant Box3DBody3D::get_param(PS3DE::BodyParameter p_param) const {
	switch (p_param) {
		case PS3DE::BODY_PARAM_FRICTION:
			return friction;
		case PS3DE::BODY_PARAM_BOUNCE:
			return bounce;
		case PS3DE::BODY_PARAM_MASS:
			return mass;
		case PS3DE::BODY_PARAM_GRAVITY_SCALE:
			return gravity_scale;
		case PS3DE::BODY_PARAM_LINEAR_DAMP_MODE:
			return linear_damp_mode;
		case PS3DE::BODY_PARAM_ANGULAR_DAMP_MODE:
			return angular_damp_mode;
		case PS3DE::BODY_PARAM_LINEAR_DAMP:
			return linear_damp;
		case PS3DE::BODY_PARAM_ANGULAR_DAMP:
			return angular_damp;
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

void Box3DBody3D::set_collision_group_index(int p_group_index) {
	collision_group_index = p_group_index;
	shapes_changed();
}

void Box3DBody3D::add_collision_exception(RID p_body) {
	collision_exceptions.insert(p_body);
	shapes_changed();
}

void Box3DBody3D::remove_collision_exception(RID p_body) {
	collision_exceptions.erase(p_body);
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

void Box3DBody3D::set_force_integration_callback(const Callable &p_callable, const Variant &p_udata) {
	force_integration_callback = p_callable;
	force_integration_udata = p_udata;
}

void Box3DBody3D::set_omit_force_integration(bool p_omit) {
	omit_force_integration = p_omit;
}

void Box3DBody3D::set_max_contacts_reported(int p_count) {
	ERR_FAIL_COND(p_count < 0);
	ERR_FAIL_COND(p_count > PS3DC::MAX_CONTACTS_REPORTED_3D_MAX);
	contacts.resize(p_count);
	contact_count = 0;
	for (const b3ShapeId &shape_id : shape_ids) {
		if (B3_IS_NON_NULL(shape_id)) {
			b3Shape_EnableContactEvents(shape_id, p_count > 0);
		}
	}
}

void Box3DBody3D::clear_reported_contacts() {
	contact_count = 0;
}

void Box3DBody3D::add_contact(const Contact &p_contact) {
	const int max_contacts = contacts.size();
	if (max_contacts == 0) {
		return;
	}
	int index = contact_count;
	if (contact_count < max_contacts) {
		contact_count++;
	} else {
		real_t least_depth = 1e20;
		index = 0;
		for (int i = 0; i < max_contacts; i++) {
			if (contacts[i].depth < least_depth) {
				least_depth = contacts[i].depth;
				index = i;
			}
		}
		if (least_depth >= p_contact.depth) {
			return;
		}
	}
	contacts.write[index] = p_contact;
}

Box3DDirectBodyState3D *Box3DBody3D::get_direct_state() {
	if (!direct_state) {
		direct_state = memnew(Box3DDirectBodyState3D);
		direct_state->body = this;
	}
	return direct_state;
}

void Box3DBody3D::add_area(Box3DArea3D *p_area) {
	for (AreaRef &ref : areas) {
		if (ref.area == p_area) {
			ref.ref_count++;
			return;
		}
	}
	AreaRef ref;
	ref.area = p_area;
	ref.ref_count = 1;
	areas.push_back(ref);
}

void Box3DBody3D::remove_area(Box3DArea3D *p_area) {
	for (uint32_t i = 0; i < areas.size(); i++) {
		if (areas[i].area == p_area) {
			areas[i].ref_count--;
			if (areas[i].ref_count <= 0) {
				areas.remove_at(i);
			}
			return;
		}
	}
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
	if (force_integration_callback.is_valid()) {
		Variant direct_state_variant = get_direct_state();
		Variant ret;
		Callable::CallError ce;
		if (force_integration_udata.get_type() == Variant::NIL) {
			const Variant *args[1] = { &direct_state_variant };
			force_integration_callback.callp(args, 1, ret, ce);
		} else {
			const Variant *args[2] = { &direct_state_variant, &force_integration_udata };
			force_integration_callback.callp(args, 2, ret, ce);
		}
		if (ce.error != Callable::CallError::CALL_OK) {
			ERR_PRINT_ONCE("Error calling Box3D force integration callback.");
		}
	}
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

Vector3 Box3DDirectBodyState3D::get_inverse_inertia() const {
	if (!body->in_space()) {
		return Vector3();
	}
	const b3MassData mass_data = b3Body_GetMassData(body->body_id);
	return Vector3(
			mass_data.inertia.cx.x != 0.0f ? 1.0f / mass_data.inertia.cx.x : 0.0f,
			mass_data.inertia.cy.y != 0.0f ? 1.0f / mass_data.inertia.cy.y : 0.0f,
			mass_data.inertia.cz.z != 0.0f ? 1.0f / mass_data.inertia.cz.z : 0.0f);
}

Basis Box3DDirectBodyState3D::get_inverse_inertia_tensor() const {
	Basis basis;
	basis.scale(get_inverse_inertia());
	return basis;
}

Basis Box3DDirectBodyState3D::get_principal_inertia_axes() const {
	return Basis();
}

Vector3 Box3DDirectBodyState3D::get_center_of_mass() const {
	if (!body->in_space()) {
		return Vector3();
	}
	return to_godot(b3Body_GetWorldPoint(body->body_id, b3Body_GetLocalCenter(body->body_id)));
}

Vector3 Box3DDirectBodyState3D::get_center_of_mass_local() const {
	if (!body->in_space()) {
		return Vector3();
	}
	return to_godot(b3Body_GetLocalCenter(body->body_id));
}

Vector3 Box3DDirectBodyState3D::get_total_gravity() const {
	return body->total_gravity;
}

real_t Box3DDirectBodyState3D::get_total_linear_damp() const {
	return body->total_linear_damp;
}

real_t Box3DDirectBodyState3D::get_total_angular_damp() const {
	return body->total_angular_damp;
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

int Box3DDirectBodyState3D::get_contact_count() const {
	return body->contact_count;
}

Vector3 Box3DDirectBodyState3D::get_contact_local_position(int p_contact_idx) const {
	ERR_FAIL_INDEX_V(p_contact_idx, body->contact_count, Vector3());
	return body->contacts[p_contact_idx].local_pos;
}

Vector3 Box3DDirectBodyState3D::get_contact_local_normal(int p_contact_idx) const {
	ERR_FAIL_INDEX_V(p_contact_idx, body->contact_count, Vector3());
	return body->contacts[p_contact_idx].local_normal;
}

Vector3 Box3DDirectBodyState3D::get_contact_impulse(int p_contact_idx) const {
	ERR_FAIL_INDEX_V(p_contact_idx, body->contact_count, Vector3());
	return body->contacts[p_contact_idx].impulse;
}

int Box3DDirectBodyState3D::get_contact_local_shape(int p_contact_idx) const {
	ERR_FAIL_INDEX_V(p_contact_idx, body->contact_count, -1);
	return body->contacts[p_contact_idx].local_shape;
}

Vector3 Box3DDirectBodyState3D::get_contact_local_velocity_at_position(int p_contact_idx) const {
	ERR_FAIL_INDEX_V(p_contact_idx, body->contact_count, Vector3());
	return body->contacts[p_contact_idx].local_velocity_at_pos;
}

RID Box3DDirectBodyState3D::get_contact_collider(int p_contact_idx) const {
	ERR_FAIL_INDEX_V(p_contact_idx, body->contact_count, RID());
	return body->contacts[p_contact_idx].collider;
}

Vector3 Box3DDirectBodyState3D::get_contact_collider_position(int p_contact_idx) const {
	ERR_FAIL_INDEX_V(p_contact_idx, body->contact_count, Vector3());
	return body->contacts[p_contact_idx].collider_pos;
}

ObjectID Box3DDirectBodyState3D::get_contact_collider_id(int p_contact_idx) const {
	ERR_FAIL_INDEX_V(p_contact_idx, body->contact_count, ObjectID());
	return body->contacts[p_contact_idx].collider_instance_id;
}

int Box3DDirectBodyState3D::get_contact_collider_shape(int p_contact_idx) const {
	ERR_FAIL_INDEX_V(p_contact_idx, body->contact_count, -1);
	return body->contacts[p_contact_idx].collider_shape;
}

Vector3 Box3DDirectBodyState3D::get_contact_collider_velocity_at_position(int p_contact_idx) const {
	ERR_FAIL_INDEX_V(p_contact_idx, body->contact_count, Vector3());
	return body->contacts[p_contact_idx].collider_velocity_at_pos;
}

real_t Box3DDirectBodyState3D::get_step() const {
	return body->get_space() ? body->get_space()->get_last_step() : 0.0;
}

RequiredResult<PhysicsDirectSpaceState3D> Box3DDirectBodyState3D::get_space_state() {
	return body->get_space() ? body->get_space()->get_direct_state() : nullptr;
}
