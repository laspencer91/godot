/**************************************************************************/
/*  node_3d_editor_plugin.h                                               */
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

#include "core/math/dynamic_bvh.h"
#include "core/templates/hash_map.h"
#include "editor/plugins/editor_plugin.h"
#include "editor/scene/3d/node_3d_editor_gizmos.h"
#include "scene/debugger/view_3d_controller.h"
#include "scene/gui/box_container.h"
#include "scene/gui/margin_container.h"
#include "scene/gui/popup.h"

class AcceptDialog;
class Button;
class ColorPickerButton;
class ConfirmationDialog;
class DirectionalLight3D;
class EditorDocument;
class EditorSelection;
class EditorSpinSlider;
class HSplitContainer;
class LineEdit;
class MenuButton;
class Node3DEditorViewport;
class Node3DEditorViewportContainer;
class OptionButton;
class PanelContainer;
class PhysicsDirectSpaceState3D;
class ProceduralSkyMaterial;
class SpinBox;
class World3D;
class VSeparator;
class VSplitContainer;
class WorldEnvironment;

class Node3DEditorView;
class HFlowContainer;

class Node3DEditor : public VBoxContainer {
	GDCLASS(Node3DEditor, VBoxContainer);

	friend class Node3DEditorView; // G2 M7.2: the view (de)registers itself in editor_views.

public:
	static const unsigned int VIEWPORTS_COUNT = 4;

	enum ToolMode {
		TOOL_MODE_TRANSFORM,
		TOOL_MODE_MOVE,
		TOOL_MODE_ROTATE,
		TOOL_MODE_SCALE,
		TOOL_MODE_SELECT,
		TOOL_MODE_LIST_SELECT,
		TOOL_LOCK_SELECTED,
		TOOL_UNLOCK_SELECTED,
		TOOL_GROUP_SELECTED,
		TOOL_UNGROUP_SELECTED,
		TOOL_RULER,
		TOOL_MAX
	};

	enum ToolOptions {
		TOOL_OPT_LOCAL_COORDS,
		TOOL_OPT_USE_SNAP,
		TOOL_OPT_USE_TRACKBALL,
		TOOL_OPT_PRESERVE_CHILDREN_TRANSFORM,
		TOOL_OPT_MAX
	};

	enum TransformMode {
		TRANSFORM_MODE_GLOBAL = 1,
		TRANSFORM_MODE_LOCAL = 2,
	};

	real_t gizmo_view_rotation_scale = 1.0;

private:
	EditorSelection *editor_selection = nullptr;

	// G2/Step③: the instanceable 3D VIEW (quad of viewports) is owned by this separate
	// class; Node3DEditor keeps the shared SERVICES (tool state, gizmo registry, toolbar).
	// v1: exactly one, created in the ctor; later: one per workspace pane. The viewport
	// accessors below forward to it. viewport_base/viewports/last_used/freelook moved here.
	Node3DEditorView *main_view = nullptr;

	// G2 M7.2a: the full toolbar flow (main_menu_hbox + contextual toolbars). Reparented into the
	// focused scene pane's header via get_shared_toolbar()/park_shared_toolbar(); toolbar_home is its
	// stock parent (the toolbar margin inside this editor's own vbox).
	HFlowContainer *main_flow = nullptr;
	Node *toolbar_home = nullptr;

	// G2 M7.2: every live Node3DEditorView — the singleton main_view plus every per-pane view minted
	// by create_view_bound_to (registered in the view ctor, removed in its dtor). update_transform_gizmo
	// drives gizmo rendering across all of them (hidden viewports self-skip), so a selected node's
	// transform gizmo shows in the focused pane, not just the main view.
	LocalVector<Node3DEditorView *> editor_views;

	VSplitContainer *shader_split = nullptr;
	HSplitContainer *left_panel_split = nullptr;
	HSplitContainer *right_panel_split = nullptr;

	/////

	ToolMode tool_mode = TOOL_MODE_TRANSFORM;

