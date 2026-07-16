/**************************************************************************/
/*  level_editor.cpp                                                      */
/**************************************************************************/
/*  G-Level LE0: SERVICE state for the level-editor workspace seam.       */
/**************************************************************************/

#include "level_editor.h"

#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/resource_loader.h"
#include "core/io/resource_saver.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "editor/editor_document.h"
#include "editor/editor_node.h"
#include "editor/level/blockout_material_registry.h"
#include "editor/level/hotspot_patch_editor.h"
#include "editor/level/level_editor_view.h"
#include "editor/level/material_browser_dock.h"
#include "editor/level/material_index.h"
#include "editor/level/material_preview_generator.h"
#include "editor/level/texel_density_scanner.h"
#include "editor/scene/3d/node_3d_editor_plugin.h"
#include "editor/settings/editor_settings.h"
#include "scene/main/scene_tree.h"

#include "modules/level_kernel/level_block.h"
#include "modules/level_kernel/hotspot_binding.h"
#include "modules/level_kernel/hotspot_atlas.h"
#include "modules/level_kernel/level_mesh_baker.h"
#include "modules/level_kernel/level_mesh_data.h"

LevelEditor *LevelEditor::singleton = nullptr;

real_t LevelEditor::snap_step_or_default() {
	return singleton ? singleton->get_snap_step() : DEFAULT_SNAP_STEP;
}

void LevelEditor::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_tool_mode"), &LevelEditor::get_tool_mode);
	ClassDB::bind_method(D_METHOD("set_tool_mode", "mode"), static_cast<void (LevelEditor::*)(ToolMode)>(&LevelEditor::set_tool_mode));
	ClassDB::bind_method(D_METHOD("get_snap_step"), &LevelEditor::get_snap_step);
	ClassDB::bind_method(D_METHOD("set_snap_step", "step"), &LevelEditor::set_snap_step);
	ClassDB::bind_method(D_METHOD("is_snap_enabled"), &LevelEditor::is_snap_enabled);
	ClassDB::bind_method(D_METHOD("set_snap_enabled", "enabled"), &LevelEditor::set_snap_enabled);
	ClassDB::bind_method(D_METHOD("get_default_block_height"), &LevelEditor::get_default_block_height);
	ClassDB::bind_method(D_METHOD("set_active_material", "material", "source_path"), static_cast<void (LevelEditor::*)(const Ref<Material> &, const String &)>(&LevelEditor::set_active_material), DEFVAL(String()));
	ClassDB::bind_method(D_METHOD("get_active_material"), static_cast<Ref<Material> (LevelEditor::*)() const>(&LevelEditor::get_active_material));
	ClassDB::bind_method(D_METHOD("get_active_material_path"), static_cast<String (LevelEditor::*)() const>(&LevelEditor::get_active_material_path));
	ClassDB::bind_method(D_METHOD("get_active_material_binding_path"), static_cast<String (LevelEditor::*)() const>(&LevelEditor::get_active_material_binding_path));
	ClassDB::bind_method(D_METHOD("get_active_material_display_name"), static_cast<String (LevelEditor::*)() const>(&LevelEditor::get_active_material_display_name));
	ClassDB::bind_method(D_METHOD("set_captured_mapping", "mapping"), static_cast<void (LevelEditor::*)(const Dictionary &)>(&LevelEditor::set_captured_mapping));
	ClassDB::bind_method(D_METHOD("get_captured_mapping"), static_cast<Dictionary (LevelEditor::*)() const>(&LevelEditor::get_captured_mapping));
	ClassDB::bind_method(D_METHOD("clear_captured_mapping"), static_cast<void (LevelEditor::*)()>(&LevelEditor::clear_captured_mapping));
	ClassDB::bind_method(D_METHOD("activate_blockout_slot", "slot"), static_cast<bool (LevelEditor::*)(int)>(&LevelEditor::activate_blockout_slot));
	ClassDB::bind_method(D_METHOD("modify_selected_texture", "operation", "value"), &LevelEditor::modify_selected_texture, DEFVAL(Vector2(1, 1)));
	ClassDB::bind_method(D_METHOD("get_material_index"), &LevelEditor::get_material_index);
	ClassDB::bind_method(D_METHOD("get_texel_density_scanner"), &LevelEditor::get_texel_density_scanner);
	ClassDB::bind_method(D_METHOD("get_hotspot_bindings"), &LevelEditor::get_hotspot_bindings);
	ClassDB::bind_method(D_METHOD("set_hotspot_mapping_mode_override", "mode"), static_cast<void (LevelEditor::*)(int)>(&LevelEditor::set_hotspot_mapping_mode_override));
	ClassDB::bind_method(D_METHOD("get_hotspot_mapping_mode_override"), static_cast<int (LevelEditor::*)() const>(&LevelEditor::get_hotspot_mapping_mode_override));
	ClassDB::bind_method(D_METHOD("get_hotspot_bindings_path"), &LevelEditor::get_hotspot_bindings_path);
	ClassDB::bind_method(D_METHOD("get_hotspot_pattern_key", "material_path"), &LevelEditor::get_hotspot_pattern_key);
	ClassDB::bind_method(D_METHOD("resolve_hotspot_atlas", "material_path"), &LevelEditor::resolve_hotspot_atlas);
	ClassDB::bind_method(D_METHOD("bind_hotspot_atlas", "material_path", "atlas_path"), &LevelEditor::bind_hotspot_atlas);
	ClassDB::bind_method(D_METHOD("set_hotspot_pattern_binding", "pattern_key", "atlas_path"), &LevelEditor::set_hotspot_pattern_binding);
	ClassDB::bind_method(D_METHOD("erase_hotspot_pattern_binding", "pattern_key"), &LevelEditor::erase_hotspot_pattern_binding);
	ClassDB::bind_method(D_METHOD("save_hotspot_bindings"), &LevelEditor::save_hotspot_bindings);
	ClassDB::bind_method(D_METHOD("reload_hotspot_bindings"), &LevelEditor::reload_hotspot_bindings);
	ClassDB::bind_method(D_METHOD("get_blockout_material_registry"), &LevelEditor::get_blockout_material_registry);
	ClassDB::bind_method(D_METHOD("get_material_browser_dock"), &LevelEditor::get_material_browser_dock);
	ClassDB::bind_method(D_METHOD("get_hotspot_patch_editor_count"), &LevelEditor::get_hotspot_patch_editor_count);
	ClassDB::bind_method(D_METHOD("get_hotspot_patch_editor_state"), &LevelEditor::get_hotspot_patch_editor_state);
	ClassDB::bind_method(D_METHOD("hotspot_editor_create_patch_px", "rect_px"), &LevelEditor::hotspot_editor_create_patch_px);
	ClassDB::bind_method(D_METHOD("hotspot_editor_set_patch_rect_px", "patch_index", "rect_px"), &LevelEditor::hotspot_editor_set_patch_rect_px);
	ClassDB::bind_method(D_METHOD("hotspot_editor_import_rect", "path"), &LevelEditor::hotspot_editor_import_rect);
	ClassDB::bind_method(D_METHOD("hotspot_editor_export_rect", "path"), &LevelEditor::hotspot_editor_export_rect);
	ClassDB::bind_method(D_METHOD("hotspot_editor_add_binding", "pattern_key"), &LevelEditor::hotspot_editor_add_binding);
	ClassDB::bind_method(D_METHOD("hotspot_editor_remove_binding", "pattern_key"), &LevelEditor::hotspot_editor_remove_binding);
	ClassDB::bind_method(D_METHOD("hotspot_editor_set_preview_enabled", "enabled"), &LevelEditor::hotspot_editor_set_preview_enabled);
	ClassDB::bind_method(D_METHOD("hotspot_editor_set_debug_enabled", "enabled"), &LevelEditor::hotspot_editor_set_debug_enabled);
	ClassDB::bind_method(D_METHOD("hotspot_editor_save"), &LevelEditor::hotspot_editor_save);

	ADD_PROPERTY(PropertyInfo(Variant::INT, "tool_mode", PROPERTY_HINT_ENUM, "Select,Block"), "set_tool_mode", "get_tool_mode");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "snap_step", PROPERTY_HINT_RANGE, "0.001,1024,0.001,or_greater,suffix:m"), "set_snap_step", "get_snap_step");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "snap_enabled"), "set_snap_enabled", "is_snap_enabled");
	ADD_SIGNAL(MethodInfo("snap_settings_changed",
			PropertyInfo(Variant::FLOAT, "step"),
			PropertyInfo(Variant::BOOL, "enabled")));
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "default_block_height", PROPERTY_HINT_NONE, "suffix:m", PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY), "", "get_default_block_height");

	BIND_ENUM_CONSTANT(TOOL_SELECT);
	BIND_ENUM_CONSTANT(TOOL_BLOCK);

	ADD_SIGNAL(MethodInfo("active_material_changed",
			PropertyInfo(Variant::OBJECT, "material", PROPERTY_HINT_RESOURCE_TYPE, Material::get_class_static()),
			PropertyInfo(Variant::STRING, "source_path")));
	ADD_SIGNAL(MethodInfo("active_material_changed_for_document",
			PropertyInfo(Variant::INT, "document_history_id"),
			PropertyInfo(Variant::OBJECT, "material", PROPERTY_HINT_RESOURCE_TYPE, Material::get_class_static()),
			PropertyInfo(Variant::STRING, "source_path")));
}

