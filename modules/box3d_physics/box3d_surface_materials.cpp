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
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "materials", PROPERTY_HINT_ARRAY_TYPE, "Box3DSurfaceMaterial"), "set_materials", "get_materials");
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
	ClassDB::bind_method(D_METHOD("match_texture", "texture_name"), &Box3DPhysics::match_texture);
	ClassDB::bind_method(D_METHOD("shape_set_surface_material", "shape", "material_id"), &Box3DPhysics::shape_set_surface_material);
	ClassDB::bind_method(D_METHOD("shape_set_surface_map", "shape", "material_ids", "triangle_indices"), &Box3DPhysics::shape_set_surface_map);
	ClassDB::bind_method(D_METHOD("body_set_surface_material", "body", "shape_idx", "material_id"), &Box3DPhysics::body_set_surface_material);
	ClassDB::bind_method(D_METHOD("get_shape_mesh_material_indices", "shape"), &Box3DPhysics::get_shape_mesh_material_indices);
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
	GLOBAL_DEF(PropertyInfo(Variant::STRING, "physics/box3d/surface_material_library", PROPERTY_HINT_FILE, "*.tres,*.res"), String());
}

void Box3DPhysics::reload_surface_material_library() {
	library.unref();
	materials_by_name.clear();
	materials_by_id.clear();

	const String path = GLOBAL_GET("physics/box3d/surface_material_library");
	if (!path.is_empty()) {
		library = ResourceLoader::load(path);
		ERR_FAIL_COND_MSG(library.is_null(), "Box3D: surface material library setting does not point to a Box3DSurfaceMaterialLibrary resource.");
	}

	if (library.is_null()) {
		return;
	}

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
			WARN_PRINT(vformat("Box3D: duplicate surface material name/id ignored: %s / %d.", name, id));
			continue;
		}
		seen_names.insert(name);
		seen_ids.insert(id);
		materials_by_name[name] = material;
		materials_by_id[id] = material;
	}
}

int Box3DPhysics::get_material_id(const StringName &p_name) const {
	const Ref<Box3DSurfaceMaterial> *material = materials_by_name.getptr(p_name);
	return material && material->is_valid() ? (*material)->get_material_id() : 0;
}

StringName Box3DPhysics::get_material_name(int p_id) const {
	const Ref<Box3DSurfaceMaterial> *material = materials_by_id.getptr(p_id);
	return material && material->is_valid() ? (*material)->get_material_name() : StringName();
}

Ref<Box3DSurfaceMaterial> Box3DPhysics::get_material(const Variant &p_id_or_name) const {
	if (p_id_or_name.get_type() == Variant::INT) {
		const Ref<Box3DSurfaceMaterial> *material = materials_by_id.getptr((int)p_id_or_name);
		return material ? *material : Ref<Box3DSurfaceMaterial>();
	}
	const Ref<Box3DSurfaceMaterial> *material = materials_by_name.getptr((StringName)p_id_or_name);
	return material ? *material : Ref<Box3DSurfaceMaterial>();
}

int Box3DPhysics::match_texture(const String &p_texture_name) const {
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

void Box3DSurfaceOverride3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_material", "material"), &Box3DSurfaceOverride3D::set_material);
	ClassDB::bind_method(D_METHOD("get_material"), &Box3DSurfaceOverride3D::get_material);
	ClassDB::bind_method(D_METHOD("set_shape_owner", "shape_owner"), &Box3DSurfaceOverride3D::set_shape_owner);
	ClassDB::bind_method(D_METHOD("get_shape_owner"), &Box3DSurfaceOverride3D::get_shape_owner);
	ClassDB::bind_method(D_METHOD("apply"), &Box3DSurfaceOverride3D::apply);

	ADD_PROPERTY(PropertyInfo(Variant::STRING_NAME, "material"), "set_material", "get_material");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "shape_owner"), "set_shape_owner", "get_shape_owner");
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

	box3d_physics->body_set_surface_material(parent_body->get_rid(), shape_owner, box3d_physics->get_material_id(material));
}