	// Reparent-tolerance (G2): the gizmo-plugin registry and grid/origin indicators are
	// one-time setup that stock Godot builds on the single ENTER_TREE. The workspace moves
	// this control between panes, so guard both against re-running, and free the indicators
	// in the destructor instead of on EXIT_TREE (they persist across reparenting).
	bool gizmos_registered = false;
	bool indicators_initialized = false;

	// G2: PER-WORLD freelists over the transform-gizmo cull-mask layers (GIZMO_BASE_LAYER..31,
	// max 5). A cull-mask layer only needs to be unique among viewports rendering the SAME
	// world (scenario) — across documents, separate scenarios make layer reuse safe for free.
	// So the budget is "5 views of the same document" (never hit) instead of "5 panes total"
	// (a dual-monitor user hits it). Keyed on the world's scenario RID id; one bit per offset
	// 0..4; empty buckets are removed on free. See ARCHITECTURE.md seam rule #4.
	HashMap<uint64_t, uint32_t> gizmo_layer_used_masks;

	Ref<ArrayMesh> move_gizmo[3], move_plane_gizmo[3], rotate_gizmo[4], scale_gizmo[3], scale_plane_gizmo[3], axis_gizmo[3];
	Ref<ArrayMesh> trackball_sphere_gizmo;
	Ref<StandardMaterial3D> gizmo_color[3];
	Ref<StandardMaterial3D> plane_gizmo_color[3];
	Ref<ShaderMaterial> rotate_gizmo_color[4];
	Ref<StandardMaterial3D> gizmo_color_hl[3];
	Ref<StandardMaterial3D> plane_gizmo_color_hl[3];
	Ref<ShaderMaterial> rotate_gizmo_color_hl[4];
	Ref<StandardMaterial3D> trackball_sphere_material;
	Ref<StandardMaterial3D> trackball_sphere_material_hl;

	Ref<Node3DGizmo> current_hover_gizmo;
	int current_hover_gizmo_handle;
	bool current_hover_gizmo_handle_secondary;

	DynamicBVH gizmo_bvh;

	real_t snap_translate_value = 0;
	real_t snap_rotate_value = 0;
	real_t snap_scale_value = 0;

	Ref<ArrayMesh> active_selection_box_xray;
	Ref<ArrayMesh> active_selection_box;
	Ref<ArrayMesh> selection_box_xray;
	Ref<ArrayMesh> selection_box;

	Ref<StandardMaterial3D> selection_box_mat;
	Ref<StandardMaterial3D> selection_box_mat_xray;
	Ref<StandardMaterial3D> active_selection_box_mat;
	Ref<StandardMaterial3D> active_selection_box_mat_xray;

	RID indicators;
	RID indicators_instance;
	RID cursor_mesh;
	RID cursor_instance;
	Ref<StandardMaterial3D> cursor_material;

	// Scene drag and drop support
	Node3D *preview_node = nullptr;
	AABB preview_bounds;

	Ref<Material> preview_material;
	Ref<Material> preview_reset_material;
	ObjectID preview_material_target;
	int preview_material_surface = -1;

	struct Gizmo {
		bool visible = false;
		real_t scale = 0;
		Transform3D transform;
	} gizmo;