void LevelEditor::set_active_material(const Ref<Material> &p_material, const String &p_source_path) {
	set_active_material(_get_active_level_document(), p_material, p_source_path);
}

void LevelEditor::set_active_material(LevelDocument *p_document, const Ref<Material> &p_material, const String &p_source_path) {
	if (!p_document) {
		return;
	}
	String source_path = p_source_path;
	if (source_path.is_empty() && p_material.is_valid() && p_material->get_path().begins_with("res://")) {
		source_path = p_material->get_path();
	}
	if (p_document->active_material == p_material && p_document->active_material_path == source_path) {
		p_document->active_material_binding_path = source_path;
		return;
	}
	p_document->active_material = p_material;
	p_document->active_material_path = source_path;
	p_document->active_material_binding_path = source_path;
	emit_signal(SNAME("active_material_changed_for_document"), p_document->get_history_id(), p_document->active_material, p_document->active_material_path);
	// Compatibility signal: existing scripts continue to observe the same
	// material/path payload. Document views use the qualified signal above.
	emit_signal(SNAME("active_material_changed"), p_document->active_material, p_document->active_material_path);
}

Ref<Material> LevelEditor::get_active_material() const {
	return get_active_material(_get_active_level_document());
}

Ref<Material> LevelEditor::get_active_material(const LevelDocument *p_document) const {
	return p_document ? p_document->active_material : Ref<Material>();
}

String LevelEditor::get_active_material_path() const {
	return get_active_material_path(_get_active_level_document());
}

String LevelEditor::get_active_material_path(const LevelDocument *p_document) const {
	return p_document ? p_document->active_material_path : String();
}

