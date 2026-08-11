/**************************************************************************/
/*  canvas_item_editor_plugin.h                                           */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#pragma once

#include "core/templates/local_vector.h"
#include "editor/plugins/editor_plugin.h"
#include "scene/gui/box_container.h"

class AcceptDialog;
class Button;
class ButtonGroup;
class CanvasItemEditorView;
class CanvasItemEditorViewport;
class ConfirmationDialog;
class EditorData;
class EditorDocument;
class EditorSceneActionsMenu;
class EditorSelection;
class EditorViewportChrome;
class EditorZoomWidget;
class HScrollBar;
class SubViewport;
class SubViewportContainer;
class HSplitContainer;
class MenuButton;
class PanelContainer;
class RichTextLabel;
class StyleBoxTexture;
class Timer;
class ViewPanner;
class Viewport;
class VScrollBar;
class VSeparator;
class VSplitContainer;

class CanvasItemEditorSelectedItem : public Object {
	GDCLASS(CanvasItemEditorSelectedItem, Object);

public:
	Transform2D prev_xform;
	Rect2 prev_rect;
	Vector2 prev_pivot;
	Vector2 prev_pivot_ratio;
	real_t prev_anchors[4] = { (real_t)0.0 };

	Transform2D pre_drag_xform;
	Rect2 pre_drag_rect;

	List<real_t> pre_drag_bones_length;
	List<Dictionary> pre_drag_bones_undo_state;

	Dictionary undo_state;
};

class HFlowContainer;

class CanvasItemEditor : public VBoxContainer {
	GDCLASS(CanvasItemEditor, VBoxContainer);

public:
	enum Tool {
		TOOL_SELECT,
		TOOL_SCENE_PAINT,
		TOOL_LIST_SELECT,
		TOOL_MOVE,
		TOOL_SCALE,
		TOOL_ROTATE,
		TOOL_EDIT_PIVOT,
		TOOL_PAN,
		TOOL_RULER,
		TOOL_MAX
	};

	enum AddNodeOption {
		ADD_NODE,
		ADD_INSTANCE,
		ADD_PASTE,
		ADD_MOVE,
	};

private:
	enum SnapTarget {
		SNAP_TARGET_NONE = 0,
		SNAP_TARGET_PARENT,
		SNAP_TARGET_SELF_ANCHORS,
		SNAP_TARGET_SELF,
		SNAP_TARGET_OTHER_NODE,
		SNAP_TARGET_GUIDE,
		SNAP_TARGET_GRID,
		SNAP_TARGET_PIXEL
	};

	enum MenuOption {
		SNAP_USE,
		SNAP_USE_NODE_PARENT,
		SNAP_USE_NODE_ANCHORS,
		SNAP_USE_NODE_SIDES,
		SNAP_USE_NODE_CENTER,
		SNAP_USE_OTHER_NODES,
		SNAP_USE_GRID,
		SNAP_USE_GUIDES,
		SNAP_USE_ROTATION,
		SNAP_USE_SCALE,
		SNAP_RELATIVE,
		SNAP_CONFIGURE,
		SNAP_USE_PIXEL,
		SHOW_HELPERS,
		SHOW_RULERS,
		SHOW_GUIDES,
		SHOW_ORIGIN,
		SHOW_VIEWPORT,
		SHOW_POSITION_GIZMOS,
		SHOW_LOCK_GIZMOS,
		SHOW_GROUP_GIZMOS,
		SHOW_TRANSFORMATION_GIZMOS,
		LOCK_SELECTED,
		UNLOCK_SELECTED,
		GROUP_SELECTED,
		UNGROUP_SELECTED,
		ANIM_INSERT_KEY,
		ANIM_INSERT_KEY_EXISTING,
		ANIM_INSERT_POS,
		ANIM_INSERT_ROT,
		ANIM_INSERT_SCALE,
		ANIM_COPY_POSE,
		ANIM_PASTE_POSE,
		ANIM_CLEAR_POSE,
		CLEAR_GUIDES,
		VIEW_CENTER_TO_SELECTION,
		VIEW_FRAME_TO_SELECTION,
		PREVIEW_CANVAS_SCALE,
		SKELETON_MAKE_BONES,
		SKELETON_SHOW_BONES,
		AUTO_RESAMPLE_CANVAS_ITEMS,
	};

