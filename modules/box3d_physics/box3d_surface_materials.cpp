/**************************************************************************/
/*  box3d_surface_materials.cpp                                           */
/**************************************************************************/

#include "box3d_surface_materials.h"

#include "box3d_conversions.h"
#include "box3d_physics_server_3d.h"

#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "core/io/resource_loader.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "scene/3d/physics/collision_object_3d.h"

static PackedInt64Array _resolve_surface_map_material_ids(Box3DPhysics *p_box3d_physics, const Ref<Box3DSurfaceMap> &p_surface_map) {
	PackedInt64Array material_ids;
	ERR_FAIL_COND_V(p_surface_map.is_null(), material_ids);

	const PackedStringArray material_names = p_surface_map->get_material_names();
	material_ids.resize(material_names.size());
	int64_t *write = material_ids.ptrw();
	for (int i = 0; i < material_names.size(); i++) {
		const StringName name = material_names[i];
		const int material_id = p_box3d_physics->get_material_id(name);
		if (name != StringName() && material_id == 0) {
			WARN_PRINT(vformat("Box3D: unknown surface material '%s' in Box3DSurfaceMap.", String(name)));
		}
		write[i] = material_id;
	}
	return material_ids;
}

static void _apply_surface_map_to_owner(Box3DPhysics *p_box3d_physics, CollisionObject3D *p_body, uint32_t p_owner, int p_shape_idx, const PackedInt64Array &p_material_ids, const PackedByteArray &p_triangle_indices) {
	const int shape_count = p_body->shape_owner_get_shape_count(p_owner);
	for (int i = 0; i < shape_count; i++) {
		const int body_shape_idx = p_body->shape_owner_get_shape_index(p_owner, i);
		if (p_shape_idx != -1 && body_shape_idx != p_shape_idx) {
			continue;
		}
		Ref<Shape3D> shape = p_body->shape_owner_get_shape(p_owner, i);
		if (shape.is_valid()) {
			p_box3d_physics->shape_set_surface_map(shape->get_rid(), p_material_ids, p_triangle_indices);
		}
	}
}

void Box3DSurfaceMaterial::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_material_name", "name"), &Box3DSurfaceMaterial::set_material_name);
	ClassDB::bind_method(D_METHOD("get_material_name"), &Box3DSurfaceMaterial::get_material_name);
	ClassDB::bind_method(D_METHOD("set_material_id", "id"), &Box3DSurfaceMaterial::set_material_id);
	ClassDB::bind_method(D_METHOD("get_material_id"), &Box3DSurfaceMaterial::get_material_id);
	ClassDB::bind_method(D_METHOD("set_friction", "friction"), &Box3DSurfaceMaterial::set_friction);
	ClassDB::bind_method(D_METHOD("get_friction"), &Box3DSurfaceMaterial::get_friction);
	ClassDB::bind_method(D_METHOD("set_restitution", "restitution"), &Box3DSurfaceMaterial::set_restitution);
	ClassDB::bind_method(D_METHOD("get_restitution"), &Box3DSurfaceMaterial::get_restitution);
	ClassDB::bind_method(D_METHOD("set_rolling_resistance", "rolling_resistance"), &Box3DSurfaceMaterial::set_rolling_resistance);
	ClassDB::bind_method(D_METHOD("get_rolling_resistance"), &Box3DSurfaceMaterial::get_rolling_resistance);
	ClassDB::bind_method(D_METHOD("set_tangent_velocity", "velocity"), &Box3DSurfaceMaterial::set_tangent_velocity);
	ClassDB::bind_method(D_METHOD("get_tangent_velocity"), &Box3DSurfaceMaterial::get_tangent_velocity);
	ClassDB::bind_method(D_METHOD("set_debug_color", "color"), &Box3DSurfaceMaterial::set_debug_color);
	ClassDB::bind_method(D_METHOD("get_debug_color"), &Box3DSurfaceMaterial::get_debug_color);
	ClassDB::bind_method(D_METHOD("set_custom", "custom"), &Box3DSurfaceMaterial::set_custom);
	ClassDB::bind_method(D_METHOD("get_custom"), &Box3DSurfaceMaterial::get_custom);
	ClassDB::bind_method(D_METHOD("set_texture_patterns", "patterns"), &Box3DSurfaceMaterial::set_texture_patterns);
	ClassDB::bind_method(D_METHOD("get_texture_patterns"), &Box3DSurfaceMaterial::get_texture_patterns);

	ADD_PROPERTY(PropertyInfo(Variant::STRING_NAME, "material_name"), "set_material_name", "get_material_name");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "material_id", PROPERTY_HINT_RANGE, "0,2147483647,1"), "set_material_id", "get_material_id");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "friction", PROPERTY_HINT_RANGE, "0,10,0.001,or_greater"), "set_friction", "get_friction");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "restitution", PROPERTY_HINT_RANGE, "0,1,0.001"), "set_restitution", "get_restitution");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "rolling_resistance", PROPERTY_HINT_RANGE, "0,1,0.001"), "set_rolling_resistance", "get_rolling_resistance");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "tangent_velocity"), "set_tangent_velocity", "get_tangent_velocity");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "debug_color"), "set_debug_color", "get_debug_color");
	ADD_PROPERTY(PropertyInfo(Variant::DICTIONARY, "custom"), "set_custom", "get_custom");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_STRING_ARRAY, "texture_patterns"), "set_texture_patterns", "get_texture_patterns");
}