String LevelEditor::get_active_material_binding_path() const {
	return get_active_material_binding_path(_get_active_level_document());
}

String LevelEditor::get_active_material_binding_path(const LevelDocument *p_document) const {
	return p_document ? p_document->active_material_binding_path : String();
}

void LevelEditor::set_active_material_binding_path(const String &p_path) {
	set_active_material_binding_path(_get_active_level_document(), p_path);
}

void LevelEditor::set_active_material_binding_path(LevelDocument *p_document, const String &p_path) {
	if (p_document) {
		p_document->active_material_binding_path = p_path;
	}
}

void LevelEditor::set_captured_mapping(const Dictionary &p_mapping) {
	set_captured_mapping(_get_active_level_document(), p_mapping);
}

void LevelEditor::set_captured_mapping(LevelDocument *p_document, const Dictionary &p_mapping) {
	if (p_document) {
		p_document->captured_mapping = p_mapping;
	}
}

Dictionary LevelEditor::get_captured_mapping() const {
	return get_captured_mapping(_get_active_level_document());
}

Dictionary LevelEditor::get_captured_mapping(const LevelDocument *p_document) const {
	return p_document ? p_document->captured_mapping : Dictionary();
}

void LevelEditor::clear_captured_mapping() {
	clear_captured_mapping(_get_active_level_document());
}

void LevelEditor::clear_captured_mapping(LevelDocument *p_document) {
	if (p_document) {
		p_document->captured_mapping.clear();
	}
}

String LevelEditor::get_active_material_display_name() const {
	return get_active_material_display_name(_get_active_level_document());
}

String LevelEditor::get_active_material_display_name(const LevelDocument *p_document) const {
	const String material_path = get_active_material_path(p_document);
	const Ref<Material> material = get_active_material(p_document);
	if (!material_path.is_empty()) {
		return material_path.get_file().get_basename().trim_prefix("M_");
	}
	if (material.is_valid() && !material->get_name().is_empty()) {
		return material->get_name();
	}
	return material.is_valid() ? TTR("Unsaved Material") : TTR("No Active Material");
}

Ref<Texture2D> LevelEditor::get_material_albedo_texture(const Ref<Material> &p_material, const String &p_source_path) const {
	if (texel_density_scanner.is_null()) {
		return Ref<Texture2D>();
	}
	const std::optional<TexelDensityResult> result = texel_density_scanner->scan(p_material, p_source_path);
	return result.has_value() ? result->texture : Ref<Texture2D>();
}

bool LevelEditor::activate_blockout_slot(int p_slot) {
	return activate_blockout_slot(_get_active_level_document(), p_slot);
}

bool LevelEditor::activate_blockout_slot(LevelDocument *p_document, int p_slot) {
	if (!p_document) {
		return false;
	}
	ERR_FAIL_INDEX_V(p_slot, BlockoutMaterialRegistry::SLOT_COUNT, false);
	ERR_FAIL_COND_V(blockout_material_registry.is_null(), false);
	const Ref<Material> material = blockout_material_registry->get_slot(p_slot);
	if (material.is_null()) {
		return false;
	}
	const String path = blockout_material_registry->get_slot_path(p_slot);
	set_active_material(p_document, material, path);
	if (path.is_empty()) {
		p_document->active_material_binding_path = LevelMeshBaker::get_builtin_blockout_material_path(p_slot);
	}
	return true;
}

LevelDocument *LevelEditor::_get_active_level_document() const {
	EditorNode *editor_node = EditorNode::get_singleton();
	EditorDocument *active_document = editor_node ? editor_node->get_editor_data().get_active_document() : nullptr;
	return active_document && active_document->get_type() == EditorDocument::TYPE_LEVEL ? static_cast<LevelDocument *>(active_document) : nullptr;
}

LevelEditorView *LevelEditor::_get_view_for_document(const LevelDocument *p_document) const {
	if (!p_document) {
		return nullptr;
	}
	for (LevelEditorView *view : views) {
		if (view && view->is_context_active() && view->get_level_document() == p_document) {
			return view;
		}
	}
	for (LevelEditorView *view : views) {
		if (view && view->is_visible_in_tree() && view->get_level_document() == p_document) {
			return view;
		}
	}
	for (LevelEditorView *view : views) {
		if (view && view->get_level_document() == p_document) {
			return view;
		}
	}
	return nullptr;
}

LevelEditorView *LevelEditor::_get_active_view() const {
	return _get_view_for_document(_get_active_level_document());
}

HotspotPatchEditor *LevelEditor::_get_active_hotspot_patch_editor() const {
	for (int i = hotspot_patch_editors.size() - 1; i >= 0; i--) {
		HotspotPatchEditor *editor = hotspot_patch_editors[i];
		if (editor && editor->is_visible_in_tree()) {
			return editor;
		}
	}
	return hotspot_patch_editors.is_empty() ? nullptr : hotspot_patch_editors[hotspot_patch_editors.size() - 1];
}

void LevelEditor::_apply_hotspot_preview_request() {
	LevelEditorView *preview_target = nullptr;
	if (hotspot_preview_enabled && hotspot_preview_atlas.is_valid()) {
		// Prefer a concurrently visible split pane, then retain support for a
		// selected level tab hidden behind the atlas tab in the same pane.
		for (LevelEditorView *view : views) {
			if (view && view->is_visible_in_tree() && view->has_hotspot_face_selection()) {
				preview_target = view;
				break;
			}
		}
		if (!preview_target) {
			for (LevelEditorView *view : views) {
				if (view && view->has_hotspot_face_selection()) {
					preview_target = view;
					break;
				}
			}
		}
	}
	for (LevelEditorView *view : views) {
		if (!view) {
			continue;
		}
		view->set_hotspot_fit_debug_enabled(hotspot_debug_enabled);
		if (view == preview_target) {
			view->show_hotspot_preview(hotspot_preview_owner, hotspot_preview_atlas);
		} else {
			view->clear_hotspot_preview(hotspot_preview_owner);
		}
	}
}