	enum MenuOption {
		MENU_TOOL_TRANSFORM,
		MENU_TOOL_MOVE,
		MENU_TOOL_ROTATE,
		MENU_TOOL_SCALE,
		MENU_TOOL_SELECT,
		MENU_TOOL_LIST_SELECT,
		MENU_TOOL_LOCAL_COORDS,
		MENU_TOOL_USE_SNAP,
		MENU_TOOL_USE_TRACKBALL,
		MENU_TOOL_PRESERVE_CHILDREN_TRANSFORM,
		MENU_TRANSFORM_CONFIGURE_SNAP,
		MENU_TRANSFORM_DIALOG,
		MENU_VIEW_USE_1_VIEWPORT,
		MENU_VIEW_USE_2_VIEWPORTS,
		MENU_VIEW_USE_2_VIEWPORTS_ALT,
		MENU_VIEW_USE_3_VIEWPORTS,
		MENU_VIEW_USE_3_VIEWPORTS_ALT,
		MENU_VIEW_USE_4_VIEWPORTS,
		MENU_VIEW_ORIGIN,
		MENU_VIEW_GRID,
		MENU_VIEW_GIZMOS_3D_ICONS,
		MENU_VIEW_CAMERA_SETTINGS,
		MENU_LOCK_SELECTED,
		MENU_UNLOCK_SELECTED,
		MENU_GROUP_SELECTED,
		MENU_UNGROUP_SELECTED,
		MENU_SNAP_TO_FLOOR,
		MENU_RULER,
		MENU_VERTEX_SNAP_BASE_VERTEX,
		MENU_VERTEX_SNAP_BASE_ORIGIN,
		MENU_VERTEX_SNAP_SOURCE_MESH,
		MENU_VERTEX_SNAP_SOURCE_COLLISION,
	};

	Button *tool_button[TOOL_MAX];
	Button *tool_option_button[TOOL_OPT_MAX];
	Button *scene_view_button_2d = nullptr;
	Button *scene_view_button_3d = nullptr;
	void _scene_view_button_pressed(bool p_2d);

	MenuButton *transform_menu = nullptr;
	PopupMenu *gizmos_menu = nullptr;
	MenuButton *view_layout_menu = nullptr;

	AcceptDialog *accept = nullptr;

	ConfirmationDialog *snap_dialog = nullptr;
	ConfirmationDialog *xform_dialog = nullptr;
	ConfirmationDialog *settings_dialog = nullptr;

	bool snap_enabled = false;
	bool snap_key_enabled = false;
	bool vertex_snap_origin_mode = false;
	bool vertex_snap_use_collision = false;
	EditorSpinSlider *snap_translate = nullptr;
	EditorSpinSlider *snap_rotate = nullptr;
	EditorSpinSlider *snap_scale = nullptr;

	bool trackball_enabled = false;

	LineEdit *xform_translate[3];
	LineEdit *xform_rotate[3];
	LineEdit *xform_scale[3];
	OptionButton *xform_type = nullptr;

	VBoxContainer *settings_vbc = nullptr;
	SpinBox *settings_fov = nullptr;
	SpinBox *settings_znear = nullptr;
	SpinBox *settings_zfar = nullptr;

	void _snap_changed();
	void _snap_update();
	void _update_vertex_snap_tooltips();
	void _xform_dialog_action();
	void _menu_item_pressed(int p_option);
	void _update_view_layout_menu_checkmarks(int p_checked_option);
	void _set_selection_meta_flag(const String &p_action_name, const String &p_meta_name, const String &p_signal_name, bool p_enable, bool p_update_transform_gizmo = false);
	void _menu_item_toggled(bool pressed, int p_option);
	void _menu_gizmo_toggled(int p_option);
	// Used for secondary menu items which are displayed depending on the currently selected node
	// (such as MeshInstance's "Mesh" menu).
	PanelContainer *context_toolbar_panel = nullptr;
	HBoxContainer *context_toolbar_hbox = nullptr;
	VSeparator *context_toolbar_divider = nullptr; // G2: tools/context divider in the inline main tool row.
	HashMap<Control *, VSeparator *> context_toolbar_separators;

	void _update_context_toolbar();

	void _generate_selection_boxes();

	void _init_indicators();
	void _set_gizmos_menu_item_icon(int p_idx, int p_state);
	void _update_gizmos_menu();
	void _update_gizmos_menu_theme();
	void _finish_indicators();

	void _toggle_maximize_view(Object *p_viewport);
	void _viewport_clicked(int p_viewport_idx);

	// Build this view's quad of viewports (wire signals, inject the ctor-scoped preview/accept
	// pointers, register into the view). Shared by the ctor's main_view and create_secondary_debug_view.
	void _build_view_viewports(Node3DEditorView *p_view);

