/**************************************************************************/
/*  level_editor_view.h                                                   */
/**************************************************************************/
/*  G-Level LE0: per-pane VIEW state for a LevelDocument.                 */
/*  Render/world/camera/grid state lives here, never on LevelEditor.      */
/**************************************************************************/

#pragma once

#include "core/input/input.h"
#include "core/object/object_id.h"
#include "core/templates/rid.h"
#include "core/variant/array.h"
#include "editor/level/level_editor.h"
#include "editor/level/level_editor_tool.h"
#include "editor/level/tool_overlay.h"
#include "scene/gui/control.h"
#include "scene/resources/immediate_mesh.h"
#include "scene/resources/material.h"

class Camera3D;
class Button;
class CheckButton;
class FastTextureOverlay;
class GridContainer;
class HBoxContainer;
class HotspotAtlas;
class Label;
class LevelDocument;
class LevelMarqueeOverlay;
class OptionButton;
class PanelContainer;
class ScrollContainer;
class SelectionHighlightOverlay;
class SelectionModel;
class SpinBox;
class SubViewport;
class SubViewportContainer;
class TextureRect;
class VBoxContainer;

class LevelEditorView : public Control {
	GDCLASS(LevelEditorView, Control);

public:
	enum MaterialsDrawerRequest {
		MATERIALS_DRAWER_TOGGLE,
		MATERIALS_DRAWER_REVEAL_ACTIVE,
	};

private:

	LevelDocument *document = nullptr; // DOCUMENT state; owned by EditorData.
	VBoxContainer *surface_layout = nullptr;
	HBoxContainer *top_strip = nullptr;
	Label *grid_step_label = nullptr;
	Button *grid_step_decrease_button = nullptr;
	Button *grid_step_increase_button = nullptr;
	SubViewportContainer *viewport_container = nullptr;
	SubViewport *viewport = nullptr;
	Camera3D *camera = nullptr;
	VBoxContainer *tool_rail = nullptr;
	Button *select_tool_button = nullptr;
	Button *block_tool_button = nullptr;
	Button *context_panel_toggle_button = nullptr;
	Button *active_material_swatch = nullptr;
	Control *context_panel_separator = nullptr;
	PanelContainer *context_panel = nullptr;
	Control *fast_texture_context = nullptr;
	Control *block_tool_context = nullptr;
	Control *texture_context = nullptr;
	Control *selection_hint_context = nullptr;
	Control *hotspot_context = nullptr;
	Label *context_title = nullptr;
	Label *selection_hint_label = nullptr;
	Label *texture_scope_label = nullptr;
	Label *captured_mapping_label = nullptr;
	Label *block_material_label = nullptr;
	CheckButton *block_snap_enabled = nullptr;
	SpinBox *block_snap_step = nullptr;
	OptionButton *hotspot_mapping_mode = nullptr;
	CheckButton *hotspot_debug_toggle = nullptr;
	Vector<Button *> icon_buttons;
	Vector<StringName> icon_button_names;
	Vector<Button *> compact_context_buttons;
	bool context_panel_expanded = true;
	bool texture_context_hovered = false;
	bool context_active = false;
	LevelEditor::ToolMode tool_mode = LevelEditor::TOOL_SELECT;
	Label *selection_mode_indicator = nullptr;
	TextureRect *active_material_preview = nullptr;
	Label *active_material_label = nullptr;
	LevelMarqueeOverlay *marquee_overlay = nullptr;
	SelectionHighlightOverlay *selection_overlay = nullptr;
	FastTextureOverlay *fast_texture_overlay = nullptr;
	ToolOverlay hotspot_fit_overlay;
	PanelContainer *hotspot_fit_hud = nullptr;
	Label *hotspot_fit_hud_label = nullptr;
	ObjectID hotspot_preview_owner;
	Ref<HotspotAtlas> hotspot_preview_atlas;
	Array hotspot_preview_diagnostics;
	bool hotspot_preview_active = false;
	bool hotspot_fit_debug_enabled = false;
	ObjectID fast_texture_previous_focus_owner;
	bool fast_texture_input_context_active = false;
	bool fast_texture_closing = false;
	Ref<LevelEditorTool> tools[2];
	Ref<LevelEditorTool> active_tool;
	bool registered_with_level_editor = false;

	Ref<ImmediateMesh> grid_mesh;
	Ref<StandardMaterial3D> grid_material;
	RID grid_instance;
	bool grid_attached = false;
	int gizmo_layer = 20; // Fallback only; normally allocated per document world.
	bool gizmo_layer_allocated = false;
	uint64_t last_selection_revision = ~uint64_t(0);

	Vector3 orbit_pivot;
	float orbit_yaw = Math::PI / 4.0f;
	float orbit_pitch = Math::deg_to_rad(28.0f);
	float orbit_distance = 14.0f;
	float freelook_yaw = 0.0f;
	float freelook_pitch = 0.0f;
	bool orbiting = false;
	bool panning = false;
	bool freelook = false;
	Input::MouseMode previous_mouse_mode = Input::MouseMode::MOUSE_MODE_VISIBLE;