b3SurfaceMaterial Box3DSurfaceMaterial::to_box3d() const {
	b3SurfaceMaterial material = b3DefaultSurfaceMaterial();
	material.friction = friction;
	material.restitution = restitution;
	material.rollingResistance = rolling_resistance;
	material.tangentVelocity = ::to_box3d(tangent_velocity);
	material.userMaterialId = (uint64_t)material_id;
	material.customColor = debug_color.a > 0.0f ? ((uint32_t)(debug_color.r * 255.0f) << 16) | ((uint32_t)(debug_color.g * 255.0f) << 8) | (uint32_t)(debug_color.b * 255.0f) : 0;
	return material;
}

void Box3DSurfaceMaterialLibrary::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_materials", "materials"), &Box3DSurfaceMaterialLibrary::set_materials);
	ClassDB::bind_method(D_METHOD("get_materials"), &Box3DSurfaceMaterialLibrary::get_materials);
	ClassDB::bind_method(D_METHOD("find_material", "name"), &Box3DSurfaceMaterialLibrary::find_material);
	ClassDB::bind_method(D_METHOD("allocate_material_id"), &Box3DSurfaceMaterialLibrary::allocate_material_id);
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "materials", PROPERTY_HINT_ARRAY_TYPE, "Box3DSurfaceMaterial"), "set_materials", "get_materials");
}

int Box3DSurfaceMaterialLibrary::find_material_index(const StringName &p_name) const {
	for (int i = 0; i < materials.size(); i++) {
		Ref<Box3DSurfaceMaterial> material = materials[i];
		if (material.is_valid() && material->get_material_name() == p_name) {
			return i;
		}
	}
	return -1;
}

Ref<Box3DSurfaceMaterial> Box3DSurfaceMaterialLibrary::find_material(const StringName &p_name) const {
	const int index = find_material_index(p_name);
	return index == -1 ? Ref<Box3DSurfaceMaterial>() : Ref<Box3DSurfaceMaterial>(materials[index]);
}

int Box3DSurfaceMaterialLibrary::_next_material_id(const TypedArray<Box3DSurfaceMaterial> &p_materials) {
	// Ids are stable handles baked into shapes and recordings, so never reuse a
	// freed id: hand out one past the current maximum.
	int max_id = 0;
	for (int i = 0; i < p_materials.size(); i++) {
		const Ref<Box3DSurfaceMaterial> surface_material = p_materials[i];
		if (surface_material.is_valid()) {
			max_id = MAX(max_id, surface_material->get_material_id());
		}
	}
	return max_id + 1;
}

int Box3DSurfaceMaterialLibrary::allocate_material_id() const {
	return _next_material_id(materials);
}