	Node *custom_camera = nullptr;

	Object *_get_editor_data(Object *p_what);

	Ref<Environment> viewport_environment;

	Node3D *selected = nullptr;

	void _request_gizmo(Object *p_obj);
	void _request_gizmo_for_id(ObjectID p_id);
	void _set_subgizmo_selection(Object *p_obj, Ref<Node3DGizmo> p_gizmo, int p_id, Transform3D p_transform = Transform3D());
	void _clear_subgizmo_selection(Object *p_obj = nullptr);

	bool gizmos_dirty = false;

	static Node3DEditor *singleton;

	void _node_added(Node *p_node);
	void _node_removed(Node *p_node);
	Vector<Ref<EditorNode3DGizmoPlugin>> gizmo_plugins_by_priority;
	Vector<Ref<EditorNode3DGizmoPlugin>> gizmo_plugins_by_name;

	void _register_all_gizmos();

	void _selection_changed();
	void _refresh_menu_icons();

	bool do_snap_selected_nodes_to_floor = false;
	void _snap_selected_nodes_to_floor();

	// Preview Sun and Environment

	class PreviewSunEnvPopup : public PopupPanel {
		GDCLASS(PreviewSunEnvPopup, PopupPanel);

	protected:
		virtual void shortcut_input(const Ref<InputEvent> &p_event) override;
	};

	uint32_t world_env_count = 0;
	uint32_t directional_light_count = 0;

	Button *sun_button = nullptr;
	Label *sun_state = nullptr;
	Label *sun_title = nullptr;
	VBoxContainer *sun_vb = nullptr;
	Popup *sun_environ_popup = nullptr;
	Control *sun_direction = nullptr;
	EditorSpinSlider *sun_angle_altitude = nullptr;
	EditorSpinSlider *sun_angle_azimuth = nullptr;
	ColorPickerButton *sun_color = nullptr;
	EditorSpinSlider *sun_energy = nullptr;
	EditorSpinSlider *sun_shadow_max_distance = nullptr;
	Button *sun_add_to_scene = nullptr;

	Vector2 sun_rotation;

	Ref<Shader> sun_direction_shader;
	Ref<ShaderMaterial> sun_direction_material;

	Button *environ_button = nullptr;
	Label *environ_state = nullptr;
	Label *environ_title = nullptr;
	VBoxContainer *environ_vb = nullptr;
	ColorPickerButton *environ_sky_color = nullptr;
	ColorPickerButton *environ_ground_color = nullptr;
	EditorSpinSlider *environ_energy = nullptr;
	Button *environ_ao_button = nullptr;
	Button *environ_glow_button = nullptr;
	Button *environ_tonemap_button = nullptr;
	Button *environ_gi_button = nullptr;
	Button *environ_add_to_scene = nullptr;

	Button *sun_environ_settings = nullptr;

	DirectionalLight3D *preview_sun = nullptr;
	bool preview_sun_dangling = false;
	WorldEnvironment *preview_environment = nullptr;
	bool preview_env_dangling = false;
	Ref<Environment> environment;
	Ref<CameraAttributesPractical> camera_attributes;
	Ref<ProceduralSkyMaterial> sky_material;
	// Document world currently carrying the preview environment; also keeps that world
	// alive across a document close until the next rebind clears it.
	Ref<World3D> preview_env_bound_world;

	bool sun_environ_updating = false;

	void _sun_direction_draw();
	void _sun_direction_input(const Ref<InputEvent> &p_event);
	void _sun_direction_set_altitude(float p_altitude);
	void _sun_direction_set_azimuth(float p_azimuth);
	void _sun_set_color(const Color &p_color);
	void _sun_set_energy(float p_energy);
	void _sun_set_shadow_max_distance(float p_shadow_max_distance);

	void _environ_set_sky_color(const Color &p_color);
	void _environ_set_ground_color(const Color &p_color);
	void _environ_set_sky_energy(float p_energy);
	void _environ_set_ao();
	void _environ_set_glow();
	void _environ_set_tonemap();
	void _environ_set_gi();