	enum DragType {
		DRAG_NONE,
		DRAG_BOX_SELECTION,
		DRAG_LEFT,
		DRAG_TOP_LEFT,
		DRAG_TOP,
		DRAG_TOP_RIGHT,
		DRAG_RIGHT,
		DRAG_BOTTOM_RIGHT,
		DRAG_BOTTOM,
		DRAG_BOTTOM_LEFT,
		DRAG_ANCHOR_TOP_LEFT,
		DRAG_ANCHOR_TOP_RIGHT,
		DRAG_ANCHOR_BOTTOM_RIGHT,
		DRAG_ANCHOR_BOTTOM_LEFT,
		DRAG_ANCHOR_ALL,
		DRAG_QUEUED,
		DRAG_MOVE,
		DRAG_MOVE_X,
		DRAG_MOVE_Y,
		DRAG_SCALE_X,
		DRAG_SCALE_Y,
		DRAG_SCALE_BOTH,
		DRAG_ROTATE,
		DRAG_PIVOT,
		DRAG_TEMP_PIVOT,
		DRAG_V_GUIDE,
		DRAG_H_GUIDE,
		DRAG_DOUBLE_GUIDE,
		DRAG_KEY_MOVE
	};

	enum GridVisibility {
		GRID_VISIBILITY_SHOW,
		GRID_VISIBILITY_SHOW_WHEN_SNAPPING,
		GRID_VISIBILITY_HIDE,
	};

	enum TransformType {
		POSITION,
		ROTATION,
		SCALE,
	};

	const String locked_transform_warning = TTRC("All selected CanvasItems are either invisible or locked in some way and can't be transformed.");

	bool selection_menu_additive_selection = false;

	Tool tool = TOOL_SELECT;
	bool tree_signals_connected = false; // Reparent-tolerance: connect permanent signals only on first ENTER_TREE.

	// Step⑤b.4a: the instanceable 2D VIEW (display stack + pan/zoom) is owned by this separate
	// class, mirroring Node3DEditor/Node3DEditorView. CanvasItemEditor keeps the shared SERVICES
	// (tool state, snap engine, toolbar, dialogs). main_view is the legacy main-screen surface;
	// active_view is the focused pane surface (falling back to main_view when no 2D pane is active).
	CanvasItemEditorView *main_view = nullptr;
	CanvasItemEditorView *active_view = nullptr;
	LocalVector<CanvasItemEditorView *> editor_views;
	CanvasItemEditorView *_get_active_view() const { return active_view ? active_view : main_view; }

	// G2 M7.2a: the full toolbar flow, reparented into the focused 2D scene pane's header
	// (get_shared_toolbar/park_shared_toolbar). toolbar_home is its stock parent.
	HFlowContainer *main_flow = nullptr;
	Node *toolbar_home = nullptr;

	// Used for secondary menu items which are displayed depending on the currently selected node
	// (such as MeshInstance's "Mesh" menu).
	PanelContainer *context_toolbar_panel = nullptr;
	HBoxContainer *context_toolbar_hbox = nullptr;
	HashMap<Control *, VSeparator *> context_toolbar_separators;

	void _update_context_toolbar();

	// Step⑤b.4b: grid_visibility + the nine show_* toggles now live on CanvasItemEditorView.

	bool auto_resampling_enabled = true;
	real_t resample_delay = 0.3;

	bool selected_from_canvas = false;
	bool had_visible_selection = false;

	// Defaults are defined in clear().
	Point2 grid_offset;
	Point2 grid_step;
	Vector2i primary_grid_step;
	int grid_step_multiplier = 0;

	Color selection_rectangle_color;
	Color locked_selection_rectangle_color;

	real_t snap_rotation_step = 0.0;
	real_t snap_rotation_offset = 0.0;
	real_t snap_scale_step = 0.0;
	bool use_local_space = true;
	bool smart_snap_active = false;
	bool grid_snap_active = false;

	bool snap_node_parent = true;
	bool snap_node_anchors = true;
	bool snap_node_sides = true;
	bool snap_node_center = true;
	bool snap_other_nodes = true;
	bool snap_guides = true;
	bool snap_rotation = false;
	bool snap_scale = false;
	bool snap_relative = false;
	// Enable pixel snapping even if pixel snap rendering is disabled in the Project Settings.
	// This results in crisper visuals by preventing 2D nodes from being placed at subpixel coordinates.
	bool snap_pixel = true;