void LevelEditor::register_hotspot_patch_editor(HotspotPatchEditor *p_editor) {
	ERR_FAIL_NULL(p_editor);
	if (hotspot_patch_editors.find(p_editor) < 0) {
		hotspot_patch_editors.push_back(p_editor);
	}
}

void LevelEditor::unregister_hotspot_patch_editor(HotspotPatchEditor *p_editor) {
	if (!p_editor) {
		return;
	}
	const int index = hotspot_patch_editors.find(p_editor);
	if (index >= 0) {
		hotspot_patch_editors.remove_at(index);
	}
	if (hotspot_preview_owner == p_editor->get_instance_id()) {
		hotspot_preview_owner = ObjectID();
		hotspot_preview_atlas.unref();
		hotspot_preview_enabled = false;
		hotspot_debug_enabled = false;
		_apply_hotspot_preview_request();
	}
}

void LevelEditor::set_hotspot_preview_request(HotspotPatchEditor *p_owner, const Ref<HotspotAtlas> &p_atlas,
		bool p_preview_enabled, bool p_debug_enabled) {
	ERR_FAIL_NULL(p_owner);
	hotspot_preview_owner = p_owner->get_instance_id();
	hotspot_preview_atlas = p_atlas;
	hotspot_preview_enabled = p_preview_enabled;
	hotspot_debug_enabled = p_debug_enabled;
	if (!hotspot_preview_enabled && !hotspot_debug_enabled) {
		hotspot_preview_atlas.unref();
	}
	_apply_hotspot_preview_request();
}

void LevelEditor::refresh_hotspot_preview_request(HotspotPatchEditor *p_owner) {
	if (p_owner && hotspot_preview_owner == p_owner->get_instance_id()) {
		_apply_hotspot_preview_request();
	}
}

void LevelEditor::notify_level_view_selection_changed(LevelEditorView *p_view) {
	(void)p_view;
	if (hotspot_preview_enabled || hotspot_debug_enabled) {
		_apply_hotspot_preview_request();
	}
}

Dictionary LevelEditor::get_hotspot_patch_editor_state() const {
	Dictionary state;
	HotspotPatchEditor *editor = _get_active_hotspot_patch_editor();
	state["open"] = editor != nullptr;
	if (!editor) {
		return state;
	}
	state["view_class"] = editor->get_class();
	const Ref<HotspotAtlas> atlas = editor->get_atlas();
	state["document_type"] = editor->get_hotspot_document() ? int(editor->get_hotspot_document()->get_type()) : -1;
	state["patch_count"] = atlas.is_valid() ? atlas->get_patches().size() : 0;
	state["selected_patch"] = editor->get_selected_patch();
	state["dirty"] = editor->get_hotspot_document() && editor->get_hotspot_document()->is_dirty();
	state["resource_edited"] = atlas.is_valid() && atlas->is_edited();
	state["preview_enabled"] = editor->is_preview_enabled();
	state["debug_enabled"] = editor->is_debug_enabled();
	state["snap_enabled"] = editor->is_snap_enabled();
	state["grid_step_px"] = editor->get_grid_step_px();
	state["atlas_path"] = atlas.is_valid() ? atlas->get_path() : String();
	return state;
}

bool LevelEditor::hotspot_editor_create_patch_px(const Rect2 &p_rect_px) {
	HotspotPatchEditor *editor = _get_active_hotspot_patch_editor();
	return editor && editor->create_patch_px(p_rect_px);
}

bool LevelEditor::hotspot_editor_set_patch_rect_px(int p_patch_index, const Rect2 &p_rect_px) {
	HotspotPatchEditor *editor = _get_active_hotspot_patch_editor();
	return editor && editor->set_patch_rect_px(p_patch_index, p_rect_px, TTR("Resize Hotspot Patch"));
}

Error LevelEditor::hotspot_editor_import_rect(const String &p_path) {
	HotspotPatchEditor *editor = _get_active_hotspot_patch_editor();
	return editor ? editor->import_rect_path(p_path) : ERR_DOES_NOT_EXIST;
}

Error LevelEditor::hotspot_editor_export_rect(const String &p_path) {
	HotspotPatchEditor *editor = _get_active_hotspot_patch_editor();
	return editor ? editor->export_rect_path(p_path) : ERR_DOES_NOT_EXIST;
}

Error LevelEditor::hotspot_editor_add_binding(const String &p_pattern_key) {
	HotspotPatchEditor *editor = _get_active_hotspot_patch_editor();
	return editor ? editor->add_binding(p_pattern_key) : ERR_DOES_NOT_EXIST;
}

Error LevelEditor::hotspot_editor_remove_binding(const String &p_pattern_key) {
	HotspotPatchEditor *editor = _get_active_hotspot_patch_editor();
	return editor ? editor->remove_binding(p_pattern_key) : ERR_DOES_NOT_EXIST;
}

void LevelEditor::hotspot_editor_set_preview_enabled(bool p_enabled) {
	if (HotspotPatchEditor *editor = _get_active_hotspot_patch_editor()) {
		editor->set_preview_enabled(p_enabled);
	}
}