	void _load_default_preview_settings();
	void _update_preview_environment();
	// Recompute directional_light_count / world_env_count from the currently edited scene.
	// The running counters maintained by _node_added/_node_removed assume a single resident
	// scene (stock Godot); with the workspace model every document keeps its own SubViewport
	// resident, so switching tabs never fires the node_removed that would decrement a prior
	// scene's light. Recounting on scene switch re-establishes truth for the active document.
	void _recount_scene_lights_and_environments();
	// The preview sun/environment nodes are parented to this singleton, which lives in the
	// root-window world — a world no document viewport renders. Retarget their render state
	// onto the active document's World3D (sun: instance_set_scenario, like the per-view
	// grid/origin decorations; environment: applied directly on the World3D).
	void _bind_preview_nodes_to_active_world();

	void _preview_settings_changed();
	void _sun_environ_settings_pressed();

	void _add_sun_to_scene(bool p_already_added_environment = false);
	void _add_environment_to_scene(bool p_already_added_sun = false);

	void _update_theme();

	void _undo_redo_inspector_callback(Object *p_undo_redo, Object *p_edited, const String &p_property, const Variant &p_new_value);

protected:
	void _notification(int p_what);
	//void _gui_input(InputEvent p_event);
	virtual void shortcut_input(const Ref<InputEvent> &p_event) override;

	static void _bind_methods();

public:
	static Node3DEditor *get_singleton() { return singleton; }

	// Single source of truth for which World3D the 3D editor renders gizmos/grid/
	// origin into and picks against. v1 returns the root-window world (unchanged);
	// the G1 flip makes these return the active document's world/scenario/space so
	// each live scene is isolated. All hardcoded root-window sites route through here.

	// Re-sync the preview sun/environment toggles with the now-active document's scene.
	// Called on scene switch (see Node3DEditorPlugin::edited_scene_changed).
	void update_preview_for_edited_scene() { _recount_scene_lights_and_environments(); }

	Ref<World3D> get_editor_world_3d() const;
	RID get_editor_scenario() const;
	PhysicsDirectSpaceState3D *get_editor_space_state() const;
	// G1: bind all 3D viewports + grid/origin to the given (active document's) world.
	void set_active_world(const Ref<World3D> &p_world);

	// G2: allocate/free a transform-gizmo cull-mask layer (GIZMO_BASE_LAYER..31) FROM p_world's
	// freelist. Returns a distinct layer while any of the 5 are free within that world; degrades
	// to sharing the base layer past 5 simultaneous views OF THE SAME world (gizmos overlap but
	// remain functional). Different worlds reuse the same layer bits safely (separate scenarios).
	int allocate_gizmo_layer(const Ref<World3D> &p_world);
	void free_gizmo_layer(const Ref<World3D> &p_world, int p_layer);

	static Size2i get_camera_viewport_size(Camera3D *p_camera);

	Vector3 snap_point(Vector3 p_target, Vector3 p_start = Vector3(0, 0, 0)) const;

	float get_znear() const;
	float get_zfar() const;
	float get_fov() const;

	Transform3D get_gizmo_transform() const { return gizmo.transform; }
	bool is_gizmo_visible() const;

	ToolMode get_tool_mode() const { return tool_mode; }
	bool are_local_coords_enabled() const;
	void set_local_coords_enabled(bool on) const;
	bool is_preserve_children_transform_enabled() const;
	bool is_snap_enabled() const { return snap_enabled ^ snap_key_enabled; }
	bool is_vertex_snap_origin_mode() const { return vertex_snap_origin_mode; }
	bool is_vertex_snap_use_collision() const;
	real_t get_translate_snap() const;
	real_t get_rotate_snap() const;
	real_t get_scale_snap() const;

	bool is_trackball_enabled() const { return trackball_enabled; }

