/**************************************************************************/
/*  hotspot_atlas.cpp                                                     */
/**************************************************************************/
/*  G-Level LE3: normalized hotspot-atlas data resource.                  */
/**************************************************************************/

#include "hotspot_atlas.h"

#include "core/core_string_names.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"

namespace {

bool _rect_is_finite(const Rect2 &p_rect) {
	return Math::is_finite(p_rect.position.x) && Math::is_finite(p_rect.position.y) &&
			Math::is_finite(p_rect.size.x) && Math::is_finite(p_rect.size.y);
}

bool _rect_is_normalized(const Rect2 &p_rect) {
	const Vector2 end = p_rect.position + p_rect.size;
	return _rect_is_finite(p_rect) && p_rect.position.x >= 0.0f && p_rect.position.y >= 0.0f &&
			p_rect.size.x >= 0.0f && p_rect.size.y >= 0.0f && end.x <= 1.0f && end.y <= 1.0f;
}

} // namespace

void HotspotPatch::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_rect_uv", "rect_uv"), &HotspotPatch::set_rect_uv);
	ClassDB::bind_method(D_METHOD("get_rect_uv"), &HotspotPatch::get_rect_uv);
	ClassDB::bind_method(D_METHOD("set_allow_rotation", "allow"), &HotspotPatch::set_allow_rotation);
	ClassDB::bind_method(D_METHOD("is_rotation_allowed"), &HotspotPatch::is_rotation_allowed);
	ClassDB::bind_method(D_METHOD("set_allow_mirror_x", "allow"), &HotspotPatch::set_allow_mirror_x);
	ClassDB::bind_method(D_METHOD("is_mirror_x_allowed"), &HotspotPatch::is_mirror_x_allowed);
	ClassDB::bind_method(D_METHOD("set_allow_mirror_y", "allow"), &HotspotPatch::set_allow_mirror_y);
	ClassDB::bind_method(D_METHOD("is_mirror_y_allowed"), &HotspotPatch::is_mirror_y_allowed);
	ClassDB::bind_method(D_METHOD("set_allow_tiling", "allow"), &HotspotPatch::set_allow_tiling);
	ClassDB::bind_method(D_METHOD("is_tiling_allowed"), &HotspotPatch::is_tiling_allowed);
	ClassDB::bind_method(D_METHOD("set_tiling_axis", "axis"), &HotspotPatch::set_tiling_axis);
	ClassDB::bind_method(D_METHOD("get_tiling_axis"), &HotspotPatch::get_tiling_axis);
	ClassDB::bind_method(D_METHOD("set_inset_px", "inset_px"), &HotspotPatch::set_inset_px);
	ClassDB::bind_method(D_METHOD("get_inset_px"), &HotspotPatch::get_inset_px);
	ClassDB::bind_method(D_METHOD("set_patch_name", "patch_name"), &HotspotPatch::set_patch_name);
	ClassDB::bind_method(D_METHOD("get_patch_name"), &HotspotPatch::get_patch_name);
	ClassDB::bind_method(D_METHOD("set_extra", "extra"), &HotspotPatch::set_extra);
	ClassDB::bind_method(D_METHOD("get_extra"), &HotspotPatch::get_extra);
	ClassDB::bind_method(D_METHOD("get_aspect"), &HotspotPatch::get_aspect);
	ClassDB::bind_method(D_METHOD("get_area_texels"), &HotspotPatch::get_area_texels);

	ADD_PROPERTY(PropertyInfo(Variant::RECT2, "rect_uv"), "set_rect_uv", "get_rect_uv");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "allow_rotation"), "set_allow_rotation", "is_rotation_allowed");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "allow_mirror_x"), "set_allow_mirror_x", "is_mirror_x_allowed");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "allow_mirror_y"), "set_allow_mirror_y", "is_mirror_y_allowed");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "allow_tiling"), "set_allow_tiling", "is_tiling_allowed");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "tiling_axis", PROPERTY_HINT_ENUM, "U,V"), "set_tiling_axis", "get_tiling_axis");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "inset_px", PROPERTY_HINT_RANGE, "0,256,0.01,or_greater,suffix:px"), "set_inset_px", "get_inset_px");
	ADD_PROPERTY(PropertyInfo(Variant::STRING_NAME, "patch_name"), "set_patch_name", "get_patch_name");
	ADD_PROPERTY(PropertyInfo(Variant::DICTIONARY, "extra"), "set_extra", "get_extra");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "aspect", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY), "", "get_aspect");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "area_texels", PROPERTY_HINT_NONE, "suffix:px²", PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY), "", "get_area_texels");

	BIND_ENUM_CONSTANT(TILING_AXIS_U);
	BIND_ENUM_CONSTANT(TILING_AXIS_V);
}