	void _create_grid();
	void _reconcile_grid();
	void _update_grid_transform();
	void _update_orbit_camera();
	void _begin_freelook();
	void _end_freelook();
	void _tool_button_pressed(int p_mode);
	void _context_panel_toggle_pressed();
	void _set_context_panel_expanded(bool p_expanded);
	void _refresh_context_panel();
	void _update_ui_theme();
	void _register_icon_button(Button *p_button, const StringName &p_icon_name);
	void _texture_context_mouse_entered();
	void _texture_context_mouse_exited();
	int _get_selected_level_block_count() const;
	void _modify_texture_pressed(int p_operation, const Vector2 &p_value);
	void _hotspot_mapping_mode_selected(int p_index);
	void _hotspot_fit_pressed(bool p_individual);
	void _hotspot_debug_toggled(bool p_enabled);
	void _block_snap_toggled(bool p_enabled);
	void _block_snap_step_changed(double p_value);
	void _grid_step_decrease_pressed();
	void _grid_step_increase_pressed();
	void _snap_settings_changed(double p_step, bool p_enabled);
	void _fast_texture_accept_pressed();
	void _fast_texture_cancel_pressed();
	void _active_material_swatch_pressed();
	void _emit_materials_drawer_request(MaterialsDrawerRequest p_request, bool p_focus_search);
	bool _try_toggle_materials_shortcut(const Ref<InputEvent> &p_event);
	bool _invoke_select_tool_shortcut(Key p_key, bool p_shift = false, bool p_ctrl = false, bool p_alt = false);
	bool _try_activate_blockout_shortcut(const Ref<InputEvent> &p_event);
	bool _try_open_fast_texture_shortcut(const Ref<InputEvent> &p_event);
	bool _open_fast_texture();
	void _close_fast_texture(bool p_accept, bool p_restore_focus = true);
	void _push_fast_texture_input_context();
	void _pop_fast_texture_input_context(bool p_restore_focus);
	void _fast_texture_focus_exited();
	void _cancel_fast_texture_if_unfocused();
	void _show_fast_texture_status(const String &p_message, bool p_warning = false);
	void _viewport_gui_input(const Ref<InputEvent> &p_event);
	void _process_freelook(double p_delta);
	void _selection_changed(const PackedInt64Array &p_dirty_blocks);
	void _object_selection_changed();
	void _sync_selection_metadata();
	void _update_transform_gizmo();
	void _active_material_changed(const Ref<Material> &p_material, const String &p_path);
	void _active_material_changed_for_document(int64_t p_document_history_id, const Ref<Material> &p_material, const String &p_path);
	void _active_material_preview_ready(const String &p_path, const Ref<Texture2D> &p_texture);
	void _capture_committed_hotspot_diagnostics();
	void _refresh_hotspot_fit_overlay();
	bool _run_hotspot_preview();
	void _clear_hotspot_fit_overlay();

	friend class FastTextureOverlay;

protected:
	static void _bind_methods();
	void _notification(int p_what);
	virtual void shortcut_input(const Ref<InputEvent> &p_event) override;

public:
	LevelDocument *get_level_document() const { return document; }
	LevelEditor::ToolMode get_tool_mode() const { return tool_mode; }
	SubViewport *get_level_viewport() const { return viewport; }
	int get_gizmo_layer() const { return gizmo_layer; }
	void set_tool_mode(LevelEditor::ToolMode p_mode);
	void set_marquee_rect(const Rect2 &p_rect, bool p_visible);
	void set_last_selection_action(const StringName &p_action);
	bool apply_active_material_to_selection();
	bool modify_selected_texture(int p_operation, const Vector2 &p_value = Vector2(1, 1));
	bool has_hotspot_face_selection() const;
	bool show_hotspot_preview(ObjectID p_owner, const Ref<HotspotAtlas> &p_atlas);
	void clear_hotspot_preview(ObjectID p_owner = ObjectID());
	void set_hotspot_fit_debug_enabled(bool p_enabled);
	Dictionary get_context_panel_state() const;
	void set_context_panel_state(const Dictionary &p_state);
	void set_context_active(bool p_active) { context_active = p_active; }
	bool is_context_active() const { return context_active; }
	void request_materials_drawer_reveal(bool p_focus_search = false);

	// DocumentView creates toolbar_host after minting the surface. Until then the strip
	// stays inside this Control; this call moves it into the pane header without cloning it.
	void mount_top_strip(Control *p_toolbar_host);

	LevelEditorView(LevelDocument *p_document);
	~LevelEditorView();
};

VARIANT_ENUM_CAST(LevelEditorView::MaterialsDrawerRequest);