	Ref<ArrayMesh> get_move_gizmo(int idx) const { return move_gizmo[idx]; }
	Ref<ArrayMesh> get_axis_gizmo(int idx) const { return axis_gizmo[idx]; }
	Ref<ArrayMesh> get_move_plane_gizmo(int idx) const { return move_plane_gizmo[idx]; }
	Ref<ArrayMesh> get_rotate_gizmo(int idx) const { return rotate_gizmo[idx]; }
	Ref<ArrayMesh> get_scale_gizmo(int idx) const { return scale_gizmo[idx]; }
	Ref<ArrayMesh> get_scale_plane_gizmo(int idx) const { return scale_plane_gizmo[idx]; }
	Ref<ArrayMesh> get_trackball_sphere_gizmo() const { return trackball_sphere_gizmo; }

	void update_grid();
	void update_transform_gizmo();
	void update_all_gizmos(Node *p_node = nullptr);
	void build_scene_gizmos(Node *p_node); // G2: force-build gizmos for a newly-active pane document (bypasses the gizmos_requested latch).
	void build_edited_scene_gizmos(); // G2: deferred wrapper — build_scene_gizmos(get_edited_scene()); hooked from set_edited_scene_root.
	void update_gizmo_opacity();
	void snap_selected_nodes_to_floor();
	void select_gizmo_highlight_axis(int p_axis);
	void set_custom_camera(Node *p_camera) { custom_camera = p_camera; }

	Dictionary get_state() const;
	void set_state(const Dictionary &p_state);

	Ref<Environment> get_viewport_environment() { return viewport_environment; }

	void add_control_to_menu_panel(Control *p_control);
	void remove_control_from_menu_panel(Control *p_control);

	void add_control_to_left_panel(Control *p_control);
	void remove_control_from_left_panel(Control *p_control);

	void add_control_to_right_panel(Control *p_control);
	void remove_control_from_right_panel(Control *p_control);

	void move_control_to_left_panel(Control *p_control);
	void move_control_to_right_panel(Control *p_control);

	VSplitContainer *get_shader_split();

	Node3D *get_single_selected_node() { return selected; }
	bool is_current_selected_gizmo(const EditorNode3DGizmo *p_gizmo);
	bool is_subgizmo_selected(int p_id);
	Vector<int> get_subgizmo_selection();
	void clear_subgizmo_selection(Object *p_obj = nullptr);
	void refresh_dirty_gizmos();

	Ref<EditorNode3DGizmo> get_current_hover_gizmo() const { return current_hover_gizmo; }
	void set_current_hover_gizmo(Ref<EditorNode3DGizmo> p_gizmo) { current_hover_gizmo = p_gizmo; }

	void set_current_hover_gizmo_handle(int p_id, bool p_secondary) {
		current_hover_gizmo_handle = p_id;
		current_hover_gizmo_handle_secondary = p_secondary;
	}

	int get_current_hover_gizmo_handle(bool &r_secondary) const {
		r_secondary = current_hover_gizmo_handle_secondary;
		return current_hover_gizmo_handle;
	}

	void set_can_preview(Camera3D *p_preview);

	void set_preview_material(Ref<Material> p_material) { preview_material = p_material; }
	Ref<Material> get_preview_material() { return preview_material; }
	void set_preview_reset_material(Ref<Material> p_material) { preview_reset_material = p_material; }
	Ref<Material> get_preview_reset_material() const { return preview_reset_material; }
	void set_preview_material_target(ObjectID p_object_id) { preview_material_target = p_object_id; }
	ObjectID get_preview_material_target() const { return preview_material_target; }
	void set_preview_material_surface(int p_surface) { preview_material_surface = p_surface; }
	int get_preview_material_surface() const { return preview_material_surface; }

	// Forward to the 3D view that owns the viewport quad (Step③). Out-of-line so the
	// forwarding can reach Node3DEditorView, which is only forward-declared here.
	Node3DEditorView *get_main_view() const { return main_view; }