void HotspotPatch::set_rect_uv(const Rect2 &p_rect_uv) {
	ERR_FAIL_COND_MSG(!_rect_is_normalized(p_rect_uv), "Hotspot rect_uv must be finite and contained in normalized 0..1 atlas space.");
	if (rect_uv == p_rect_uv) {
		return;
	}
	rect_uv = p_rect_uv;
	emit_changed();
}

void HotspotPatch::set_tiling_axis(int p_axis) {
	ERR_FAIL_INDEX(p_axis, 2);
	if (tiling_axis == p_axis) {
		return;
	}
	tiling_axis = p_axis;
	emit_changed();
}

void HotspotPatch::set_inset_px(float p_inset_px) {
	ERR_FAIL_COND_MSG(!Math::is_finite(p_inset_px) || p_inset_px < 0.0f, "Hotspot inset_px must be finite and non-negative.");
	if (inset_px == p_inset_px) {
		return;
	}
	inset_px = p_inset_px;
	emit_changed();
}

void HotspotPatch::_update_derived(const Size2i &p_texture_size) {
	if (p_texture_size.x <= 0 || p_texture_size.y <= 0) {
		aspect = 0.0f;
		area_texels = 0.0f;
		return;
	}
	const float width_px = rect_uv.size.x * float(p_texture_size.x);
	const float height_px = rect_uv.size.y * float(p_texture_size.y);
	aspect = height_px > 0.0f ? width_px / height_px : 0.0f;
	area_texels = width_px * height_px;
}