StringName Box3DSurfaceMaterialLibrary::_unique_name(const TypedArray<Box3DSurfaceMaterial> &p_materials, const StringName &p_base, int p_ignore_index) {
	HashSet<String> taken;
	for (int i = 0; i < p_materials.size(); i++) {
		// The entry being renamed must not count its own current name as taken.
		if (i == p_ignore_index) {
			continue;
		}
		const Ref<Box3DSurfaceMaterial> surface_material = p_materials[i];
		if (surface_material.is_valid()) {
			taken.insert(String(surface_material->get_material_name()));
		}
	}

	const String base = String(p_base);
	if (!taken.has(base)) {
		return p_base;
	}
	// Candidates stay String until one is accepted: a StringName is interned in a global
	// table for the life of the process, so rejected suffixes must never become one.
	for (int suffix = 2;; suffix++) {
		const String candidate = base + " " + itos(suffix);
		if (!taken.has(candidate)) {
			return candidate;
		}
	}
}

StringName Box3DSurfaceMaterialLibrary::make_unique_name(const StringName &p_base) const {
	return _unique_name(materials, p_base, -1);
}

void Box3DSurfaceMaterialLibrary::set_materials(const TypedArray<Box3DSurfaceMaterial> &p_materials) {
	// A duplicate name or id makes the later entry invisible to the registry, the Physics
	// tab and every inspector dropdown while it still sits in the .tres, which reads as
	// silent data loss. Heal it here: this is the one choke point every ingest path goes
	// through (resource loading, the settings tab's undo/redo, scripts setting in bulk).
	// First occurrence always wins, and a valid array is left completely untouched.
	HashSet<StringName> seen_names;
	HashSet<int> seen_ids;
	for (int i = 0; i < p_materials.size(); i++) {
		Ref<Box3DSurfaceMaterial> surface_material = p_materials[i];
		if (surface_material.is_null()) {
			continue;
		}

		const StringName material_name = surface_material->get_material_name();
		if (material_name == StringName() || seen_names.has(material_name)) {
			const StringName unique_name = _unique_name(p_materials, material_name == StringName() ? StringName("New Material") : material_name, i);
			WARN_PRINT(vformat("Box3D: surface material name '%s' is empty or already used; renamed to '%s'.", String(material_name), String(unique_name)));
			surface_material->set_material_name(unique_name);
			seen_names.insert(unique_name);
		} else {
			seen_names.insert(material_name);
		}

		const int id = surface_material->get_material_id();
		if (id <= 0 || seen_ids.has(id)) {
			// One past the maximum id in the array, so the fresh id can collide with neither
			// an earlier nor a later entry, and no retired id is ever handed out again.
			const int unique_id = _next_material_id(p_materials);
			WARN_PRINT(vformat("Box3D: surface material '%s' has an invalid or already used id %d; reassigned to %d.", String(surface_material->get_material_name()), id, unique_id));
			surface_material->set_material_id(unique_id);
			seen_ids.insert(unique_id);
		} else {
			seen_ids.insert(id);
		}
	}

	materials = p_materials;
}

void Box3DSurfaceMap::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_material_names", "names"), &Box3DSurfaceMap::set_material_names);
	ClassDB::bind_method(D_METHOD("get_material_names"), &Box3DSurfaceMap::get_material_names);
	ClassDB::bind_method(D_METHOD("set_triangle_indices", "indices"), &Box3DSurfaceMap::set_triangle_indices);
	ClassDB::bind_method(D_METHOD("get_triangle_indices"), &Box3DSurfaceMap::get_triangle_indices);
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_STRING_ARRAY, "material_names"), "set_material_names", "get_material_names");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_BYTE_ARRAY, "triangle_indices"), "set_triangle_indices", "get_triangle_indices");
}