	// Mint a fully independent Node3DEditorView -- its own viewport quad and grid/origin
	// decoration -- bound to p_document's world (falls back to the main view's world if the
	// document is null/unbound). This is the 3D editor surface a per-pane DocumentView hosts; N of these
	// coexist, one per pane, each rendering its document's isolated world. Returned as a
	// Control for the workspace to host.
	Control *create_view_bound_to(EditorDocument *p_document);
	Node3DEditorViewport *get_editor_viewport(int p_idx) const;
	Node3DEditorViewport *get_last_used_viewport();
	// Resolve the viewport a forwarded 3D input event came from. Input can originate from any
	// live view's pane (not just main_view), and while a camera preview is active the camera
	// handed to plugins is the previewing one — both are matched here.
	Node3DEditorViewport *find_viewport_for_input_camera(Camera3D *p_camera) const;

	// G2 M7.2a: the shared 3D toolbar follows the focused scene pane. get_shared_toolbar() is the
	// Control the pane header reparents in; park_shared_toolbar() returns it to its stock home.
	Control *get_shared_toolbar() const;
	void park_shared_toolbar();
	// G2 M7.2a-fix: re-point the toolbar's shortcut contexts (tool Q/W/E/R, snap, view menu) at the
	// focused pane so its keyboard shortcuts fire there; null resets to this singleton.
	void set_toolbar_shortcut_context(Node *p_context);
	void set_scene_view_button_state(bool p_2d);

	void set_freelook_viewport(Node3DEditorViewport *p_viewport);
	Node3DEditorViewport *get_freelook_viewport() const;

	void add_gizmo_plugin(Ref<EditorNode3DGizmoPlugin> p_plugin);
	void remove_gizmo_plugin(Ref<EditorNode3DGizmoPlugin> p_plugin);

	DynamicBVH::ID insert_gizmo_bvh_node(Node3D *p_node, const AABB &p_aabb);
	void update_gizmo_bvh_node(DynamicBVH::ID p_id, const AABB &p_aabb);
	void remove_gizmo_bvh_node(DynamicBVH::ID p_id);
	// The gizmo BVH is shared by every open document; queries take the caller's World3D and
	// only return nodes living in it, so consumers never see other documents' nodes.
	Vector<Node3D *> gizmo_bvh_ray_query(const Vector3 &p_ray_start, const Vector3 &p_ray_end, const Ref<World3D> &p_world);
	Vector<Node3D *> gizmo_bvh_frustum_query(const Vector<Plane> &p_frustum, const Ref<World3D> &p_world);

	void edit(Node3D *p_spatial);
	void clear();

	Node3DEditor();
	~Node3DEditor();
};

// One 3D editing view: the quad of Node3DEditorViewports for a single pane (Step③).
// The instanceable half of the old Node3DEditor -- shared tool/gizmo/toolbar state
// stays on the Node3DEditor singleton (services), which owns one of these today and
// will own one per workspace pane later. Holds only view-local presentation state.
class Node3DEditorView : public MarginContainer {
	GDCLASS(Node3DEditorView, MarginContainer);
	friend class Node3DEditor; // Services reads/writes this view's bound world (Step③a.1b).

	Node3DEditor *editor = nullptr; // Services singleton this view belongs to.
	Node3DEditorViewportContainer *viewport_base = nullptr;
	Node3DEditorViewport *viewports[Node3DEditor::VIEWPORTS_COUNT] = {};
	int last_used_viewport = 0;
	Node3DEditorViewport *freelook_viewport = nullptr;
	EditorDocument *document = nullptr;

	// The World3D this view renders/picks against (was Node3DEditor::bound_world). Set when the
	// workspace activates a document in this view; the resolver reads THIS, not the globally-
	// active document, so N views can each bind their own world.
	Ref<World3D> bound_world;