	bool key_pos = true;
	bool key_rot = true;
	bool key_scale = false;

	// Step⑤b.4c: temp_pivot / ruler_tool_active / ruler_tool_origin now live on CanvasItemEditorView.
	// Step⑤b.4b: ruler_width_scaled / ruler_font_size now live on CanvasItemEditorView.
	Point2 node_create_position;
	real_t grab_distance = 0.0;
	bool simple_panning = false;

	MenuOption last_option = SNAP_USE;

public:
	struct SelectResult {
		CanvasItem *item = nullptr;
		real_t z_index = 0;
		bool has_z = true;
		_FORCE_INLINE_ bool operator<(const SelectResult &p_rr) const {
			return has_z && p_rr.has_z ? p_rr.z_index < z_index : p_rr.has_z;
		}
	};

private:
	// Step⑤b.4c: selection_results (drag-time cache) moved to CanvasItemEditorView; the
	// selection_results_menu snapshot stays here (consumed by the editor's popup handlers).
	Vector<SelectResult> selection_results_menu;

	struct _HoverResult {
		Point2 position;
		Ref<Texture2D> icon;
		String name;
	};
	// Step⑤b.4c: hovering_results moved to CanvasItemEditorView.

	struct BoneList {
		Transform2D xform;
		real_t length = 0;
		uint64_t last_pass = 0;
	};

	uint64_t bone_last_frame = 0;

	struct BoneKey {
		ObjectID from;
		ObjectID to;
		_FORCE_INLINE_ bool operator<(const BoneKey &p_key) const {
			if (from == p_key.from) {
				return to < p_key.to;
			} else {
				return from < p_key.from;
			}
		}
	};

	HashMap<BoneKey, BoneList> bone_list;
	MenuButton *skeleton_menu = nullptr;

	struct PoseClipboard {
		Vector2 pos;
		Vector2 scale;
		real_t rot = 0;
		ObjectID id;
	};
	List<PoseClipboard> pose_clipboard;

	Button *select_button = nullptr;

	Button *move_button = nullptr;
	Button *scene_paint_button = nullptr;
	Button *scale_button = nullptr;
	Button *rotate_button = nullptr;

	Button *list_select_button = nullptr;
	Button *pivot_button = nullptr;
	Button *pan_button = nullptr;

	Button *ruler_button = nullptr;

	Button *local_space_button = nullptr;
	Button *smart_snap_button = nullptr;
	Button *grid_snap_button = nullptr;
	MenuButton *snap_config_menu = nullptr;
	PopupMenu *smartsnap_config_popup = nullptr;

	Button *lock_button = nullptr;
	Button *unlock_button = nullptr;

	Button *group_button = nullptr;
	Button *ungroup_button = nullptr;
	Button *scene_view_button_2d = nullptr;
	Button *scene_view_button_3d = nullptr;
	void _scene_view_button_pressed(bool p_2d);

	MenuButton *view_menu = nullptr;
	EditorSceneActionsMenu *scene_actions_menu = nullptr;
	PopupMenu *grid_menu = nullptr;
	PopupMenu *theme_menu = nullptr;
	PopupMenu *gizmos_menu = nullptr;
	HBoxContainer *animation_hb = nullptr;
	MenuButton *animation_menu = nullptr;

	Button *key_loc_button = nullptr;
	Button *key_rot_button = nullptr;
	Button *key_scale_button = nullptr;
	Button *key_insert_button = nullptr;
	Button *key_auto_insert_button = nullptr;

	PopupMenu *selection_menu = nullptr;
	PopupMenu *add_node_menu = nullptr;

	// Step⑤b.4c: the full drag/input machinery (drag_type, drag_from/to, drag_selection,
	// dragged_guide_*, is_hovering_*_guide, box_selecting_to, original_transform,
	// cursor_shape_override, etc.) now lives on CanvasItemEditorView.
	bool updating_value_dialog = false;

	Ref<StyleBoxTexture> select_sb;
	Ref<Texture2D> select_handle;
	Ref<Texture2D> anchor_handle;