void HotspotAtlas::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_atlas_id", "atlas_id"), &HotspotAtlas::set_atlas_id);
	ClassDB::bind_method(D_METHOD("get_atlas_id"), &HotspotAtlas::get_atlas_id);
	ClassDB::bind_method(D_METHOD("set_reference_texture", "reference_texture"), &HotspotAtlas::set_reference_texture);
	ClassDB::bind_method(D_METHOD("get_reference_texture"), &HotspotAtlas::get_reference_texture);
	ClassDB::bind_method(D_METHOD("get_reference_texture_size"), &HotspotAtlas::get_reference_texture_size);
	ClassDB::bind_method(D_METHOD("set_texel_density_target", "target"), &HotspotAtlas::set_texel_density_target);
	ClassDB::bind_method(D_METHOD("get_texel_density_target"), &HotspotAtlas::get_texel_density_target);
	ClassDB::bind_method(D_METHOD("set_patches", "patches"), &HotspotAtlas::set_patches);
	ClassDB::bind_method(D_METHOD("get_patches"), &HotspotAtlas::get_patches);
	ClassDB::bind_method(D_METHOD("get_patch_rect_px", "patch_index"), &HotspotAtlas::get_patch_rect_px);
	ClassDB::bind_method(D_METHOD("refresh_derived_metrics"), &HotspotAtlas::refresh_derived_metrics);
	ClassDB::bind_method(D_METHOD("set_default_mapping_mode", "mode"), &HotspotAtlas::set_default_mapping_mode);
	ClassDB::bind_method(D_METHOD("get_default_mapping_mode"), &HotspotAtlas::get_default_mapping_mode);
	ClassDB::bind_method(D_METHOD("set_disallow_random", "disallow"), &HotspotAtlas::set_disallow_random);
	ClassDB::bind_method(D_METHOD("is_random_disallowed"), &HotspotAtlas::is_random_disallowed);
	ClassDB::bind_method(D_METHOD("set_tiling_policy", "policy"), &HotspotAtlas::set_tiling_policy);
	ClassDB::bind_method(D_METHOD("get_tiling_policy"), &HotspotAtlas::get_tiling_policy);
	ClassDB::bind_method(D_METHOD("set_param_names", "names"), &HotspotAtlas::set_param_names);
	ClassDB::bind_method(D_METHOD("get_param_names"), &HotspotAtlas::get_param_names);
	ClassDB::bind_method(D_METHOD("set_target_materials", "paths"), &HotspotAtlas::set_target_materials);
	ClassDB::bind_method(D_METHOD("get_target_materials"), &HotspotAtlas::get_target_materials);
	ClassDB::bind_method(D_METHOD("import_rect", "path"), &HotspotAtlas::import_rect);
	ClassDB::bind_method(D_METHOD("import_rect_file", "path"), &HotspotAtlas::import_rect_file);
	ClassDB::bind_method(D_METHOD("export_rect", "path"), &HotspotAtlas::export_rect);
	ClassDB::bind_method(D_METHOD("export_rect_file", "path"), &HotspotAtlas::export_rect_file);
	ClassDB::bind_method(D_METHOD("get_last_rect_error"), &HotspotAtlas::get_last_rect_error);

	ADD_PROPERTY(PropertyInfo(Variant::STRING_NAME, "atlas_id"), "set_atlas_id", "get_atlas_id");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "reference_texture", PROPERTY_HINT_RESOURCE_TYPE, "Texture2D"), "set_reference_texture", "get_reference_texture");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "texel_density_target", PROPERTY_HINT_RANGE, "0.001,16384,0.001,or_greater,suffix:px/m"), "set_texel_density_target", "get_texel_density_target");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "patches", PROPERTY_HINT_ARRAY_TYPE, "HotspotPatch"), "set_patches", "get_patches");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "default_mapping_mode", PROPERTY_HINT_ENUM, "Automatic,Square,Conforming,Follow Active Quads"), "set_default_mapping_mode", "get_default_mapping_mode");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "disallow_random"), "set_disallow_random", "is_random_disallowed");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "tiling_policy", PROPERTY_HINT_ENUM, "No,Allow,Only"), "set_tiling_policy", "get_tiling_policy");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_STRING_ARRAY, "param_names"), "set_param_names", "get_param_names");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "target_materials", PROPERTY_HINT_ARRAY_TYPE, "StringName"), "set_target_materials", "get_target_materials");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR2I, "reference_texture_size", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY), "", "get_reference_texture_size");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "last_rect_error", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NONE), "", "get_last_rect_error");

	BIND_ENUM_CONSTANT(MAPPING_AUTOMATIC);
	BIND_ENUM_CONSTANT(MAPPING_SQUARE);
	BIND_ENUM_CONSTANT(MAPPING_CONFORMING);
	BIND_ENUM_CONSTANT(MAPPING_FOLLOW_ACTIVE_QUADS);
	BIND_ENUM_CONSTANT(TILING_NO);
	BIND_ENUM_CONSTANT(TILING_ALLOW);
	BIND_ENUM_CONSTANT(TILING_ONLY);
}

void HotspotAtlas::_disconnect_reference_texture() {
	if (reference_texture.is_valid()) {
		const Callable changed = callable_mp(this, &HotspotAtlas::_reference_texture_changed);
		if (reference_texture->is_connected(CoreStringName(changed), changed)) {
			reference_texture->disconnect(CoreStringName(changed), changed);
		}
	}
}

void HotspotAtlas::_connect_reference_texture() {
	if (reference_texture.is_valid()) {
		const Callable changed = callable_mp(this, &HotspotAtlas::_reference_texture_changed);
		if (!reference_texture->is_connected(CoreStringName(changed), changed)) {
			reference_texture->connect(CoreStringName(changed), changed);
		}
	}
}

void HotspotAtlas::_disconnect_patch_signals() {
	const Callable changed = callable_mp(this, &HotspotAtlas::_patch_changed);
	for (int i = 0; i < patches.size(); i++) {
		Ref<HotspotPatch> patch = patches[i];
		if (patch.is_valid() && patch->is_connected(CoreStringName(changed), changed)) {
			patch->disconnect(CoreStringName(changed), changed);
		}
	}
}

void HotspotAtlas::_connect_patch_signals() {
	const Callable changed = callable_mp(this, &HotspotAtlas::_patch_changed);
	for (int i = 0; i < patches.size(); i++) {
		Ref<HotspotPatch> patch = patches[i];
		if (patch.is_valid() && !patch->is_connected(CoreStringName(changed), changed)) {
			patch->connect(CoreStringName(changed), changed);
		}
	}
}

