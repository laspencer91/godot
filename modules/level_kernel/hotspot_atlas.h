/**************************************************************************/
/*  hotspot_atlas.h                                                       */
/**************************************************************************/
/*  G-Level LE3: normalized hotspot-atlas data and Source .rect I/O.      */
/**************************************************************************/

#pragma once

#include "core/io/resource.h"
#include "core/math/rect2.h"
#include "core/templates/rid.h"
#include "core/variant/typed_array.h"
#include "scene/resources/texture.h"

class HotspotPatch : public Resource {
	GDCLASS(HotspotPatch, Resource);

	Rect2 rect_uv;
	bool allow_rotation = false;
	bool allow_mirror_x = false;
	bool allow_mirror_y = false;
	bool allow_tiling = false;
	int tiling_axis = 0;
	float inset_px = 0.0f;
	StringName patch_name;
	Dictionary extra;

	// Derived against the owning atlas' reference texture. These values are
	// deliberately omitted from storage.
	float aspect = 0.0f;
	float area_texels = 0.0f;

	void _update_derived(const Size2i &p_texture_size);

	friend class HotspotAtlas;

protected:
	static void _bind_methods();

public:
	enum TilingAxis {
		TILING_AXIS_U = 0,
		TILING_AXIS_V = 1,
	};

	void set_rect_uv(const Rect2 &p_rect_uv);
	Rect2 get_rect_uv() const { return rect_uv; }

	void set_allow_rotation(bool p_allow) { allow_rotation = p_allow; emit_changed(); }
	bool is_rotation_allowed() const { return allow_rotation; }
	void set_allow_mirror_x(bool p_allow) { allow_mirror_x = p_allow; emit_changed(); }
	bool is_mirror_x_allowed() const { return allow_mirror_x; }
	void set_allow_mirror_y(bool p_allow) { allow_mirror_y = p_allow; emit_changed(); }
	bool is_mirror_y_allowed() const { return allow_mirror_y; }
	void set_allow_tiling(bool p_allow) { allow_tiling = p_allow; emit_changed(); }
	bool is_tiling_allowed() const { return allow_tiling; }

	void set_tiling_axis(int p_axis);
	int get_tiling_axis() const { return tiling_axis; }
	void set_inset_px(float p_inset_px);
	float get_inset_px() const { return inset_px; }
	void set_patch_name(const StringName &p_name) { patch_name = p_name; emit_changed(); }
	StringName get_patch_name() const { return patch_name; }
	void set_extra(const Dictionary &p_extra) { extra = p_extra; emit_changed(); }
	Dictionary get_extra() const { return extra; }

	float get_aspect() const { return aspect; }
	float get_area_texels() const { return area_texels; }
};

class HotspotAtlas : public Resource {
	GDCLASS(HotspotAtlas, Resource);

public:
	enum MappingMode {
		MAPPING_AUTOMATIC = 0,
		MAPPING_SQUARE = 1,
		MAPPING_CONFORMING = 2,
		MAPPING_FOLLOW_ACTIVE_QUADS = 3,
	};

	enum TilingPolicy {
		TILING_NO = 0,
		TILING_ALLOW = 1,
		TILING_ONLY = 2,
	};

private:
	StringName atlas_id;
	Ref<Texture2D> reference_texture;
	float texel_density_target = 256.0f;
	TypedArray<HotspotPatch> patches;
	int default_mapping_mode = MAPPING_AUTOMATIC;
	bool disallow_random = false;
	int tiling_policy = TILING_ALLOW;
	PackedStringArray param_names;
	TypedArray<StringName> target_materials;

	// The identity key is the resource path when one exists and the texture RID
	// otherwise. Dimensions participate so a changed imported/procedural texture
	// refreshes the derived cache without ever consulting a resource name.
	String derived_texture_path;
	RID derived_texture_rid;
	Size2i derived_texture_size;
	bool patch_update_in_progress = false;
	String last_rect_error;

	void _disconnect_reference_texture();
	void _connect_reference_texture();
	void _disconnect_patch_signals();
	void _connect_patch_signals();
	void _reference_texture_changed();
	void _patch_changed();
	void _recompute_patch_metrics(bool p_force = false);
	void _synthesize_patch_names();

protected:
	static void _bind_methods();

public:
	void set_atlas_id(const StringName &p_atlas_id) { atlas_id = p_atlas_id; emit_changed(); }
	StringName get_atlas_id() const { return atlas_id; }

	void set_reference_texture(const Ref<Texture2D> &p_texture);
	Ref<Texture2D> get_reference_texture() const { return reference_texture; }
	Size2i get_reference_texture_size() const { return derived_texture_size; }

	void set_texel_density_target(float p_target);
	float get_texel_density_target() const { return texel_density_target; }

	void set_patches(const TypedArray<HotspotPatch> &p_patches);
	TypedArray<HotspotPatch> get_patches() const { return patches; }
	Rect2 get_patch_rect_px(int p_patch_index) const;
	void refresh_derived_metrics();

	void set_default_mapping_mode(int p_mode);
	int get_default_mapping_mode() const { return default_mapping_mode; }
	void set_disallow_random(bool p_disallow) { disallow_random = p_disallow; emit_changed(); }
	bool is_random_disallowed() const { return disallow_random; }
	void set_tiling_policy(int p_policy);
	int get_tiling_policy() const { return tiling_policy; }
	void set_param_names(const PackedStringArray &p_names) { param_names = p_names; emit_changed(); }
	PackedStringArray get_param_names() const { return param_names; }
	void set_target_materials(const TypedArray<StringName> &p_paths) { target_materials = p_paths; emit_changed(); }
	TypedArray<StringName> get_target_materials() const { return target_materials; }

	Error import_rect(const String &p_path);
	Error import_rect_file(const String &p_path) { return import_rect(p_path); }
	Error export_rect(const String &p_path);
	Error export_rect_file(const String &p_path) { return export_rect(p_path); }
	String get_last_rect_error() const { return last_rect_error; }

	HotspotAtlas();
	~HotspotAtlas();
};

VARIANT_ENUM_CAST(HotspotPatch::TilingAxis);
VARIANT_ENUM_CAST(HotspotAtlas::MappingMode);
VARIANT_ENUM_CAST(HotspotAtlas::TilingPolicy);