	Ref<Shortcut> drag_pivot_shortcut;
	Ref<Shortcut> set_pivot_shortcut;
	Ref<Shortcut> multiply_grid_step_shortcut;
	Ref<Shortcut> divide_grid_step_shortcut;
	Ref<Shortcut> reset_transform_position_shortcut;
	Ref<Shortcut> reset_transform_rotation_shortcut;
	Ref<Shortcut> reset_transform_scale_shortcut;

	bool _is_node_locked(const Node *p_node) const;
	bool _is_node_movable(const Node *p_node, bool p_popup_warning = false);
	void _find_canvas_items_in_rect(const Rect2 &p_rect, Node *p_node, List<CanvasItem *> *r_items, const Transform2D &p_parent_xform = Transform2D(), const Transform2D &p_canvas_xform = Transform2D());
	// Step⑤b.4c: _get_canvas_items_at_pos / _select_click_on_item (viewport-space wrappers) moved to CanvasItemEditorView.

	ConfirmationDialog *snap_dialog = nullptr;

	CanvasItem *ref_item = nullptr;

	void _save_canvas_item_state(const List<CanvasItem *> &p_canvas_items, bool save_bones = false);
	void _restore_canvas_item_state(const List<CanvasItem *> &p_canvas_items, bool restore_bones = false);
	void _commit_canvas_item_state(const List<CanvasItem *> &p_canvas_items, const String &action_name, bool commit_bones = false);

	Vector2 _anchor_to_position(const Control *p_control, Vector2 anchor);
	Vector2 _position_to_anchor(const Control *p_control, Vector2 position);

	void _prepare_view_menu();
	void _popup_callback(int p_op);
	void _snap_changed();
	void _selection_result_pressed(int);
	void _selection_menu_hide();
	void _add_node_pressed(int p_result);
	void _adjust_new_node_position(Node *p_node);
	void _reset_create_position();
	void _update_editor_settings();
	void _prepare_grid_menu();
	void _on_grid_menu_id_pressed(int p_id);
	void _reset_transform(TransformType p_type);

public:
	enum ThemePreviewMode {
		THEME_PREVIEW_PROJECT,
		THEME_PREVIEW_EDITOR,
		THEME_PREVIEW_DEFAULT,

		THEME_PREVIEW_MAX // The number of options for enumerating.
	};

private:
	ThemePreviewMode theme_preview = THEME_PREVIEW_PROJECT;
	void _switch_theme_preview(int p_mode);

	List<CanvasItem *> _get_edited_canvas_items(bool p_retrieve_locked = false, bool p_remove_canvas_item_if_parent_in_selection = true, bool *r_has_locked_items = nullptr) const;
	Rect2 _get_encompassing_rect_from_list(const List<CanvasItem *> &p_list);
	void _expand_encompassing_rect_using_children(Rect2 &r_rect, const Node *p_node, bool &r_first, const Transform2D &p_parent_xform = Transform2D(), const Transform2D &p_canvas_xform = Transform2D(), bool include_locked_nodes = true);
	Rect2 _get_encompassing_rect(const Node *p_node);

	Object *_get_editor_data(Object *p_what);

	void _insert_animation_keys(bool p_location, bool p_rotation, bool p_scale, bool p_on_existing);

	void _keying_changed();

	virtual void shortcut_input(const Ref<InputEvent> &p_ev) override;

	// Step⑤b.4b: the overlay draw path (_draw_viewport + the whole _draw_* family) now lives on
	// CanvasItemEditorView.
	// Step⑤b.4c: the input/drag machinery (_gui_input_viewport + full chain, _commit_drag,
	// _reset_drag, _update_cursor, get_cursor_shape logic) now lives on CanvasItemEditorView too.

	void _update_lock_and_group_button();

	void _selection_changed();
	void _focus_selection(int p_op);

	void _project_settings_changed();

	SnapTarget snap_target[2];
	Transform2D snap_transform;
	void _snap_if_closer_float(
			const real_t p_value,
			real_t &r_current_snap, SnapTarget &r_current_snap_target,
			const real_t p_target_value, const SnapTarget p_snap_target,
			const real_t p_radius = 10.0);
	void _snap_if_closer_point(
			Point2 p_value,
			Point2 &r_current_snap, SnapTarget (&r_current_snap_target)[2],
			Point2 p_target_value, const SnapTarget p_snap_target,
			const real_t rotation = 0.0,
			const real_t p_radius = 10.0);
	void _snap_other_nodes(
			const Point2 p_value,
			const Transform2D p_transform_to_snap,
			Point2 &r_current_snap, SnapTarget (&r_current_snap_target)[2],
			const SnapTarget p_snap_target, List<const CanvasItem *> p_exceptions,
			const Node *p_current);