void Box3DPhysics::_bind_methods() {
	ClassDB::bind_method(D_METHOD("reload_surface_material_library"), &Box3DPhysics::reload_surface_material_library);
	ClassDB::bind_method(D_METHOD("get_surface_material_library"), &Box3DPhysics::get_surface_material_library);
	ClassDB::bind_method(D_METHOD("get_material_id", "name"), &Box3DPhysics::get_material_id);
	ClassDB::bind_method(D_METHOD("get_material_name", "id"), &Box3DPhysics::get_material_name);
	ClassDB::bind_method(D_METHOD("get_material", "id_or_name"), &Box3DPhysics::get_material);
	ClassDB::bind_method(D_METHOD("has_material", "name"), &Box3DPhysics::has_material);
	ClassDB::bind_method(D_METHOD("get_material_names"), &Box3DPhysics::get_material_names);
	ClassDB::bind_method(D_METHOD("get_materials"), &Box3DPhysics::get_materials);
	ClassDB::bind_method(D_METHOD("match_texture", "texture_name"), &Box3DPhysics::match_texture);
	ClassDB::bind_method(D_METHOD("shape_set_surface_material", "shape", "material_id"), &Box3DPhysics::shape_set_surface_material);
	ClassDB::bind_method(D_METHOD("shape_set_surface_map", "shape", "material_ids", "triangle_indices"), &Box3DPhysics::shape_set_surface_map);
	ClassDB::bind_method(D_METHOD("body_set_surface_material", "body", "shape_idx", "material_id"), &Box3DPhysics::body_set_surface_material);
	ClassDB::bind_method(D_METHOD("get_shape_mesh_material_indices", "shape"), &Box3DPhysics::get_shape_mesh_material_indices);
	ClassDB::bind_method(D_METHOD("get_face_material", "shape", "face_index"), &Box3DPhysics::get_face_material);
	ClassDB::bind_method(D_METHOD("joint_set_box3d_param", "joint", "param", "value"), &Box3DPhysics::joint_set_box3d_param);
	ClassDB::bind_method(D_METHOD("joint_get_box3d_param", "joint", "param"), &Box3DPhysics::joint_get_box3d_param);
	ClassDB::bind_method(D_METHOD("joint_set_box3d_target_rotation", "joint", "target_rotation"), &Box3DPhysics::joint_set_box3d_target_rotation);
	ClassDB::bind_method(D_METHOD("joint_get_box3d_target_rotation", "joint"), &Box3DPhysics::joint_get_box3d_target_rotation);
	ClassDB::bind_method(D_METHOD("joint_set_box3d_motor_velocity", "joint", "velocity"), &Box3DPhysics::joint_set_box3d_motor_velocity);
	ClassDB::bind_method(D_METHOD("joint_get_box3d_motor_velocity", "joint"), &Box3DPhysics::joint_get_box3d_motor_velocity);
	ClassDB::bind_method(D_METHOD("recording_start", "space", "byte_capacity"), &Box3DPhysics::recording_start, DEFVAL(0));
	ClassDB::bind_method(D_METHOD("recording_stop", "space"), &Box3DPhysics::recording_stop);
	ClassDB::bind_method(D_METHOD("recording_is_active", "space"), &Box3DPhysics::recording_is_active);
	ClassDB::bind_method(D_METHOD("recording_get_size", "space"), &Box3DPhysics::recording_get_size);
	ClassDB::bind_method(D_METHOD("recording_save", "space", "path"), &Box3DPhysics::recording_save);
	ClassDB::bind_method(D_METHOD("recording_validate", "data", "worker_count"), &Box3DPhysics::recording_validate, DEFVAL(1));
	ClassDB::bind_method(D_METHOD("recording_validate_file", "path", "worker_count"), &Box3DPhysics::recording_validate_file, DEFVAL(1));

	// Emitted whenever the registry is rebuilt, so inspector dropdowns and the Physics
	// tab can refresh without polling.
	ADD_SIGNAL(MethodInfo("surface_materials_changed"));

	BIND_ENUM_CONSTANT(Box3DPhysicsServer3D::BOX3D_JOINT_CONSTRAINT_HERTZ);
	BIND_ENUM_CONSTANT(Box3DPhysicsServer3D::BOX3D_JOINT_CONSTRAINT_DAMPING_RATIO);
	BIND_ENUM_CONSTANT(Box3DPhysicsServer3D::BOX3D_JOINT_FORCE_THRESHOLD);
	BIND_ENUM_CONSTANT(Box3DPhysicsServer3D::BOX3D_JOINT_TORQUE_THRESHOLD);
	BIND_ENUM_CONSTANT(Box3DPhysicsServer3D::BOX3D_JOINT_SPHERICAL_SPRING_HERTZ);
	BIND_ENUM_CONSTANT(Box3DPhysicsServer3D::BOX3D_JOINT_SPHERICAL_SPRING_DAMPING_RATIO);
	BIND_ENUM_CONSTANT(Box3DPhysicsServer3D::BOX3D_JOINT_SPHERICAL_MAX_MOTOR_TORQUE);
}

Box3DPhysics::Box3DPhysics() {
	singleton = this;
}

Box3DPhysics::~Box3DPhysics() {
	if (singleton == this) {
		singleton = nullptr;
	}
}