void LevelEditor::hotspot_editor_set_debug_enabled(bool p_enabled) {
	if (HotspotPatchEditor *editor = _get_active_hotspot_patch_editor()) {
		editor->set_debug_enabled(p_enabled);
	}
}

void LevelEditor::hotspot_editor_save() {
	if (HotspotPatchEditor *editor = _get_active_hotspot_patch_editor()) {
		editor->save_resource();
	}
}

bool LevelEditor::modify_selected_texture(int p_operation, const Vector2 &p_value) {
	LevelEditorView *view = _get_active_view();
	return view && view->modify_selected_texture(p_operation, p_value);
}

bool LevelEditor::apply_active_material_to_selection(LevelDocument *p_document) {
	LevelEditorView *view = _get_view_for_document(p_document);
	return view && view->apply_active_material_to_selection();
}

void LevelEditor::request_material_preview(const String &p_path, const Callable &p_callback) {
	if (material_preview_queue.is_valid() && !p_path.is_empty()) {
		material_preview_queue->request_preview(p_path, p_callback);
	}
}

void LevelEditor::_material_preview_source_changed(const String &p_path) {
	if (material_preview_queue.is_valid()) {
		material_preview_queue->invalidate(p_path);
	}
	if (texel_density_scanner.is_valid()) {
		texel_density_scanner->invalidate(p_path);
	}
}

Ref<MaterialIndex> LevelEditor::get_material_index() const {
	return material_index;
}

Ref<TexelDensityScanner> LevelEditor::get_texel_density_scanner() const {
	return texel_density_scanner;
}

Ref<HotspotBinding> LevelEditor::get_hotspot_bindings() const {
	return hotspot_bindings;
}

void LevelEditor::set_hotspot_mapping_mode_override(int p_mode) {
	set_hotspot_mapping_mode_override(_get_active_level_document(), p_mode);
}

void LevelEditor::set_hotspot_mapping_mode_override(LevelDocument *p_document, int p_mode) {
	ERR_FAIL_COND_MSG(p_mode < -1 || p_mode > HotspotAtlas::MAPPING_FOLLOW_ACTIVE_QUADS, "Hotspot mapping override must be Atlas Default (-1) or a HotspotAtlas mapping mode.");
	if (p_document) {
		p_document->hotspot_mapping_mode_override = p_mode;
	}
}

int LevelEditor::get_hotspot_mapping_mode_override() const {
	return get_hotspot_mapping_mode_override(_get_active_level_document());
}

int LevelEditor::get_hotspot_mapping_mode_override(const LevelDocument *p_document) const {
	return p_document ? p_document->hotspot_mapping_mode_override : -1;
}

String LevelEditor::_get_configured_hotspot_bindings_path() const {
	static const String default_path = "res://levels/hotspot_bindings.tres";
	EditorSettings *settings = EditorSettings::get_singleton();
	if (!settings || !settings->has_setting("level_editor/hotspot/bindings_path")) {
		return default_path;
	}
	const Variant configured = settings->get("level_editor/hotspot/bindings_path");
	if (configured.get_type() != Variant::STRING) {
		return default_path;
	}
	const String path = String(configured).strip_edges();
	return path.is_empty() ? default_path : path;
}

String LevelEditor::get_hotspot_bindings_path() const {
	return _get_configured_hotspot_bindings_path();
}

String LevelEditor::get_hotspot_pattern_key(const String &p_material_path) const {
	if (p_material_path.is_empty() || texel_density_scanner.is_null()) {
		return String();
	}
	const Dictionary scan_result = texel_density_scanner->scan_path(p_material_path);
	if (!bool(scan_result.get("found", false))) {
		return String();
	}
	return HotspotBinding::pattern_key_from_texture_path(scan_result.get("texture_path", String()));
}

String LevelEditor::resolve_hotspot_atlas(const String &p_material_path) const {
	if (hotspot_bindings.is_null()) {
		return String();
	}
	const String pattern_key = get_hotspot_pattern_key(p_material_path);
	return pattern_key.is_empty() ? String() : hotspot_bindings->resolve_pattern(pattern_key);
}

Error LevelEditor::bind_hotspot_atlas(const String &p_material_path, const String &p_atlas_path) {
	const String pattern_key = get_hotspot_pattern_key(p_material_path);
	if (pattern_key.is_empty()) {
		return ERR_DOES_NOT_EXIST;
	}
	return set_hotspot_pattern_binding(pattern_key, p_atlas_path);
}

Error LevelEditor::set_hotspot_pattern_binding(const String &p_pattern_key, const String &p_atlas_path) {
	if (p_pattern_key.strip_edges().is_empty() || p_atlas_path.strip_edges().is_empty()) {
		return ERR_INVALID_PARAMETER;
	}
	if (hotspot_bindings.is_null()) {
		hotspot_bindings.instantiate();
	}
	hotspot_bindings->set_binding(p_pattern_key, p_atlas_path);
	return save_hotspot_bindings();
}

Error LevelEditor::erase_hotspot_pattern_binding(const String &p_pattern_key) {
	if (p_pattern_key.strip_edges().is_empty() || hotspot_bindings.is_null()) {
		return ERR_INVALID_PARAMETER;
	}
	if (!hotspot_bindings->erase_binding(p_pattern_key)) {
		return ERR_DOES_NOT_EXIST;
	}
	return save_hotspot_bindings();
}