	void _button_toggle_local_space(bool p_status);
	void _button_toggle_smart_snap(bool p_status);
	void _button_toggle_grid_snap(bool p_status);
	void _button_tool_select(int p_index);

	HSplitContainer *left_panel_split = nullptr;
	HSplitContainer *right_panel_split = nullptr;
	VSplitContainer *bottom_split = nullptr;

	void _set_owner_for_node_and_children(Node *p_node, Node *p_owner);

	friend class CanvasItemEditorPlugin;
	friend class CanvasItemEditorView; // Step⑤b.4 promissory note: the view reads editor-owned service state.

protected:
	void _notification(int p_what);

	static void _bind_methods();

	static CanvasItemEditor *singleton;

public:
	enum SnapMode {
		SNAP_GRID = 1 << 0,
		SNAP_GUIDES = 1 << 1,
		SNAP_PIXEL = 1 << 2,
		SNAP_NODE_PARENT = 1 << 3,
		SNAP_NODE_ANCHORS = 1 << 4,
		SNAP_NODE_SIDES = 1 << 5,
		SNAP_NODE_CENTER = 1 << 6,
		SNAP_OTHER_NODES = 1 << 7,

		SNAP_DEFAULT = SNAP_GRID | SNAP_GUIDES | SNAP_PIXEL,
	};

	String message;

	Point2 snap_point(Point2 p_target, unsigned int p_modes = SNAP_DEFAULT, unsigned int p_forced_modes = 0, const CanvasItem *p_self_canvas_item = nullptr, const List<CanvasItem *> &p_other_nodes_exceptions = List<CanvasItem *>());
	real_t snap_angle(real_t p_target, real_t p_start = 0) const;

	Transform2D get_canvas_transform() const;

	static CanvasItemEditor *get_singleton() { return singleton; }
	Dictionary get_state() const;
	void set_state(const Dictionary &p_state);
	void clear();

	void add_control_to_menu_panel(Control *p_control);
	void remove_control_from_menu_panel(Control *p_control);

	void add_control_to_left_panel(Control *p_control);
	void remove_control_from_left_panel(Control *p_control);

	void add_control_to_right_panel(Control *p_control);
	void remove_control_from_right_panel(Control *p_control);

	VSplitContainer *get_bottom_split();

	Control *get_viewport_control();
	// A document scene_root stays parked under DocumentsHolder while pane viewports render its
	// World2D. Treat that source viewport as visible to editor overlays, while retaining the stock
	// visibility rule for nested SubViewports.
	bool is_viewport_visible_for_editing(const Viewport *p_viewport) const;

	// Step⑤b.4d: mint a per-pane 2D view bound to p_document's isolated World2D (the 2D analog of
	// Node3DEditor::create_view_bound_to). DocumentView hosts the returned Control; it renders
	// p_document independently and edits only while p_document is the active edited scene.
	Control *create_view_bound_to(EditorDocument *p_document);

	// G2 M7.2a: the shared 2D toolbar follows the focused scene pane.
	Control *get_shared_toolbar() const;
	void park_shared_toolbar();
	// G2 M7.2a-fix: re-point the toolbar's shortcut contexts at the focused pane (null resets to this).
	void set_toolbar_shortcut_context(Node *p_context);
	void set_scene_view_button_state(bool p_2d);

	// ⑤c: bind the main view to p_document (the active document) so it renders + edits that
	// document through its own world-bound viewport. Called by EditorNode on every scene switch.
	void activate_document(EditorDocument *p_document);

	Control *get_controls_container();

	void find_canvas_items_at_pos(const Point2 &p_pos, Node *p_node, Vector<SelectResult> &r_items, const Transform2D &p_parent_xform = Transform2D(), const Transform2D &p_canvas_xform = Transform2D());