void HotspotAtlas::_reference_texture_changed() {
	_recompute_patch_metrics();
}

void HotspotAtlas::_patch_changed() {
	if (patch_update_in_progress) {
		return;
	}
	_recompute_patch_metrics(true);
	emit_changed();
}

void HotspotAtlas::_synthesize_patch_names() {
	patch_update_in_progress = true;
	for (int i = 0; i < patches.size(); i++) {
		Ref<HotspotPatch> patch = patches[i];
		if (patch.is_valid() && patch->get_patch_name().is_empty()) {
			patch->set_patch_name(StringName("p" + itos(i)));
		}
	}
	patch_update_in_progress = false;
}

void HotspotAtlas::_recompute_patch_metrics(bool p_force) {
	String texture_path;
	RID texture_rid;
	Size2i texture_size;
	if (reference_texture.is_valid()) {
		texture_path = reference_texture->get_path();
		texture_rid = reference_texture->get_rid();
		texture_size = Size2i(reference_texture->get_width(), reference_texture->get_height());
		if (texture_size.x <= 0 || texture_size.y <= 0) {
			texture_size = Size2i();
		}
	}
	if (!p_force && texture_path == derived_texture_path && texture_rid == derived_texture_rid && texture_size == derived_texture_size) {
		return;
	}
	derived_texture_path = texture_path;
	derived_texture_rid = texture_rid;
	derived_texture_size = texture_size;
	for (int i = 0; i < patches.size(); i++) {
		Ref<HotspotPatch> patch = patches[i];
		if (patch.is_valid()) {
			patch->_update_derived(derived_texture_size);
		}
	}
}

void HotspotAtlas::set_reference_texture(const Ref<Texture2D> &p_texture) {
	if (reference_texture == p_texture) {
		_recompute_patch_metrics(true);
		return;
	}
	_disconnect_reference_texture();
	reference_texture = p_texture;
	_connect_reference_texture();
	_recompute_patch_metrics(true);
	emit_changed();
}

void HotspotAtlas::set_texel_density_target(float p_target) {
	ERR_FAIL_COND_MSG(!Math::is_finite(p_target) || p_target <= 0.0f, "Hotspot texel_density_target must be finite and greater than zero.");
	if (texel_density_target == p_target) {
		return;
	}
	texel_density_target = p_target;
	emit_changed();
}

void HotspotAtlas::set_patches(const TypedArray<HotspotPatch> &p_patches) {
	_disconnect_patch_signals();
	patches = p_patches;
	_synthesize_patch_names();
	_connect_patch_signals();
	_recompute_patch_metrics(true);
	emit_changed();
}

Rect2 HotspotAtlas::get_patch_rect_px(int p_patch_index) const {
	ERR_FAIL_INDEX_V(p_patch_index, patches.size(), Rect2());
	Ref<HotspotPatch> patch = patches[p_patch_index];
	ERR_FAIL_COND_V(patch.is_null(), Rect2());
	const Vector2 texture_size(derived_texture_size);
	return Rect2(patch->get_rect_uv().position * texture_size, patch->get_rect_uv().size * texture_size);
}

void HotspotAtlas::refresh_derived_metrics() {
	_recompute_patch_metrics(true);
}

void HotspotAtlas::set_default_mapping_mode(int p_mode) {
	ERR_FAIL_INDEX(p_mode, 4);
	if (default_mapping_mode == p_mode) {
		return;
	}
	default_mapping_mode = p_mode;
	emit_changed();
}

void HotspotAtlas::set_tiling_policy(int p_policy) {
	ERR_FAIL_INDEX(p_policy, 3);
	if (tiling_policy == p_policy) {
		return;
	}
	tiling_policy = p_policy;
	emit_changed();
}

HotspotAtlas::HotspotAtlas() {
	param_names.push_back("albedo_texture");
	param_names.push_back("BaseColor");
	param_names.push_back("base_color_texture");
	param_names.push_back("texture_albedo");
}

HotspotAtlas::~HotspotAtlas() {
	_disconnect_patch_signals();
	_disconnect_reference_texture();
}