void Box3DPhysics::register_project_settings() {
	// Defaulted like audio/buses/default_bus_layout: the Physics tab in Project Settings
	// creates and owns this file, so the path is a conventional default rather than
	// something the user is expected to pick in a file dialog.
	GLOBAL_DEF(PropertyInfo(Variant::STRING, "physics/box3d/surface_material_library", PROPERTY_HINT_FILE, "*.tres,*.res"), "res://box3d_surface_materials.tres");
	GLOBAL_DEF(PropertyInfo(Variant::FLOAT, "physics/box3d/joints/constraint_hertz", PROPERTY_HINT_RANGE, U"0,240,0.1,or_greater,suffix:Hz"), 60.0f);
	GLOBAL_DEF(PropertyInfo(Variant::FLOAT, "physics/box3d/joints/constraint_damping_ratio", PROPERTY_HINT_RANGE, U"0,10,0.01,or_greater"), 2.0f);
}

String Box3DPhysics::get_surface_material_library_path() {
	return GLOBAL_GET("physics/box3d/surface_material_library");
}

void Box3DPhysics::_ensure_surface_material_library() const {
	if (library_resolved) {
		return;
	}
	// Project libraries may subclass the native resource in GDScript or a GDExtension. Loading
	// during module initialization is too early for those types, so resolve on first real use.
	const_cast<Box3DPhysics *>(this)->reload_surface_material_library();
}

void Box3DPhysics::reload_surface_material_library() {
	library.unref();
	materials_by_name.clear();
	materials_by_id.clear();
	library_resolved = true;

	const String path = get_surface_material_library_path();
	// A project that has never opened the Physics tab simply has no library file yet.
	// That is the empty-registry case, not an error.
	if (!path.is_empty() && ResourceLoader::exists(path)) {
		library = ResourceLoader::load(path);
		if (library.is_null()) {
			ERR_PRINT("Box3D: surface material library setting does not point to a Box3DSurfaceMaterialLibrary resource.");
		}
	}

	if (library.is_valid()) {
		TypedArray<Box3DSurfaceMaterial> materials = library->get_materials();
		HashSet<StringName> seen_names;
		HashSet<int> seen_ids;
		for (int i = 0; i < materials.size(); i++) {
			Ref<Box3DSurfaceMaterial> material = materials[i];
			if (material.is_null()) {
				continue;
			}
			const StringName name = material->get_material_name();
			const int id = material->get_material_id();
			if (name == StringName() || id <= 0) {
				WARN_PRINT("Box3D: surface material entries need a non-empty name and id > 0.");
				continue;
			}
			if (seen_names.has(name) || seen_ids.has(id)) {
				// Box3DSurfaceMaterialLibrary::set_materials() uniquifies on ingest, so a collision
				// here means the name or id was changed in place on an already-registered material.
				WARN_PRINT(vformat("Box3D: surface material '%s' (id %d) collides with an earlier entry and was not registered. Its name or id was modified in place after the library was loaded.", String(name), id));
				continue;
			}
			seen_names.insert(name);
			seen_ids.insert(id);
			materials_by_name[name] = material;
			materials_by_id[id] = material;
		}
	}

	emit_signal(SNAME("surface_materials_changed"));
}

bool Box3DPhysics::has_material(const StringName &p_name) const {
	_ensure_surface_material_library();
	return materials_by_name.has(p_name);
}

PackedStringArray Box3DPhysics::get_material_names() const {
	const TypedArray<Box3DSurfaceMaterial> materials = get_materials();
	PackedStringArray names;
	names.resize(materials.size());
	String *names_write = names.ptrw();
	for (int i = 0; i < materials.size(); i++) {
		names_write[i] = String(Ref<Box3DSurfaceMaterial>(materials[i])->get_material_name());
	}
	return names;
}

TypedArray<Box3DSurfaceMaterial> Box3DPhysics::get_materials() const {
	_ensure_surface_material_library();
	TypedArray<Box3DSurfaceMaterial> result;
	if (library.is_null()) {
		return result;
	}
	// Iterate the library rather than the hash map so the order matches the Physics tab.
	TypedArray<Box3DSurfaceMaterial> library_materials = library->get_materials();
	for (int i = 0; i < library_materials.size(); i++) {
		Ref<Box3DSurfaceMaterial> material = library_materials[i];
		if (material.is_valid() && materials_by_name.has(material->get_material_name())) {
			result.push_back(material);
		}
	}
	return result;
}