	// Redraw every live 2D view. Hidden/background views self-gate their edit overlays; fan-out keeps
	// tree-driven selection and node changes synchronized with the focused pane.
	void update_viewport();

	Tool get_current_tool() { return tool; }
	void set_current_tool(Tool p_tool);

	bool is_grid_visible() const;
	Vector2 get_grid_step() const { return grid_step; }

	void edit(CanvasItem *p_canvas_item);

	void focus_selection();
	void center_at(const Point2 &p_pos);

	void set_cursor_shape_override(CursorShape p_shape = CURSOR_ARROW);
	virtual CursorShape get_cursor_shape(const Point2 &p_pos) const override;

	ThemePreviewMode get_theme_preview() const { return theme_preview; }

	EditorSelection *editor_selection = nullptr;

	CanvasItemEditor();
};

// Step⑤b.4a: instanceable per-pane 2D view. Mirrors Node3DEditorView: owns the display stack
// (viewport overlay + scene container + scrollbars + zoom widget) and the pan/zoom transform
// state. CanvasItemEditor (services) owns one as main_view and reaches into it via friendship.
// The overlay's draw/gui_input still target the editor's handlers (moved in Phases 2-3).
class CanvasItemEditorView : public Control {
	GDCLASS(CanvasItemEditorView, Control);
	friend class CanvasItemEditor; // Step⑤b.4 promissory note: services reads/writes this view's display + pan/zoom state.

	CanvasItemEditor *editor = nullptr; // Services singleton this view belongs to.

	// Step⑤b.4d: document binding. null = "shim mode": this view displays the globally-active
	// document via the shared scene_root (the main_view). Non-null = a per-pane view minted by
	// create_view_bound_to(): it renders p_document through its OWN view_viewport (bound to the
	// document's World2D) and edits only while p_document is the active edited scene.
	EditorDocument *document = nullptr;
	SubViewport *view_viewport = nullptr; // Child viewport bound to document's World2D (document != null).

	Control *viewport = nullptr;
	EditorViewportChrome *viewport_chrome = nullptr;
	Control *viewport_scrollable = nullptr;
	SubViewportContainer *scene_view_container = nullptr; // Displays the active document's scene_root SubViewport.

	HScrollBar *h_scroll = nullptr;
	VScrollBar *v_scroll = nullptr;

	VBoxContainer *controls_vb = nullptr;
	Button *button_center_view = nullptr;
	EditorZoomWidget *zoom_widget = nullptr;

	Transform2D transform;
	real_t zoom = 1.0;
	Point2 view_offset;
	Point2 previous_update_view_offset;
	bool updating_scroll = false;

	Ref<ViewPanner> panner;
	bool pan_pressed = false;
	Timer *resample_timer = nullptr;

	void _pan_callback(Vector2 p_scroll_vec, Ref<InputEvent> p_event);
	void _zoom_callback(float p_zoom_factor, Vector2 p_origin, Ref<InputEvent> p_event);
	void _update_scroll(real_t);
	void _update_scrollbars();
	void _update_oversampling();
	void _update_zoom(real_t p_zoom);
	void _shortcut_zoom_set(real_t p_zoom);
	void _zoom_on_position(real_t p_zoom, Point2 p_position = Point2());

	// Step⑤b.4b: per-view visibility toggles (moved from the editor). The overlay draw path below
	// reads these directly; the editor's menu/state code reaches them via friendship.
	CanvasItemEditor::GridVisibility grid_visibility = CanvasItemEditor::GRID_VISIBILITY_SHOW_WHEN_SNAPPING;
	bool show_rulers = true;
	bool show_guides = true;
	bool show_origin = true;
	bool show_viewport = true;
	bool show_helpers = false;
	bool show_position_gizmos = true;
	bool show_lock_gizmos = true;
	bool show_group_gizmos = true;
	bool show_transformation_gizmos = true;

	real_t ruler_width_scaled = 16.0;
	int ruler_font_size = 8;