Error LevelEditor::save_hotspot_bindings() {
	if (hotspot_bindings.is_null()) {
		hotspot_bindings.instantiate();
	}
	const String path = _get_configured_hotspot_bindings_path();
	if (!path.begins_with("res://") || (path.get_extension().to_lower() != "tres" && path.get_extension().to_lower() != "res")) {
		return ERR_INVALID_PARAMETER;
	}
	const String directory = path.get_base_dir();
	if (!directory.is_empty() && directory != "res://") {
		const Error directory_error = DirAccess::make_dir_recursive_absolute(ProjectSettings::get_singleton()->globalize_path(directory));
		if (directory_error != OK) {
			return directory_error;
		}
	}
	return ResourceSaver::save(hotspot_bindings, path);
}

Error LevelEditor::reload_hotspot_bindings() {
	const String path = _get_configured_hotspot_bindings_path();
	if (!path.begins_with("res://")) {
		hotspot_bindings.instantiate();
		return ERR_INVALID_PARAMETER;
	}
	if (!FileAccess::exists(path)) {
		hotspot_bindings.instantiate();
		return ERR_FILE_NOT_FOUND;
	}
	Ref<HotspotBinding> loaded = ResourceLoader::load(path, "HotspotBinding", ResourceFormatLoader::CACHE_MODE_IGNORE);
	if (loaded.is_null()) {
		hotspot_bindings.instantiate();
		return ERR_FILE_CORRUPT;
	}
	hotspot_bindings = loaded;
	return OK;
}

Ref<BlockoutMaterialRegistry> LevelEditor::get_blockout_material_registry() const {
	return blockout_material_registry;
}

LevelEditor::ToolMode LevelEditor::get_tool_mode() const {
	LevelEditorView *active_view = _get_active_view();
	return active_view ? active_view->get_tool_mode() : tool_mode;
}

void LevelEditor::set_tool_mode(ToolMode p_mode) {
	set_tool_mode(_get_active_view(), p_mode);
}

void LevelEditor::set_tool_mode(LevelEditorView *p_view, ToolMode p_mode) {
	ERR_FAIL_INDEX(int(p_mode), 2);
	tool_mode = p_mode; // Compatibility fallback when no Level document is active.
	if (!p_view || p_view->get_tool_mode() == p_mode) {
		return;
	}
	p_view->set_tool_mode(p_mode);
}

void LevelEditor::set_snap_step(real_t p_step) {
	ERR_FAIL_COND_MSG(!Math::is_finite(p_step) || p_step <= CMP_EPSILON, "Level editor snap step must be finite and greater than zero.");
	if (Math::is_equal_approx(snap_step, p_step)) {
		return;
	}
	snap_step = p_step;
	emit_signal(SNAME("snap_settings_changed"), snap_step, snap_enabled);
}

void LevelEditor::set_snap_enabled(bool p_enabled) {
	if (snap_enabled == p_enabled) {
		return;
	}
	snap_enabled = p_enabled;
	emit_signal(SNAME("snap_settings_changed"), snap_step, snap_enabled);
}

void LevelEditor::_register_view(LevelEditorView *p_view) {
	ERR_FAIL_NULL(p_view);
	ERR_FAIL_COND(views.find(p_view) >= 0);
	views.push_back(p_view);
	p_view->set_tool_mode(TOOL_SELECT);
	if (hotspot_preview_enabled || hotspot_debug_enabled) {
		_apply_hotspot_preview_request();
	}
}

void LevelEditor::_unregister_view(LevelEditorView *p_view) {
	if (p_view) {
		p_view->clear_hotspot_preview(hotspot_preview_owner);
		p_view->set_hotspot_fit_debug_enabled(false);
	}
	const int index = views.find(p_view);
	if (index >= 0) {
		views.remove_at(index);
	}
	if (hotspot_preview_enabled || hotspot_debug_enabled) {
		_apply_hotspot_preview_request();
	}
}

void LevelEditor::_scan_node(Node *p_node) {
	if (!p_node) {
		return;
	}
	if (LevelBlock *block = Object::cast_to<LevelBlock>(p_node)) {
		_track_block(block);
	}
	for (int i = 0; i < p_node->get_child_count(); i++) {
		_scan_node(p_node->get_child(i));
	}
}

void LevelEditor::_node_added(Node *p_node) {
	if (LevelBlock *block = Object::cast_to<LevelBlock>(p_node)) {
		_track_block(block);
	}
}

void LevelEditor::_node_removed(Node *p_node) {
	if (LevelBlock *block = Object::cast_to<LevelBlock>(p_node)) {
		_untrack_block(block);
	}
}

void LevelEditor::_track_block(LevelBlock *p_block) {
	ERR_FAIL_NULL(p_block);
	const ObjectID block_id = p_block->get_instance_id();
	const int64_t bound_id = (int64_t)(uint64_t)block_id;
	const Callable entered_callable = callable_mp(this, &LevelEditor::_block_world_entered).bind(bound_id);
	const Callable exiting_callable = callable_mp(this, &LevelEditor::_block_world_exiting).bind(bound_id);
	const Callable baked_callable = callable_mp(this, &LevelEditor::_block_baked).bind(bound_id);
	const Callable transform_callable = callable_mp(this, &LevelEditor::_block_transform_changed).bind(bound_id);
	if (!p_block->is_connected(SNAME("level_world_entered"), entered_callable)) {
		p_block->connect(SNAME("level_world_entered"), entered_callable);
	}
	if (!p_block->is_connected(SNAME("level_world_exiting"), exiting_callable)) {
		p_block->connect(SNAME("level_world_exiting"), exiting_callable);
	}
	if (!p_block->is_connected(SNAME("baked"), baked_callable)) {
		p_block->connect(SNAME("baked"), baked_callable);
	}
	p_block->set_notify_transform(true);
	if (!p_block->is_connected(SNAME("level_transform_changed"), transform_callable)) {
		p_block->connect(SNAME("level_transform_changed"), transform_callable);
	}
	if (p_block->is_inside_world()) {
		_register_block(block_id);
	}
}