int Box3DPhysics::get_material_id(const StringName &p_name) const {
	_ensure_surface_material_library();
	const Ref<Box3DSurfaceMaterial> *material = materials_by_name.getptr(p_name);
	return material && material->is_valid() ? (*material)->get_material_id() : 0;
}

StringName Box3DPhysics::get_material_name(int p_id) const {
	_ensure_surface_material_library();
	const Ref<Box3DSurfaceMaterial> *material = materials_by_id.getptr(p_id);
	return material && material->is_valid() ? (*material)->get_material_name() : StringName();
}

Ref<Box3DSurfaceMaterial> Box3DPhysics::get_material(const Variant &p_id_or_name) const {
	_ensure_surface_material_library();
	if (p_id_or_name.get_type() == Variant::INT) {
		const Ref<Box3DSurfaceMaterial> *material = materials_by_id.getptr((int)p_id_or_name);
		return material ? *material : Ref<Box3DSurfaceMaterial>();
	}
	const Ref<Box3DSurfaceMaterial> *material = materials_by_name.getptr((StringName)p_id_or_name);
	return material ? *material : Ref<Box3DSurfaceMaterial>();
}

int Box3DPhysics::match_texture(const String &p_texture_name) const {
	_ensure_surface_material_library();
	for (const KeyValue<StringName, Ref<Box3DSurfaceMaterial>> &kv : materials_by_name) {
		PackedStringArray patterns = kv.value->get_texture_patterns();
		for (int i = 0; i < patterns.size(); i++) {
			if (p_texture_name.matchn(patterns[i])) {
				return kv.value->get_material_id();
			}
		}
	}
	return 0;
}

b3SurfaceMaterial Box3DPhysics::get_box3d_material(int p_id) const {
	Ref<Box3DSurfaceMaterial> material = get_material(p_id);
	return material.is_valid() ? material->to_box3d() : b3DefaultSurfaceMaterial();
}

void Box3DPhysics::shape_set_surface_material(RID p_shape, int p_material_id) {
	Box3DPhysicsServer3D *server = Box3DPhysicsServer3D::get_singleton();
	ERR_FAIL_NULL(server);
	server->shape_set_surface_material(p_shape, p_material_id);
}

void Box3DPhysics::shape_set_surface_map(RID p_shape, const PackedInt64Array &p_material_ids, const PackedByteArray &p_triangle_indices) {
	Box3DPhysicsServer3D *server = Box3DPhysicsServer3D::get_singleton();
	ERR_FAIL_NULL(server);
	server->shape_set_surface_map(p_shape, p_material_ids, p_triangle_indices);
}

void Box3DPhysics::body_set_surface_material(RID p_body, int p_shape_idx, int p_material_id) {
	Box3DPhysicsServer3D *server = Box3DPhysicsServer3D::get_singleton();
	ERR_FAIL_NULL(server);
	server->body_set_surface_material(p_body, p_shape_idx, p_material_id);
}

PackedByteArray Box3DPhysics::get_shape_mesh_material_indices(RID p_shape) const {
	Box3DPhysicsServer3D *server = Box3DPhysicsServer3D::get_singleton();
	ERR_FAIL_NULL_V(server, PackedByteArray());
	return server->shape_get_mesh_material_indices(p_shape);
}

Ref<Box3DSurfaceMaterial> Box3DPhysics::get_face_material(RID p_shape, int p_face_index) const {
	Box3DPhysicsServer3D *server = Box3DPhysicsServer3D::get_singleton();
	ERR_FAIL_NULL_V(server, Ref<Box3DSurfaceMaterial>());
	return get_material(server->shape_get_face_material_id(p_shape, p_face_index));
}

void Box3DPhysics::joint_set_box3d_param(RID p_joint, int p_param, real_t p_value) {
	Box3DPhysicsServer3D *server = Box3DPhysicsServer3D::get_singleton();
	ERR_FAIL_NULL(server);
	server->joint_set_box3d_param(p_joint, (Box3DPhysicsServer3D::Box3DJointParam)p_param, p_value);
}