	// Step⑤b.4c: the drag/input machinery (moved from the editor). The overlay's gui_input targets
	// _gui_input_viewport below; the editor reaches this state via friendship for the few remaining
	// service-side touch points (undo save/restore, popup handlers, focus-out/tool-switch commits).
	Point2 drag_start_origin;
	CanvasItemEditor::DragType drag_type = CanvasItemEditor::DRAG_NONE;
	Point2 drag_from;
	Point2 drag_to;
	Point2 drag_rotation_center;
	List<CanvasItem *> drag_selection;
	int dragged_guide_index = -1;
	Point2 dragged_guide_pos;
	bool is_hovering_h_guide = false;
	bool is_hovering_v_guide = false;
	Transform2D original_transform;
	Point2 box_selecting_to;
	CursorShape cursor_shape_override = CURSOR_ARROW;
	Vector2 temp_pivot = Vector2(Math::INF, Math::INF);
	bool ruler_tool_active = false;
	Point2 ruler_tool_origin;
	Vector<CanvasItemEditor::SelectResult> selection_results;
	Vector<CanvasItemEditor::_HoverResult> hovering_results;

	// Step⑤b.4b: the overlay draw path (moved from the editor). Service/selection/scene reads are
	// routed through `editor->`; the view's own display + pan/zoom state is read directly.
	// Transform-sink seam: _draw_viewport rebuilds `transform` and pushes it via
	// _update_display_transform() onto _get_transform_sink() so only the view that displays the
	// sink writes the shared global_canvas_transform (Phase 4 makes the sink the view's viewport).
	void _update_display_transform();
	Viewport *_get_transform_sink();

	// Step⑤b.4d: document-bound helpers (ported from CanvasView2D). _is_active_document() is true
	// when this view's document is the editor's active edited scene; _ensure_active() makes it so;
	// _edits_gated() is the single view-many/edit-active gate shared by the input and draw paths.
	bool _is_active_document() const;
	void _ensure_active();
	bool _edits_gated() const;

	// Recompute the settings/theme-derived ruler caches. Called from ENTER_TREE and (per-view) from
	// CanvasItemEditor::_update_editor_settings(), so the derivation lives in one place.
	void _recompute_ruler_metrics();

	void _draw_text_at_position(Point2 p_position, const String &p_string, Side p_side);
	void _draw_margin_at_position(int p_value, Point2 p_position, Side p_side);
	void _draw_percentage_at_position(real_t p_value, Point2 p_position, Side p_side);
	void _draw_straight_line(Point2 p_from, Point2 p_to, Color p_color);

	void _draw_smart_snapping();
	void _draw_rulers();
	void _draw_guides();
	void _draw_focus();
	void _draw_grid();
	void _draw_ruler_tool();
	void _draw_control_anchors(Control *control);
	void _draw_control_helpers(Control *control);
	void _draw_selection();
	void _draw_axis();
	void _draw_invisible_nodes_positions(Node *p_node, const Transform2D &p_parent_xform = Transform2D(), const Transform2D &p_canvas_xform = Transform2D());
	void _draw_locks_and_groups(Node *p_node, const Transform2D &p_parent_xform = Transform2D(), const Transform2D &p_canvas_xform = Transform2D());
	void _draw_hover();
	void _draw_message();

	void _draw_viewport();

	// Step⑤b.4c: the input/drag machinery (moved from the editor). Service reads (snap/undo/menus/
	// selection/tool) are routed through `editor->`; view-owned drag + display state is read directly.
	void _get_canvas_items_at_pos(const Point2 &p_pos, Vector<CanvasItemEditor::SelectResult> &r_items, bool p_allow_locked = false);
	bool _select_click_on_item(CanvasItem *item, Point2 p_click_pos, bool p_append);

	bool _gui_input_anchors(const Ref<InputEvent> &p_event);
	bool _gui_input_move(const Ref<InputEvent> &p_event);
	bool _gui_input_open_scene_on_double_click(const Ref<InputEvent> &p_event);
	bool _gui_input_scale(const Ref<InputEvent> &p_event);
	bool _gui_input_pivot(const Ref<InputEvent> &p_event);
	bool _gui_input_resize(const Ref<InputEvent> &p_event);
	bool _gui_input_rotate(const Ref<InputEvent> &p_event);
	bool _gui_input_select(const Ref<InputEvent> &p_event);
	bool _gui_input_ruler_tool(const Ref<InputEvent> &p_event);
	bool _gui_input_zoom_or_pan(const Ref<InputEvent> &p_event, bool p_already_accepted);
	bool _gui_input_rulers_and_guides(const Ref<InputEvent> &p_event);
	bool _gui_input_hover(const Ref<InputEvent> &p_event);

