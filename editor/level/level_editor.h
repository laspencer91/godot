/**************************************************************************/
/*  level_editor.h                                                        */
/**************************************************************************/
/*  G-Level LE0: SERVICE state for the level-editor workspace seam.       */
/*  Per-pane render state belongs to LevelEditorView (ARCHITECTURE.md).    */
/**************************************************************************/

#pragma once

#include "core/math/dynamic_bvh.h"
#include "core/math/rect2.h"
#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "scene/main/node.h"
#include "scene/resources/material.h"

class BlockoutMaterialRegistry;
class HotspotAtlas;
class HotspotAtlasDocument;
class HotspotBinding;
class HotspotPatchEditor;
class LevelBlock;
class LevelDocument;
class LevelEditorView;
class MaterialBrowserDock;
class MaterialBrowserPreviewQueue;
class MaterialIndex;
class TexelDensityScanner;
class Texture2D;

class LevelEditor : public Node {
	GDCLASS(LevelEditor, Node);

public:
	enum ToolMode {
		TOOL_SELECT,
		TOOL_BLOCK,
	};
	static constexpr real_t DEFAULT_SNAP_STEP = 1.0;
	static constexpr real_t DEFAULT_BLOCK_HEIGHT = 3.0;
	static constexpr int MAJOR_GRID_MULTIPLE = 4;

private:
	static LevelEditor *singleton;
	ToolMode tool_mode = TOOL_SELECT;
	real_t snap_step = DEFAULT_SNAP_STEP;
	bool snap_enabled = true;
	real_t default_block_height = DEFAULT_BLOCK_HEIGHT;
	LocalVector<LevelEditorView *> views;
	LocalVector<HotspotPatchEditor *> hotspot_patch_editors;
	HashMap<ObjectID, DynamicBVH::ID> block_bvh_ids;
	Ref<MaterialIndex> material_index;
	Ref<TexelDensityScanner> texel_density_scanner;
	Ref<MaterialBrowserPreviewQueue> material_preview_queue;
	Ref<HotspotBinding> hotspot_bindings;
	ObjectID hotspot_preview_owner;
	Ref<HotspotAtlas> hotspot_preview_atlas;
	bool hotspot_preview_enabled = false;
	bool hotspot_debug_enabled = false;
	Ref<BlockoutMaterialRegistry> blockout_material_registry;
	LocalVector<MaterialBrowserDock *> material_browser_views;

	void _register_view(LevelEditorView *p_view);
	void _unregister_view(LevelEditorView *p_view);
	void _scan_node(Node *p_node);
	void _node_added(Node *p_node);
	void _node_removed(Node *p_node);
	void _track_block(LevelBlock *p_block);
	void _untrack_block(LevelBlock *p_block);
	void _register_block(ObjectID p_block_id);
	void _unregister_block(ObjectID p_block_id);
	void _block_world_entered(int64_t p_block_id);
	void _block_world_exiting(int64_t p_block_id);
	void _block_baked(int64_t p_block_id);
	void _block_transform_changed(int64_t p_block_id);
	bool _get_block_world_aabb(LevelBlock *p_block, AABB &r_aabb) const;
	void _scene_changed();
	void _material_preview_source_changed(const String &p_path);
	String _get_configured_hotspot_bindings_path() const;
	LevelDocument *_get_active_level_document() const;
	LevelEditorView *_get_view_for_document(const LevelDocument *p_document) const;
	LevelEditorView *_get_active_view() const;
	HotspotPatchEditor *_get_active_hotspot_patch_editor() const;
	void _apply_hotspot_preview_request();

	friend class LevelEditorView;

protected:
	static void _bind_methods();
	void _notification(int p_what);

public:
	static LevelEditor *get_singleton() { return singleton; }
	static real_t snap_step_or_default();

	ToolMode get_tool_mode() const;
	void set_tool_mode(ToolMode p_mode);
	void set_tool_mode(LevelEditorView *p_view, ToolMode p_mode);
	real_t get_snap_step() const { return snap_step; }
	void set_snap_step(real_t p_step);
	bool is_snap_enabled() const { return snap_enabled; }
	void set_snap_enabled(bool p_enabled);
	real_t get_default_block_height() const { return default_block_height; }

	// Legacy script-facing methods resolve the currently focused Level document.
	// Bound views/tools must use the LevelDocument-qualified overloads below.
	void set_active_material(const Ref<Material> &p_material, const String &p_source_path = String());
	Ref<Material> get_active_material() const;
	String get_active_material_path() const;
	String get_active_material_binding_path() const;
	void set_active_material_binding_path(const String &p_path);
	String get_active_material_display_name() const;
	void set_captured_mapping(const Dictionary &p_mapping);
	Dictionary get_captured_mapping() const;
	void clear_captured_mapping();