real_t Box3DPhysics::joint_get_box3d_param(RID p_joint, int p_param) const {
	Box3DPhysicsServer3D *server = Box3DPhysicsServer3D::get_singleton();
	ERR_FAIL_NULL_V(server, 0.0);
	return server->joint_get_box3d_param(p_joint, (Box3DPhysicsServer3D::Box3DJointParam)p_param);
}

void Box3DPhysics::joint_set_box3d_target_rotation(RID p_joint, const Quaternion &p_target_rotation) {
	Box3DPhysicsServer3D *server = Box3DPhysicsServer3D::get_singleton();
	ERR_FAIL_NULL(server);
	server->joint_set_box3d_target_rotation(p_joint, p_target_rotation);
}

Quaternion Box3DPhysics::joint_get_box3d_target_rotation(RID p_joint) const {
	Box3DPhysicsServer3D *server = Box3DPhysicsServer3D::get_singleton();
	ERR_FAIL_NULL_V(server, Quaternion());
	return server->joint_get_box3d_target_rotation(p_joint);
}

void Box3DPhysics::joint_set_box3d_motor_velocity(RID p_joint, const Vector3 &p_velocity) {
	Box3DPhysicsServer3D *server = Box3DPhysicsServer3D::get_singleton();
	ERR_FAIL_NULL(server);
	server->joint_set_box3d_motor_velocity(p_joint, p_velocity);
}

Vector3 Box3DPhysics::joint_get_box3d_motor_velocity(RID p_joint) const {
	Box3DPhysicsServer3D *server = Box3DPhysicsServer3D::get_singleton();
	ERR_FAIL_NULL_V(server, Vector3());
	return server->joint_get_box3d_motor_velocity(p_joint);
}

bool Box3DPhysics::recording_start(RID p_space, int p_byte_capacity) {
	Box3DPhysicsServer3D *server = Box3DPhysicsServer3D::get_singleton();
	ERR_FAIL_NULL_V(server, false);
	return server->space_start_recording(p_space, p_byte_capacity);
}

PackedByteArray Box3DPhysics::recording_stop(RID p_space) {
	Box3DPhysicsServer3D *server = Box3DPhysicsServer3D::get_singleton();
	ERR_FAIL_NULL_V(server, PackedByteArray());
	return server->space_stop_recording(p_space);
}

bool Box3DPhysics::recording_is_active(RID p_space) const {
	Box3DPhysicsServer3D *server = Box3DPhysicsServer3D::get_singleton();
	ERR_FAIL_NULL_V(server, false);
	return server->space_is_recording(p_space);
}

int Box3DPhysics::recording_get_size(RID p_space) const {
	Box3DPhysicsServer3D *server = Box3DPhysicsServer3D::get_singleton();
	ERR_FAIL_NULL_V(server, 0);
	return server->space_get_recording_size(p_space);
}

bool Box3DPhysics::recording_save(RID p_space, const String &p_path) const {
	Box3DPhysicsServer3D *server = Box3DPhysicsServer3D::get_singleton();
	ERR_FAIL_NULL_V(server, false);
	String path = p_path;
	if (ProjectSettings::get_singleton() != nullptr) {
		path = ProjectSettings::get_singleton()->globalize_path(path);
	}
	return server->space_save_recording(p_space, path);
}

bool Box3DPhysics::recording_validate(const PackedByteArray &p_data, int p_worker_count) const {
	ERR_FAIL_COND_V_MSG(p_data.is_empty(), false, "Box3D: cannot validate an empty recording.");
	return b3ValidateReplay(p_data.ptr(), p_data.size(), MAX(1, p_worker_count));
}

bool Box3DPhysics::recording_validate_file(const String &p_path, int p_worker_count) const {
	String path = p_path;
	if (ProjectSettings::get_singleton() != nullptr) {
		path = ProjectSettings::get_singleton()->globalize_path(path);
	}
	const CharString utf8_path = path.utf8();
	b3Recording *loaded = b3LoadRecordingFromFile(utf8_path.get_data());
	ERR_FAIL_NULL_V_MSG(loaded, false, "Box3D: failed to load recording file.");
	const uint8_t *data = b3Recording_GetData(loaded);
	const int size = b3Recording_GetSize(loaded);
	const bool valid = data != nullptr && size > 0 && b3ValidateReplay(data, size, MAX(1, p_worker_count));
	b3DestroyRecording(loaded);
	return valid;
}