	void _commit_drag();
	void _reset_drag();
	void _gui_input_viewport(const Ref<InputEvent> &p_event);
	void _update_cursor();

protected:
	void _notification(int p_what);
	static void _bind_methods() {}

public:
	// Step⑤b.4d: bind this view to p_document -- build its own SubViewport pointed at the
	// document's World2D so the pane renders that document independently (mirrors
	// Node3DEditorView::set_active_world). Called by create_view_bound_to() before the view enters
	// the tree; main_view stays unbound (document == null).
	void bind_document(EditorDocument *p_document);
	void set_context_active(bool p_active);

	void update_viewport();
	Transform2D get_canvas_transform() const { return transform; }
	SubViewport *get_scene_viewport() const { return view_viewport; }
	Control *get_overlay_control() const { return viewport; }
	Control *get_controls_container() const { return controls_vb; }

	// Step⑤b.4c: drag-lifecycle forwarders used by the editor's focus-out / tool-switch handlers.
	void commit_drag_if_any();
	void cancel_drag();

	void set_cursor_shape_override(CursorShape p_shape = CURSOR_ARROW);
	virtual CursorShape get_cursor_shape(const Point2 &p_pos) const override;

	CanvasItemEditorView(CanvasItemEditor *p_editor);
	~CanvasItemEditorView();
};

class CanvasItemEditorPlugin : public EditorPlugin {
	GDCLASS(CanvasItemEditorPlugin, EditorPlugin);

	CanvasItemEditor *canvas_item_editor = nullptr;

protected:
	void _notification(int p_what);

public:
	virtual String get_plugin_name() const override { return TTRC("2D"); }
	bool has_main_screen() const override { return true; }
	virtual void edit(Object *p_object) override;
	virtual bool handles(Object *p_object) const override;
	virtual void make_visible(bool p_visible) override;
	virtual Dictionary get_state() const override;
	virtual void set_state(const Dictionary &p_state) override;
	virtual void clear() override;

	CanvasItemEditor *get_canvas_item_editor() { return canvas_item_editor; }

	CanvasItemEditorPlugin();
};

class CanvasItemEditorViewport : public Control {
	GDCLASS(CanvasItemEditorViewport, Control);

	// The type of node that will be created when dropping texture into the viewport.
	String default_texture_node_type;
	// Node types that are available to select from when dropping texture into viewport.
	Vector<String> texture_node_types;

	Vector<String> selected_files;
	Node *target_node = nullptr;
	Point2 drop_pos;

	CanvasItemEditor *canvas_item_editor = nullptr;
	Control *preview_node = nullptr;
	AcceptDialog *accept = nullptr;
	AcceptDialog *texture_node_type_selector = nullptr;
	RichTextLabel *tooltip_panel = nullptr;
	Ref<ButtonGroup> button_group;

	void _on_mouse_exit();
	void _on_select_texture_node_type(Object *selected);
	void _on_change_type_confirmed();
	void _on_change_type_closed();

	void _create_preview(const Vector<String> &files) const;
	void _remove_preview();

	bool _cyclical_dependency_exists(const String &p_target_scene_path, Node *p_desired_node) const;
	bool _is_any_texture_selected() const;
	void _add_node_to_scene(Node *p_parent, Node *p_child, const Vector2 &p_target_position);
	void _create_texture_node(Node *p_parent, Node *p_child, const String &p_path, const Point2 &p_point);
	void _create_audio_node(Node *p_parent, const String &p_path, const Point2 &p_point);
	bool _create_instance(Node *p_parent, const String &p_path, const Point2 &p_point);
	void _create_mesh_node(Node *p_parent, const String &p_path, const Point2 &p_point);
	void _perform_drop_data();
	void _show_texture_node_type_selector();
	void _update_theme();

	void _show_tooltip(const String &p_title, const String &p_description) const;

protected:
	void _notification(int p_what);

public:
	virtual bool can_drop_data(const Point2 &p_point, const Variant &p_data) const override;
	virtual void drop_data(const Point2 &p_point, const Variant &p_data) override;

	CanvasItemEditorViewport(CanvasItemEditor *p_canvas_item_editor, Control *p_controls_container);
	~CanvasItemEditorViewport();
};