	// Per-view grid/origin decoration (Step①). The view owns its own resource lifecycle: it
	// builds these on ITS OWN NOTIFICATION_ENTER_TREE (not the services' -- so they're created
	// when this view is genuinely in the tree, theme/world ready) and frees them in the dtor,
	// tolerant of reparenting between panes. Instances are created DETACHED and attached to
	// bound_world's scenario by _reconcile_decorations(), deferred one frame the first time so
	// the freshly-created materials register with the renderer before anything renders them.
	bool decorations_initialized = false;
	bool decorations_bindable = false; // Set by the deferred first reconcile; gates attaching.
	RID origin_mesh;
	RID origin_multimesh;
	RID origin_instance;
	bool origin_enabled = false;
	RID grid[3];
	RID grid_instance[3];
	bool grid_visible[3] = { false, false, false }; // currently visible
	bool grid_enable[3] = { false, false, false }; // should be always visible if true
	bool grid_enabled = false;
	bool grid_init_draw = false;
	Camera3D::ProjectionType grid_camera_last_update_perspective = Camera3D::PROJECTION_PERSPECTIVE;
	Vector3 grid_camera_last_update_position;
	Ref<ShaderMaterial> origin_mat;
	Ref<ShaderMaterial> grid_mat[3];

	void _init_grid();
	void _finish_grid();
	void _reconcile_decorations(); // Attach origin/grid instances to bound_world's scenario.
	void _deferred_first_bind(); // Deferred: mark bindable, then reconcile (breaks the material race).
	bool _is_active_document() const;
	void _ensure_active();

protected:
	static void _bind_methods() {}
	void _notification(int p_what);
	virtual void input(const Ref<InputEvent> &p_event) override;

public:
	// World binding + per-view grid/origin decoration. Node3DEditor forwards its same-named
	// methods here so external/internal callers are unchanged. get_editor_world_3d() falls
	// back to the root-window world before the first bind.
	Ref<World3D> get_editor_world_3d() const;
	RID get_editor_scenario() const;
	PhysicsDirectSpaceState3D *get_editor_space_state() const;
	void bind_document(EditorDocument *p_document);
	void set_active_world(const Ref<World3D> &p_world);

	void init_decorations();
	void finish_decorations();
	void update_grid();

	Node3DEditorViewportContainer *get_viewport_base() const { return viewport_base; }

	// Store a viewport built by the editor (which has the ctor-scoped preview/accept
	// pointers) into this view's slot; the view owns the quad from then on.
	void register_viewport(int p_idx, Node3DEditorViewport *p_viewport) {
		ERR_FAIL_INDEX(p_idx, static_cast<int>(Node3DEditor::VIEWPORTS_COUNT));
		viewports[p_idx] = p_viewport;
	}

	Node3DEditorViewport *get_editor_viewport(int p_idx) const {
		ERR_FAIL_INDEX_V(p_idx, static_cast<int>(Node3DEditor::VIEWPORTS_COUNT), nullptr);
		return viewports[p_idx];
	}
	Node3DEditorViewport *get_last_used_viewport() const { return viewports[last_used_viewport]; }
	int get_last_used_viewport_index() const { return last_used_viewport; }
	void set_last_used_viewport_index(int p_idx) { last_used_viewport = p_idx; }

	void set_freelook_viewport(Node3DEditorViewport *p_viewport) { freelook_viewport = p_viewport; }
	Node3DEditorViewport *get_freelook_viewport() const { return freelook_viewport; }

	Node3DEditorView(Node3DEditor *p_editor);
	~Node3DEditorView();
};

class Node3DEditorPlugin : public EditorPlugin {
	GDCLASS(Node3DEditorPlugin, EditorPlugin);

	Node3DEditor *spatial_editor = nullptr;

public:
	Node3DEditor *get_spatial_editor() { return spatial_editor; }
	virtual String get_plugin_name() const override { return TTRC("3D"); }
	bool has_main_screen() const override { return true; }
	virtual void make_visible(bool p_visible) override;
	virtual void edit(Object *p_object) override;
	virtual bool handles(Object *p_object) const override;

	virtual Dictionary get_state() const override;
	virtual void set_state(const Dictionary &p_state) override;
	virtual void clear() override { spatial_editor->clear(); }

	virtual void edited_scene_changed() override;

	Node3DEditorPlugin();
};