	void set_active_material(LevelDocument *p_document, const Ref<Material> &p_material, const String &p_source_path = String());
	Ref<Material> get_active_material(const LevelDocument *p_document) const;
	String get_active_material_path(const LevelDocument *p_document) const;
	String get_active_material_binding_path(const LevelDocument *p_document) const;
	void set_active_material_binding_path(LevelDocument *p_document, const String &p_path);
	String get_active_material_display_name(const LevelDocument *p_document) const;
	void set_captured_mapping(LevelDocument *p_document, const Dictionary &p_mapping);
	Dictionary get_captured_mapping(const LevelDocument *p_document) const;
	void clear_captured_mapping(LevelDocument *p_document);
	Ref<Texture2D> get_material_albedo_texture(const Ref<Material> &p_material, const String &p_source_path = String()) const;
	bool activate_blockout_slot(int p_slot);
	bool activate_blockout_slot(LevelDocument *p_document, int p_slot);
	bool apply_active_material_to_selection(LevelDocument *p_document);
	bool modify_selected_texture(int p_operation, const Vector2 &p_value = Vector2(1, 1));
	void request_material_preview(const String &p_path, const Callable &p_callback);

	Ref<MaterialIndex> get_material_index() const;
	Ref<TexelDensityScanner> get_texel_density_scanner() const;
	Ref<HotspotBinding> get_hotspot_bindings() const;
	void set_hotspot_mapping_mode_override(int p_mode);
	int get_hotspot_mapping_mode_override() const;
	void set_hotspot_mapping_mode_override(LevelDocument *p_document, int p_mode);
	int get_hotspot_mapping_mode_override(const LevelDocument *p_document) const;
	String get_hotspot_bindings_path() const;
	String get_hotspot_pattern_key(const String &p_material_path) const;
	String resolve_hotspot_atlas(const String &p_material_path) const;
	Error bind_hotspot_atlas(const String &p_material_path, const String &p_atlas_path);
	Error set_hotspot_pattern_binding(const String &p_pattern_key, const String &p_atlas_path);
	Error erase_hotspot_pattern_binding(const String &p_pattern_key);
	Error save_hotspot_bindings();
	Error reload_hotspot_bindings();
	Ref<BlockoutMaterialRegistry> get_blockout_material_registry() const;
	// Legacy script seam: resolves a browser bound to the focused Level document.
	MaterialBrowserDock *get_material_browser_dock() const;

	// G-Level LE0: mint one unparented VIEW for the requesting document/pane.
	// DocumentView owns placement and lifetime; this service retains no render state.
	LevelEditorView *create_editor_view(LevelDocument *p_document);
	MaterialBrowserDock *create_material_browser_view(LevelDocument *p_document);
	void release_material_browser_view(MaterialBrowserDock *p_browser);
	HotspotPatchEditor *create_editor_view(HotspotAtlasDocument *p_document);

	// WP21 view/service coordination. Patch-editor views own their UI; the
	// service only routes their preview request to a selected LevelEditorView.
	void register_hotspot_patch_editor(HotspotPatchEditor *p_editor);
	void unregister_hotspot_patch_editor(HotspotPatchEditor *p_editor);
	void set_hotspot_preview_request(HotspotPatchEditor *p_owner, const Ref<HotspotAtlas> &p_atlas,
			bool p_preview_enabled, bool p_debug_enabled);
	void refresh_hotspot_preview_request(HotspotPatchEditor *p_owner);
	void notify_level_view_selection_changed(LevelEditorView *p_view);

	// Additive scriptable seams used by the editor smoke; each delegates to the
	// same public handler used by the actual patch-editor controls.
	int get_hotspot_patch_editor_count() const { return hotspot_patch_editors.size(); }
	Dictionary get_hotspot_patch_editor_state() const;
	bool hotspot_editor_create_patch_px(const Rect2 &p_rect_px);
	bool hotspot_editor_set_patch_rect_px(int p_patch_index, const Rect2 &p_rect_px);
	Error hotspot_editor_import_rect(const String &p_path);
	Error hotspot_editor_export_rect(const String &p_path);
	Error hotspot_editor_add_binding(const String &p_pattern_key);
	Error hotspot_editor_remove_binding(const String &p_pattern_key);
	void hotspot_editor_set_preview_enabled(bool p_enabled);
	void hotspot_editor_set_debug_enabled(bool p_enabled);
	void hotspot_editor_save();

	LevelEditor();
	~LevelEditor();
};

VARIANT_ENUM_CAST(LevelEditor::ToolMode);
