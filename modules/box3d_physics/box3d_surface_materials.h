/**************************************************************************/
/*  box3d_surface_materials.h                                             */
/**************************************************************************/

#pragma once

#include "core/object/ref_counted.h"
#include "core/templates/hash_map.h"
#include "core/variant/typed_array.h"
#include "scene/main/node.h"

#include "box3d/box3d.h"

class Box3DSurfaceMaterial : public Resource {
	GDCLASS(Box3DSurfaceMaterial, Resource);

	StringName material_name;
	int material_id = 0;
	float friction = 0.6f;
	float restitution = 0.0f;
	float rolling_resistance = 0.0f;
	Vector3 tangent_velocity;
	Color debug_color;
	Dictionary custom;
	PackedStringArray texture_patterns;

protected:
	static void _bind_methods();

public:
	void set_material_name(const StringName &p_name) { material_name = p_name; }
	StringName get_material_name() const { return material_name; }
	void set_material_id(int p_id) { material_id = MAX(0, p_id); }
	int get_material_id() const { return material_id; }
	void set_friction(float p_friction) { friction = p_friction; }
	float get_friction() const { return friction; }
	void set_restitution(float p_restitution) { restitution = p_restitution; }
	float get_restitution() const { return restitution; }
	void set_rolling_resistance(float p_rolling_resistance) { rolling_resistance = p_rolling_resistance; }
	float get_rolling_resistance() const { return rolling_resistance; }
	void set_tangent_velocity(const Vector3 &p_velocity) { tangent_velocity = p_velocity; }
	Vector3 get_tangent_velocity() const { return tangent_velocity; }
	void set_debug_color(const Color &p_color) { debug_color = p_color; }
	Color get_debug_color() const { return debug_color; }
	void set_custom(const Dictionary &p_custom) { custom = p_custom; }
	Dictionary get_custom() const { return custom; }
	void set_texture_patterns(const PackedStringArray &p_patterns) { texture_patterns = p_patterns; }
	PackedStringArray get_texture_patterns() const { return texture_patterns; }

	b3SurfaceMaterial to_box3d() const;
};

class Box3DSurfaceMaterialLibrary : public Resource {
	GDCLASS(Box3DSurfaceMaterialLibrary, Resource);

	TypedArray<Box3DSurfaceMaterial> materials;

	// Shared by the authoring helpers and by set_materials(), which has to uniquify against
	// the array being ingested rather than against the already-assigned member.
	static StringName _unique_name(const TypedArray<Box3DSurfaceMaterial> &p_materials, const StringName &p_base, int p_ignore_index);
	static int _next_material_id(const TypedArray<Box3DSurfaceMaterial> &p_materials);

protected:
	static void _bind_methods();

public:
	void set_materials(const TypedArray<Box3DSurfaceMaterial> &p_materials);
	TypedArray<Box3DSurfaceMaterial> get_materials() const { return materials; }

	// Authoring helpers. The Physics tab in Project Settings owns this resource, so
	// name/id bookkeeping lives here instead of being re-derived by every caller.
	int find_material_index(const StringName &p_name) const;
	Ref<Box3DSurfaceMaterial> find_material(const StringName &p_name) const;
	int allocate_material_id() const;
	StringName make_unique_name(const StringName &p_base) const;
};

class Box3DSurfaceMap : public Resource {
	GDCLASS(Box3DSurfaceMap, Resource);

	PackedStringArray material_names;
	PackedByteArray triangle_indices;

protected:
	static void _bind_methods();

public:
	void set_material_names(const PackedStringArray &p_names) { material_names = p_names; }
	PackedStringArray get_material_names() const { return material_names; }
	void set_triangle_indices(const PackedByteArray &p_indices) { triangle_indices = p_indices; }
	PackedByteArray get_triangle_indices() const { return triangle_indices; }
};

class Box3DPhysics : public Object {
	GDCLASS(Box3DPhysics, Object);

	inline static Box3DPhysics *singleton = nullptr;

	Ref<Box3DSurfaceMaterialLibrary> library;
	HashMap<StringName, Ref<Box3DSurfaceMaterial>> materials_by_name;
	HashMap<int, Ref<Box3DSurfaceMaterial>> materials_by_id;
	mutable bool library_resolved = false;

	void _ensure_surface_material_library() const;

protected:
	static void _bind_methods();

public:
	Box3DPhysics();
	~Box3DPhysics();

	static Box3DPhysics *get_singleton() { return singleton; }
	static void register_project_settings();
	static String get_surface_material_library_path();

	void reload_surface_material_library();
	Ref<Box3DSurfaceMaterialLibrary> get_surface_material_library() const;
	int get_material_id(const StringName &p_name) const;
	StringName get_material_name(int p_id) const;
	Ref<Box3DSurfaceMaterial> get_material(const Variant &p_id_or_name) const;
	bool has_material(const StringName &p_name) const;
	PackedStringArray get_material_names() const;
	TypedArray<Box3DSurfaceMaterial> get_materials() const;
	int match_texture(const String &p_texture_name) const;
	b3SurfaceMaterial get_box3d_material(int p_id) const;

	void shape_set_surface_material(RID p_shape, int p_material_id);
	void shape_set_surface_map(RID p_shape, const PackedInt64Array &p_material_ids, const PackedByteArray &p_triangle_indices);
	void body_set_surface_material(RID p_body, int p_shape_idx, int p_material_id);
	PackedByteArray get_shape_mesh_material_indices(RID p_shape) const;
	Ref<Box3DSurfaceMaterial> get_face_material(RID p_shape, int p_face_index) const;
	String get_material_name_hint() const;

	void joint_set_box3d_param(RID p_joint, int p_param, real_t p_value);
	real_t joint_get_box3d_param(RID p_joint, int p_param) const;
	void joint_set_box3d_target_rotation(RID p_joint, const Quaternion &p_target_rotation);
	Quaternion joint_get_box3d_target_rotation(RID p_joint) const;
	void joint_set_box3d_motor_velocity(RID p_joint, const Vector3 &p_velocity);
	Vector3 joint_get_box3d_motor_velocity(RID p_joint) const;

	bool recording_start(RID p_space, int p_byte_capacity = 0);
	PackedByteArray recording_stop(RID p_space);
	bool recording_is_active(RID p_space) const;
	int recording_get_size(RID p_space) const;
	bool recording_save(RID p_space, const String &p_path) const;
	bool recording_validate(const PackedByteArray &p_data, int p_worker_count = 1) const;
	bool recording_validate_file(const String &p_path, int p_worker_count = 1) const;
};

class Box3DSurfaceOverride3D : public Node {
	GDCLASS(Box3DSurfaceOverride3D, Node);

	StringName material;
	Ref<Box3DSurfaceMap> surface_map;
	int shape_owner = -1;

protected:
	static void _bind_methods();
	void _validate_property(PropertyInfo &p_property) const;
	void _notification(int p_what);

public:
	void set_material(const StringName &p_material);
	StringName get_material() const { return material; }
	void set_surface_map(const Ref<Box3DSurfaceMap> &p_surface_map);
	Ref<Box3DSurfaceMap> get_surface_map() const { return surface_map; }
	void set_shape_owner(int p_shape_owner);
	int get_shape_owner() const { return shape_owner; }
	void apply();
};
