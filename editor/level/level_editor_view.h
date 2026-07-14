/**************************************************************************/
/*  level_editor_view.h                                                   */
/**************************************************************************/
/*  G-Level LE0: per-pane VIEW state for a LevelDocument.                 */
/*  Render/world/camera/grid state lives here, never on LevelEditor.      */
/**************************************************************************/

#pragma once

#include "core/input/input.h"
#include "core/templates/rid.h"
#include "editor/level/level_editor.h"
#include "editor/level/level_editor_tool.h"
#include "scene/gui/control.h"
#include "scene/resources/immediate_mesh.h"
#include "scene/resources/material.h"

class Camera3D;
class Button;
class HBoxContainer;
class Label;
class LevelDocument;
class LevelMarqueeOverlay;
class SelectionHighlightOverlay;
class SelectionModel;
class SubViewport;
class SubViewportContainer;
class VBoxContainer;

class LevelEditorView : public Control {
	GDCLASS(LevelEditorView, Control);

	LevelDocument *document = nullptr; // DOCUMENT state; owned by EditorData.
	VBoxContainer *surface_layout = nullptr;
	HBoxContainer *top_strip = nullptr;
	SubViewportContainer *viewport_container = nullptr;
	SubViewport *viewport = nullptr;
	Camera3D *camera = nullptr;
	Button *select_tool_button = nullptr;
	Button *block_tool_button = nullptr;
	Label *selection_mode_indicator = nullptr;
	LevelMarqueeOverlay *marquee_overlay = nullptr;
	SelectionHighlightOverlay *selection_overlay = nullptr;
	Ref<LevelEditorTool> tools[2];
	Ref<LevelEditorTool> active_tool;

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
	void _viewport_gui_input(const Ref<InputEvent> &p_event);
	void _process_freelook(double p_delta);
	void _selection_changed(const PackedInt64Array &p_dirty_blocks);
	void _sync_selection_metadata();

protected:
	static void _bind_methods() {}
	void _notification(int p_what);
	virtual void shortcut_input(const Ref<InputEvent> &p_event) override;

public:
	LevelDocument *get_level_document() const { return document; }
	SubViewport *get_level_viewport() const { return viewport; }
	int get_gizmo_layer() const { return gizmo_layer; }
	void set_tool_mode(LevelEditor::ToolMode p_mode);
	void set_marquee_rect(const Rect2 &p_rect, bool p_visible);
	void set_last_selection_action(const StringName &p_action);

	// DocumentView creates toolbar_host after minting the surface. Until then the strip
	// stays inside this Control; this call moves it into the pane header without cloning it.
	void mount_top_strip(Control *p_toolbar_host);

	LevelEditorView(LevelDocument *p_document);
	~LevelEditorView();
};