void LevelEditor::_untrack_block(LevelBlock *p_block) {
	ERR_FAIL_NULL(p_block);
	const ObjectID block_id = p_block->get_instance_id();
	const int64_t bound_id = (int64_t)(uint64_t)block_id;
	const Callable entered_callable = callable_mp(this, &LevelEditor::_block_world_entered).bind(bound_id);
	const Callable exiting_callable = callable_mp(this, &LevelEditor::_block_world_exiting).bind(bound_id);
	const Callable baked_callable = callable_mp(this, &LevelEditor::_block_baked).bind(bound_id);
	const Callable transform_callable = callable_mp(this, &LevelEditor::_block_transform_changed).bind(bound_id);
	if (p_block->is_connected(SNAME("level_world_entered"), entered_callable)) {
		p_block->disconnect(SNAME("level_world_entered"), entered_callable);
	}
	if (p_block->is_connected(SNAME("level_world_exiting"), exiting_callable)) {
		p_block->disconnect(SNAME("level_world_exiting"), exiting_callable);
	}
	if (p_block->is_connected(SNAME("baked"), baked_callable)) {
		p_block->disconnect(SNAME("baked"), baked_callable);
	}
	if (p_block->is_connected(SNAME("level_transform_changed"), transform_callable)) {
		p_block->disconnect(SNAME("level_transform_changed"), transform_callable);
	}
	_unregister_block(block_id);
}

bool LevelEditor::_get_block_world_aabb(LevelBlock *p_block, AABB &r_aabb) const {
	if (!p_block || !p_block->is_inside_world() || p_block->get_data().is_null()) {
		return false;
	}
	const PackedVector3Array positions = p_block->get_data()->get_vertex_positions();
	const PackedByteArray alive = p_block->get_data()->get_vertex_alive();
	bool initialized = false;
	AABB local_aabb;
	const int vertex_count = MIN(positions.size(), alive.size());
	for (int vertex_id = 0; vertex_id < vertex_count; vertex_id++) {
		if (alive[vertex_id] == 0 || !positions[vertex_id].is_finite()) {
			continue;
		}
		if (!initialized) {
			local_aabb = AABB(positions[vertex_id], Vector3());
			initialized = true;
		} else {
			local_aabb.expand_to(positions[vertex_id]);
		}
	}
	if (!initialized) {
		return false;
	}
	r_aabb = p_block->get_global_transform().xform(local_aabb);
	return r_aabb.position.is_finite() && r_aabb.size.is_finite();
}

void LevelEditor::_register_block(ObjectID p_block_id) {
	Object *object = ObjectDB::get_instance(p_block_id);
	LevelBlock *block = Object::cast_to<LevelBlock>(object);
	Node3DEditor *node_3d_editor = Node3DEditor::get_singleton();
	AABB world_aabb;
	if (!block || !node_3d_editor || !_get_block_world_aabb(block, world_aabb)) {
		_unregister_block(p_block_id);
		return;
	}
	HashMap<ObjectID, DynamicBVH::ID>::Iterator registered = block_bvh_ids.find(p_block_id);
	if (registered) {
		node_3d_editor->update_gizmo_bvh_node(registered->value, world_aabb);
	} else {
		block_bvh_ids[p_block_id] = node_3d_editor->insert_gizmo_bvh_node(block, world_aabb);
	}
}

void LevelEditor::_unregister_block(ObjectID p_block_id) {
	HashMap<ObjectID, DynamicBVH::ID>::Iterator registered = block_bvh_ids.find(p_block_id);
	if (!registered) {
		return;
	}
	if (Node3DEditor *node_3d_editor = Node3DEditor::get_singleton()) {
		node_3d_editor->remove_gizmo_bvh_node(registered->value);
	}
	block_bvh_ids.erase(p_block_id);
}

void LevelEditor::_block_world_entered(int64_t p_block_id) {
	_register_block(ObjectID((uint64_t)p_block_id));
}

void LevelEditor::_block_world_exiting(int64_t p_block_id) {
	_unregister_block(ObjectID((uint64_t)p_block_id));
}

void LevelEditor::_block_baked(int64_t p_block_id) {
	_register_block(ObjectID((uint64_t)p_block_id));
}

void LevelEditor::_block_transform_changed(int64_t p_block_id) {
	_register_block(ObjectID((uint64_t)p_block_id));
}

void LevelEditor::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			if (EditorNode *editor_node = EditorNode::get_singleton()) {
				editor_node->connect(SNAME("scene_changed"), callable_mp(this, &LevelEditor::_scene_changed));
			}
			callable_mp(this, &LevelEditor::_scene_changed).call_deferred();
			SceneTree *scene_tree = get_tree();
			if (scene_tree) {
				scene_tree->connect(SNAME("node_added"), callable_mp(this, &LevelEditor::_node_added));
				scene_tree->connect(SNAME("node_removed"), callable_mp(this, &LevelEditor::_node_removed));
				_scan_node(scene_tree->get_root());
			}
		} break;
		case NOTIFICATION_EXIT_TREE: {
			if (EditorNode *editor_node = EditorNode::get_singleton()) {
				const Callable scene_callable = callable_mp(this, &LevelEditor::_scene_changed);
				if (editor_node->is_connected(SNAME("scene_changed"), scene_callable)) {
					editor_node->disconnect(SNAME("scene_changed"), scene_callable);
				}
			}
			SceneTree *scene_tree = get_tree();
			if (scene_tree) {
				const Callable added_callable = callable_mp(this, &LevelEditor::_node_added);
				const Callable removed_callable = callable_mp(this, &LevelEditor::_node_removed);
				if (scene_tree->is_connected(SNAME("node_added"), added_callable)) {
					scene_tree->disconnect(SNAME("node_added"), added_callable);
				}
				if (scene_tree->is_connected(SNAME("node_removed"), removed_callable)) {
					scene_tree->disconnect(SNAME("node_removed"), removed_callable);
				}
			}
			Vector<ObjectID> registered_ids;
			for (const KeyValue<ObjectID, DynamicBVH::ID> &entry : block_bvh_ids) {
				registered_ids.push_back(entry.key);
			}
			for (const ObjectID &block_id : registered_ids) {
				_unregister_block(block_id);
			}
		} break;
	}
}

