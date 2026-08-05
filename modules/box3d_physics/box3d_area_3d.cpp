/**************************************************************************/
/*  box3d_area_3d.cpp                                                      */
/**************************************************************************/

#include "box3d_area_3d.h"

#include "box3d_conversions.h"
#include "box3d_shape_scaling.h"
#include "box3d_shape_3d.h"
#include "box3d_space_3d.h"

#include "core/templates/hashfuncs.h"

static constexpr uint64_t BOX3D_QUERY_FILTER_BIT = UINT64_C(1) << 63;

uint32_t Box3DArea3D::MonitorKey::hash(const MonitorKey &p_key) {
	uint32_t h = hash_one_uint64(p_key.rid.get_id());
	h = hash_murmur3_one_64(p_key.instance_id, h);
	h = hash_murmur3_one_32(p_key.collider_shape, h);
	return hash_fmix32(hash_murmur3_one_32(p_key.self_shape, h));
}

Box3DArea3D::Box3DArea3D() :
		Box3DCollisionObject3D(TYPE_AREA) {
}

Box3DArea3D::~Box3DArea3D() {
	set_space(nullptr);
	clear_shapes();
}

bool Box3DArea3D::in_space() const {
	return space != nullptr && B3_IS_NON_NULL(body_id);
}

static b3MeshData *_clone_area_mesh_with_transform(const b3MeshData *p_mesh, const Transform3D &p_transform) {
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

void Box3DArea3D::set_space(Box3DSpace3D *p_space) {
	if (space == p_space) {
		return;
	}
	if (space) {
		if (in_space()) {
			_destroy_all_shapes();
			b3DestroyBody(body_id);
			body_id = b3_nullBodyId;
		}
		space->area_removed(this);
	}

	space = p_space;
	if (space) {
		space->area_added(this);
		Vector3 area_scale;
		const Transform3D area_transform = box3d_decompose_transform(transform, area_scale);
		scale = area_scale;
		b3BodyDef def = b3DefaultBodyDef();
		def.type = b3_staticBody;
		def.position = to_box3d(area_transform.origin);
		def.rotation = to_box3d(area_transform.basis.get_rotation_quaternion());
		def.userData = this;
		body_id = b3CreateBody(space->get_world(), &def);
		_build_all_shapes();
	}
}

void Box3DArea3D::_destroy_all_shapes() {
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

void Box3DArea3D::_build_all_shapes() {
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
		def.isSensor = true;
		def.enableSensorEvents = true;
		// Box3D filtering is a two-way mask/category AND, but Godot area detection is one-way
		// (area mask vs body layer only). Body shapes always carry the query bit in maskBits, so
		// putting it in the sensor's categoryBits makes the reverse test pass unconditionally and
		// area-vs-body reduces to exactly Godot's test. Queries are unaffected: their 32-bit masks
		// never contain the query bit, so it is inert in categoryBits there.
		def.filter.categoryBits = (uint64_t)collision_layer | BOX3D_QUERY_FILTER_BIT;
		def.filter.maskBits = (uint64_t)collision_mask | BOX3D_QUERY_FILTER_BIT;
		def.userData = (void *)(uintptr_t)i;

		b3ShapeId shape_id = b3_nullShapeId;
		const b3Vec3 unit_scale = b3Vec3{ 1.0f, 1.0f, 1.0f };
		const Transform3D scaled_shape_transform = box3d_get_scaled_shape_transform(slot.xform, scale);

		switch (s->type) {
			case PS3DE::SHAPE_SPHERE: {
				b3Sphere sphere;
				sphere.center = to_box3d(scaled_shape_transform.origin);
				sphere.radius = s->sphere_radius * box3d_get_uniform_primitive_scale(scaled_shape_transform);
				shape_id = b3CreateSphereShape(body_id, &def, &sphere);
			} break;
			case PS3DE::SHAPE_CAPSULE: {
				const float half_cylinder = MAX(0.0f, 0.5f * s->capsule_height - s->capsule_radius);
				b3Capsule capsule;
				capsule.center1 = to_box3d(scaled_shape_transform.xform(Vector3(0, half_cylinder, 0)));
				capsule.center2 = to_box3d(scaled_shape_transform.xform(Vector3(0, -half_cylinder, 0)));
				capsule.radius = s->capsule_radius * box3d_get_uniform_primitive_scale(scaled_shape_transform);
				shape_id = b3CreateCapsuleShape(body_id, &def, &capsule);
			} break;
			case PS3DE::SHAPE_BOX: {
				if (s->box_built) {
					shape_id = box3d_create_scaled_hull_shape(body_id, &def, &s->box_hull.base, slot.xform, scale);
				}
			} break;
			case PS3DE::SHAPE_CYLINDER:
			case PS3DE::SHAPE_CONVEX_POLYGON: {
				if (s->hull) {
					shape_id = box3d_create_scaled_hull_shape(body_id, &def, s->hull, slot.xform, scale);
				}
			} break;
			case PS3DE::SHAPE_CONCAVE_POLYGON:
			case PS3DE::SHAPE_HEIGHTMAP: {
				if (s->mesh) {
					b3MeshData *mesh = s->mesh;
					b3Vec3 mesh_scale = to_box3d(scale);
					if (!slot.xform.is_equal_approx(Transform3D())) {
						mesh = _clone_area_mesh_with_transform(s->mesh, scaled_shape_transform);
						if (mesh == nullptr) {
							WARN_PRINT("Box3D: failed to bake area mesh instance transform.");
							continue;
						}
						instance_meshes[i] = mesh;
						mesh_scale = unit_scale;
					}
					shape_id = b3CreateMeshShape(body_id, &def, mesh, mesh_scale);
				}
			} break;
			default:
				break;
		}

		if (B3_IS_NON_NULL(shape_id)) {
			shape_ids[i] = shape_id;
		}
	}
}

void Box3DArea3D::add_shape(RID p_shape_rid, Box3DShape3D *p_shape, const Transform3D &p_xform, bool p_disabled) {
	ShapeSlot slot;
	slot.rid = p_shape_rid;
	slot.shape = p_shape;
	slot.xform = p_xform;
	slot.disabled = p_disabled;
	slots.push_back(slot);
	p_shape->add_owner(this);
	shapes_changed();
}

void Box3DArea3D::set_shape(int p_index, RID p_shape_rid, Box3DShape3D *p_shape) {
	ERR_FAIL_INDEX(p_index, (int)slots.size());
	if (slots[p_index].shape) {
		slots[p_index].shape->remove_owner(this);
	}
	slots[p_index].rid = p_shape_rid;
	slots[p_index].shape = p_shape;
	p_shape->add_owner(this);
	shapes_changed();
}

void Box3DArea3D::set_shape_transform(int p_index, const Transform3D &p_xform) {
	ERR_FAIL_INDEX(p_index, (int)slots.size());
	slots[p_index].xform = p_xform;
	shapes_changed();
}

void Box3DArea3D::set_shape_disabled(int p_index, bool p_disabled) {
	ERR_FAIL_INDEX(p_index, (int)slots.size());
	slots[p_index].disabled = p_disabled;
	shapes_changed();
}

void Box3DArea3D::remove_shape_at(int p_index) {
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

void Box3DArea3D::remove_shape(Box3DShape3D *p_shape) {
	for (int i = (int)slots.size() - 1; i >= 0; i--) {
		if (slots[i].shape == p_shape) {
			slots.remove_at(i);
		}
	}
	p_shape->remove_owner(this);
	shapes_changed();
}

void Box3DArea3D::clear_shapes() {
	for (const ShapeSlot &slot : slots) {
		if (slot.shape) {
			slot.shape->remove_owner(this);
		}
	}
	slots.clear();
	shapes_changed();
}

const Box3DArea3D::ShapeSlot *Box3DArea3D::get_shape_slot(int p_index) const {
	ERR_FAIL_INDEX_V(p_index, (int)slots.size(), nullptr);
	return &slots[p_index];
}

void Box3DArea3D::shapes_changed() {
	if (in_space()) {
		_build_all_shapes();
	}
}

void Box3DArea3D::set_transform(const Transform3D &p_transform) {
	transform = p_transform;
	Vector3 new_scale;
	const Transform3D rigid_transform = box3d_decompose_transform(transform, new_scale);
	if (!scale.is_equal_approx(new_scale)) {
		scale = new_scale;
		shapes_changed();
	}
	if (in_space()) {
		b3Body_SetTransform(body_id, to_box3d(rigid_transform.origin), to_box3d(rigid_transform.basis.get_rotation_quaternion()));
	}
}

void Box3DArea3D::set_collision_layer(uint32_t p_layer) {
	collision_layer = p_layer;
	shapes_changed();
}

void Box3DArea3D::set_collision_mask(uint32_t p_mask) {
	collision_mask = p_mask;
	shapes_changed();
}

void Box3DArea3D::set_monitorable(bool p_monitorable) {
	// A Box3D sensor shape's enableSensorEvents flag gates its OWN sensing, so it must stay
	// enabled regardless of monitorable. Whether other areas may observe this one is enforced
	// module-side when sensor events are dispatched (Box3DSpace3D::_process_sensor_event).
	monitorable = p_monitorable;
}

void Box3DArea3D::set_param(PS3DE::AreaParameter p_param, const Variant &p_value) {
	switch (p_param) {
		case PS3DE::AREA_PARAM_GRAVITY_OVERRIDE_MODE:
			gravity_override_mode = (PS3DE::AreaSpaceOverrideMode)(int)p_value;
			break;
		case PS3DE::AREA_PARAM_GRAVITY:
			gravity = p_value;
			break;
		case PS3DE::AREA_PARAM_GRAVITY_VECTOR:
			gravity_vector = p_value;
			break;
		case PS3DE::AREA_PARAM_GRAVITY_IS_POINT:
			gravity_is_point = p_value;
			break;
		case PS3DE::AREA_PARAM_GRAVITY_POINT_UNIT_DISTANCE:
			gravity_point_unit_distance = p_value;
			break;
		case PS3DE::AREA_PARAM_LINEAR_DAMP_OVERRIDE_MODE:
			linear_damp_override_mode = (PS3DE::AreaSpaceOverrideMode)(int)p_value;
			break;
		case PS3DE::AREA_PARAM_LINEAR_DAMP:
			linear_damp = p_value;
			break;
		case PS3DE::AREA_PARAM_ANGULAR_DAMP_OVERRIDE_MODE:
			angular_damp_override_mode = (PS3DE::AreaSpaceOverrideMode)(int)p_value;
			break;
		case PS3DE::AREA_PARAM_ANGULAR_DAMP:
			angular_damp = p_value;
			break;
		case PS3DE::AREA_PARAM_PRIORITY:
			priority = p_value;
			break;
		case PS3DE::AREA_PARAM_WIND_FORCE_MAGNITUDE:
			wind_force_magnitude = p_value;
			break;
		case PS3DE::AREA_PARAM_WIND_SOURCE:
			wind_source = p_value;
			break;
		case PS3DE::AREA_PARAM_WIND_DIRECTION:
			wind_direction = p_value;
			break;
		case PS3DE::AREA_PARAM_WIND_ATTENUATION_FACTOR:
			wind_attenuation_factor = p_value;
			break;
	}
}

Variant Box3DArea3D::get_param(PS3DE::AreaParameter p_param) const {
	switch (p_param) {
		case PS3DE::AREA_PARAM_GRAVITY_OVERRIDE_MODE:
			return gravity_override_mode;
		case PS3DE::AREA_PARAM_GRAVITY:
			return gravity;
		case PS3DE::AREA_PARAM_GRAVITY_VECTOR:
			return gravity_vector;
		case PS3DE::AREA_PARAM_GRAVITY_IS_POINT:
			return gravity_is_point;
		case PS3DE::AREA_PARAM_GRAVITY_POINT_UNIT_DISTANCE:
			return gravity_point_unit_distance;
		case PS3DE::AREA_PARAM_LINEAR_DAMP_OVERRIDE_MODE:
			return linear_damp_override_mode;
		case PS3DE::AREA_PARAM_LINEAR_DAMP:
			return linear_damp;
		case PS3DE::AREA_PARAM_ANGULAR_DAMP_OVERRIDE_MODE:
			return angular_damp_override_mode;
		case PS3DE::AREA_PARAM_ANGULAR_DAMP:
			return angular_damp;
		case PS3DE::AREA_PARAM_PRIORITY:
			return priority;
		case PS3DE::AREA_PARAM_WIND_FORCE_MAGNITUDE:
			return wind_force_magnitude;
		case PS3DE::AREA_PARAM_WIND_SOURCE:
			return wind_source;
		case PS3DE::AREA_PARAM_WIND_DIRECTION:
			return wind_direction;
		case PS3DE::AREA_PARAM_WIND_ATTENUATION_FACTOR:
			return wind_attenuation_factor;
	}
	return Variant();
}

void Box3DArea3D::compute_gravity(const Vector3 &p_position, Vector3 &r_gravity) const {
	if (gravity_is_point) {
		Vector3 v = transform.xform(gravity_vector) - p_position;
		if (gravity_point_unit_distance > 0.0) {
			const real_t length_sq = v.length_squared();
			r_gravity = length_sq > 0.0 ? v.normalized() * (gravity * gravity_point_unit_distance * gravity_point_unit_distance / length_sq) : Vector3();
		} else {
			r_gravity = v.is_zero_approx() ? Vector3() : v.normalized() * gravity;
		}
	} else {
		r_gravity = gravity_vector * gravity;
	}
}

void Box3DArea3D::queue_body_event(bool p_added, RID p_body, ObjectID p_instance_id, int p_body_shape, int p_area_shape) {
	MonitorKey key;
	key.rid = p_body;
	key.instance_id = p_instance_id;
	key.collider_shape = p_body_shape;
	key.self_shape = p_area_shape;
	monitored_bodies[key].state += p_added ? 1 : -1;
}

void Box3DArea3D::queue_area_event(bool p_added, RID p_area, ObjectID p_instance_id, int p_area_shape, int p_self_shape) {
	// Monitorable gating happens on the VISITOR side (Box3DSpace3D::_process_sensor_event):
	// Godot reports area B to area A when B is monitorable, regardless of A's own flag.
	MonitorKey key;
	key.rid = p_area;
	key.instance_id = p_instance_id;
	key.collider_shape = p_area_shape;
	key.self_shape = p_self_shape;
	monitored_areas[key].state += p_added ? 1 : -1;
}

void Box3DArea3D::call_queries() {
	if (!monitored_bodies.is_empty()) {
		if (monitor_callback.is_valid()) {
			Variant res[5];
			const Variant *resptr[5] = { &res[0], &res[1], &res[2], &res[3], &res[4] };
			for (KeyValue<MonitorKey, MonitorState> &E : monitored_bodies) {
				if (E.value.state == 0) {
					continue;
				}
				res[0] = E.value.state > 0 ? PS3DE::AREA_BODY_ADDED : PS3DE::AREA_BODY_REMOVED;
				res[1] = E.key.rid;
				res[2] = E.key.instance_id;
				res[3] = (int)E.key.collider_shape;
				res[4] = (int)E.key.self_shape;
				Callable::CallError ce;
				Variant ret;
				monitor_callback.callp(resptr, 5, ret, ce);
				if (ce.error != Callable::CallError::CALL_OK) {
					ERR_PRINT_ONCE("Error calling Box3D area monitor callback: " + Variant::get_callable_error_text(monitor_callback, resptr, 5, ce));
				}
			}
		}
		monitored_bodies.clear();
	}

	if (!monitored_areas.is_empty()) {
		if (area_monitor_callback.is_valid()) {
			Variant res[5];
			const Variant *resptr[5] = { &res[0], &res[1], &res[2], &res[3], &res[4] };
			for (KeyValue<MonitorKey, MonitorState> &E : monitored_areas) {
				if (E.value.state == 0) {
					continue;
				}
				res[0] = E.value.state > 0 ? PS3DE::AREA_BODY_ADDED : PS3DE::AREA_BODY_REMOVED;
				res[1] = E.key.rid;
				res[2] = E.key.instance_id;
				res[3] = (int)E.key.collider_shape;
				res[4] = (int)E.key.self_shape;
				Callable::CallError ce;
				Variant ret;
				area_monitor_callback.callp(resptr, 5, ret, ce);
				if (ce.error != Callable::CallError::CALL_OK) {
					ERR_PRINT_ONCE("Error calling Box3D area monitor callback: " + Variant::get_callable_error_text(area_monitor_callback, resptr, 5, ce));
				}
			}
		}
		monitored_areas.clear();
	}
}