String Box3DPhysics::get_material_name_hint() const {
	return String(",").join(get_material_names());
}

Ref<Box3DSurfaceMaterialLibrary> Box3DPhysics::get_surface_material_library() const {
	_ensure_surface_material_library();
	return library;
}

void Box3DSurfaceOverride3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_material", "material"), &Box3DSurfaceOverride3D::set_material);
	ClassDB::bind_method(D_METHOD("get_material"), &Box3DSurfaceOverride3D::get_material);
	ClassDB::bind_method(D_METHOD("set_surface_map", "surface_map"), &Box3DSurfaceOverride3D::set_surface_map);
	ClassDB::bind_method(D_METHOD("get_surface_map"), &Box3DSurfaceOverride3D::get_surface_map);
	ClassDB::bind_method(D_METHOD("set_shape_owner", "shape_owner"), &Box3DSurfaceOverride3D::set_shape_owner);
	ClassDB::bind_method(D_METHOD("get_shape_owner"), &Box3DSurfaceOverride3D::get_shape_owner);
	ClassDB::bind_method(D_METHOD("apply"), &Box3DSurfaceOverride3D::apply);

	ADD_PROPERTY(PropertyInfo(Variant::STRING_NAME, "material", PROPERTY_HINT_ENUM, ""), "set_material", "get_material");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "surface_map", PROPERTY_HINT_RESOURCE_TYPE, "Box3DSurfaceMap"), "set_surface_map", "get_surface_map");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "shape_owner"), "set_shape_owner", "get_shape_owner");
}

void Box3DSurfaceOverride3D::_validate_property(PropertyInfo &p_property) const {
	if (p_property.name == StringName("material")) {
		Box3DPhysics *box3d_physics = Box3DPhysics::get_singleton();
		if (box3d_physics != nullptr) {
			p_property.hint = PROPERTY_HINT_ENUM;
			p_property.hint_string = box3d_physics->get_material_name_hint();
		}
	}
}

void Box3DSurfaceOverride3D::_notification(int p_what) {
	if (p_what == NOTIFICATION_ENTER_TREE) {
		callable_mp(this, &Box3DSurfaceOverride3D::apply).call_deferred();
	}
}

void Box3DSurfaceOverride3D::set_material(const StringName &p_material) {
	material = p_material;
	if (is_inside_tree()) {
		apply();
	}
}

void Box3DSurfaceOverride3D::set_surface_map(const Ref<Box3DSurfaceMap> &p_surface_map) {
	surface_map = p_surface_map;
	if (is_inside_tree()) {
		apply();
	}
}

void Box3DSurfaceOverride3D::set_shape_owner(int p_shape_owner) {
	shape_owner = p_shape_owner;
	if (is_inside_tree()) {
		apply();
	}
}

void Box3DSurfaceOverride3D::apply() {
	Box3DPhysics *box3d_physics = Box3DPhysics::get_singleton();
	ERR_FAIL_NULL(box3d_physics);

	CollisionObject3D *parent_body = Object::cast_to<CollisionObject3D>(get_parent());
	ERR_FAIL_NULL_MSG(parent_body, "Box3DSurfaceOverride3D must be a child of a CollisionObject3D.");

	const int material_id = box3d_physics->get_material_id(material);
	if (material != StringName() && material_id == 0) {
		WARN_PRINT(vformat("Box3D: unknown surface material '%s' on Box3DSurfaceOverride3D.", String(material)));
	}
	box3d_physics->body_set_surface_material(parent_body->get_rid(), shape_owner, material_id);

	if (surface_map.is_valid()) {
		const PackedInt64Array material_ids = _resolve_surface_map_material_ids(box3d_physics, surface_map);
		const PackedByteArray triangle_indices = surface_map->get_triangle_indices();
		if (shape_owner == -1) {
			List<uint32_t> owners;
			parent_body->get_shape_owners(&owners);
			for (const uint32_t &owner : owners) {
				_apply_surface_map_to_owner(box3d_physics, parent_body, owner, shape_owner, material_ids, triangle_indices);
			}
		} else {
			const uint32_t owner = parent_body->shape_find_owner(shape_owner);
			ERR_FAIL_COND(owner == UINT32_MAX);
			_apply_surface_map_to_owner(box3d_physics, parent_body, owner, shape_owner, material_ids, triangle_indices);
		}
	}
}