void LevelEditor::_scene_changed() {
	for (MaterialBrowserDock *browser : material_browser_views) {
		if (browser) {
			browser->active_document_changed();
		}
	}
}

MaterialBrowserDock *LevelEditor::get_material_browser_dock() const {
	LevelDocument *active_document = _get_active_level_document();
	for (MaterialBrowserDock *browser : material_browser_views) {
		if (browser && browser->get_bound_document() == active_document) {
			return browser;
		}
	}
	return material_browser_views.is_empty() ? nullptr : material_browser_views[0];
}

LevelEditorView *LevelEditor::create_editor_view(LevelDocument *p_document) {
	ERR_FAIL_NULL_V(p_document, nullptr);
	return memnew(LevelEditorView(p_document));
}

MaterialBrowserDock *LevelEditor::create_material_browser_view(LevelDocument *p_document) {
	ERR_FAIL_NULL_V(p_document, nullptr);
	MaterialBrowserDock *browser = memnew(MaterialBrowserDock(this, material_index, texel_density_scanner, p_document, material_preview_queue));
	material_browser_views.push_back(browser);
	return browser;
}

void LevelEditor::release_material_browser_view(MaterialBrowserDock *p_browser) {
	const int index = material_browser_views.find(p_browser);
	if (index >= 0) {
		material_browser_views.remove_at(index);
	}
}

HotspotPatchEditor *LevelEditor::create_editor_view(HotspotAtlasDocument *p_document) {
	ERR_FAIL_NULL_V(p_document, nullptr);
	return memnew(HotspotPatchEditor(p_document));
}

LevelEditor::LevelEditor() {
	ERR_FAIL_COND(singleton != nullptr);
	singleton = this;
	set_name("LevelEditor");

	ED_SHORTCUT("level_editor/add_block", TTRC("Add Block"), KeyModifierMask::SHIFT | Key::B);
	ED_SHORTCUT("level_editor/blockout_material_1", TTRC("Blockout Material 1"), KeyModifierMask::SHIFT | KeyModifierMask::ALT | Key::KEY_1);
	ED_SHORTCUT("level_editor/blockout_material_2", TTRC("Blockout Material 2"), KeyModifierMask::SHIFT | KeyModifierMask::ALT | Key::KEY_2);
	ED_SHORTCUT("level_editor/blockout_material_3", TTRC("Blockout Material 3"), KeyModifierMask::SHIFT | KeyModifierMask::ALT | Key::KEY_3);
	ED_SHORTCUT("level_editor/blockout_material_4", TTRC("Blockout Material 4"), KeyModifierMask::SHIFT | KeyModifierMask::ALT | Key::KEY_4);
	ED_SHORTCUT("level_editor/blockout_material_5", TTRC("Blockout Material 5"), KeyModifierMask::SHIFT | KeyModifierMask::ALT | Key::KEY_5);
	ED_SHORTCUT("level_editor/blockout_material_6", TTRC("Blockout Material 6"), KeyModifierMask::SHIFT | KeyModifierMask::ALT | Key::KEY_6);
	ED_SHORTCUT("level_editor/blockout_material_7", TTRC("Blockout Material 7"), KeyModifierMask::SHIFT | KeyModifierMask::ALT | Key::KEY_7);
	ED_SHORTCUT("level_editor/blockout_material_8", TTRC("Blockout Material 8"), KeyModifierMask::SHIFT | KeyModifierMask::ALT | Key::KEY_8);
	ED_SHORTCUT("level_editor/blockout_material_9", TTRC("Blockout Material 9"), KeyModifierMask::SHIFT | KeyModifierMask::ALT | Key::KEY_9);
	ED_SHORTCUT("level_editor/blockout_material_0", TTRC("Blockout Material 0"), KeyModifierMask::SHIFT | KeyModifierMask::ALT | Key::KEY_0);

	material_index.instantiate();
	material_index->initialize();
	texel_density_scanner.instantiate();
	material_preview_queue.instantiate();
	material_preview_queue->initialize(texel_density_scanner);
	material_index->connect(SNAME("material_removed"), callable_mp(this, &LevelEditor::_material_preview_source_changed));
	material_index->connect(SNAME("material_changed"), callable_mp(this, &LevelEditor::_material_preview_source_changed));
	reload_hotspot_bindings();
	blockout_material_registry.instantiate();
	blockout_material_registry->initialize(material_index);
	Engine::Singleton engine_singleton("LevelEditor", this);
	engine_singleton.editor_only = true;
	Engine::get_singleton()->add_singleton(engine_singleton);
}

LevelEditor::~LevelEditor() {
	if (singleton == this) {
		Engine::get_singleton()->remove_singleton("LevelEditor");
		singleton = nullptr;
	}
}
