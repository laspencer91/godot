/**************************************************************************/
/*  level_editor_view.cpp                                                 */
/**************************************************************************/
/*  G-Level LE0: per-pane VIEW state for a LevelDocument.                 */
/*  Grid RIDs follow create-detached -> deferred bind -> free-in-dtor.     */
/**************************************************************************/

#include "level_editor_view.h"

#include "core/object/callable_mp.h"
#include "editor/editor_document.h"
#include "editor/editor_node.h"
#include "editor/level/block_tool.h"
#include "editor/level/select_tool.h"
#include "editor/level/selection_highlight_overlay.h"
#include "editor/level/selection_model.h"
#include "editor/scene/3d/node_3d_editor_plugin.h"
#include "editor/settings/editor_settings.h"
#include "editor/themes/editor_scale.h"
#include "scene/3d/camera_3d.h"
#include "scene/gui/box_container.h"
#include "scene/gui/button.h"
#include "scene/gui/label.h"
#include "scene/gui/margin_container.h"
#include "scene/gui/separator.h"
#include "scene/gui/subviewport_container.h"
#include "scene/main/viewport.h"
#include "servers/rendering/rendering_server.h"

class LevelMarqueeOverlay : public Control {
	Rect2 marquee_rect;
	bool marquee_visible = false;

protected:
	void _notification(int p_what) {
		if (p_what == NOTIFICATION_DRAW && marquee_visible) {
			draw_rect(marquee_rect, Color(0.22, 0.60, 1.0, 0.12), true);
			draw_rect(marquee_rect, Color(0.42, 0.78, 1.0, 0.95), false, 1.5 * EDSCALE);
		}
	}

public:
	void set_marquee(const Rect2 &p_rect, bool p_visible) {
		marquee_rect = p_rect;
		marquee_visible = p_visible;
		queue_redraw();
	}
};

void LevelEditorView::_create_grid() {
	grid_material.instantiate();
	grid_material->set_shading_mode(BaseMaterial3D::SHADING_MODE_UNSHADED);
	grid_material->set_transparency(BaseMaterial3D::TRANSPARENCY_ALPHA);
	grid_material->set_flag(BaseMaterial3D::FLAG_ALBEDO_FROM_VERTEX_COLOR, true);

	grid_mesh.instantiate();
	grid_mesh->surface_begin(Mesh::PRIMITIVE_LINES, grid_material);
	constexpr int half_extent = 64;
	for (int i = -half_extent; i <= half_extent; i++) {
		Color x_line = (i % LevelEditor::MAJOR_GRID_MULTIPLE == 0) ? Color(0.48, 0.52, 0.58, 0.42) : Color(0.42, 0.46, 0.52, 0.20);
		Color z_line = x_line;
		if (i == 0) {
			x_line = Color(0.30, 0.58, 0.95, 0.75);
			z_line = Color(0.95, 0.34, 0.30, 0.75);
		}

		grid_mesh->surface_set_color(x_line);
		grid_mesh->surface_add_vertex(Vector3(i, 0.0, -half_extent));
		grid_mesh->surface_add_vertex(Vector3(i, 0.0, half_extent));
		grid_mesh->surface_set_color(z_line);
		grid_mesh->surface_add_vertex(Vector3(-half_extent, 0.0, i));
		grid_mesh->surface_add_vertex(Vector3(half_extent, 0.0, i));
	}
	grid_mesh->surface_end();

	RenderingServer *rs = RenderingServer::get_singleton();
	ERR_FAIL_NULL(rs);
	// Keep the decoration detached until the material/mesh have crossed the render-server queue.
	grid_instance = rs->instance_create();
	rs->instance_set_base(grid_instance, grid_mesh->get_rid());
	rs->instance_set_layer_mask(grid_instance, 1u << gizmo_layer);
	rs->instance_geometry_set_cast_shadows_setting(grid_instance, RSE::SHADOW_CASTING_SETTING_OFF);
	rs->instance_geometry_set_flag(grid_instance, RSE::INSTANCE_FLAG_IGNORE_OCCLUSION_CULLING, true);
	rs->instance_geometry_set_flag(grid_instance, RSE::INSTANCE_FLAG_USE_BAKED_LIGHT, false);
	callable_mp(this, &LevelEditorView::_reconcile_grid).call_deferred();
}

void LevelEditorView::_reconcile_grid() {
	if (!document || !grid_instance.is_valid()) {
		return;
	}
	RenderingServer *rs = RenderingServer::get_singleton();
	if (!rs || !document->get_scenario().is_valid()) {
		return;
	}
	rs->instance_set_scenario(grid_instance, document->get_scenario());
	grid_attached = true;
	_update_grid_transform();
	rs->instance_set_visible(grid_instance, is_visible_in_tree());
}

void LevelEditorView::_update_grid_transform() {
	if (!grid_attached || !camera || !grid_instance.is_valid()) {
		return;
	}
	const Vector3 camera_position = camera->get_position();
	constexpr real_t major_grid_step = LevelEditor::DEFAULT_SNAP_STEP * LevelEditor::MAJOR_GRID_MULTIPLE;
	Transform3D grid_transform;
	grid_transform.origin = Vector3(
			Math::floor(camera_position.x / major_grid_step) * major_grid_step,
			0.01f,
			Math::floor(camera_position.z / major_grid_step) * major_grid_step);
	RenderingServer::get_singleton()->instance_set_transform(grid_instance, grid_transform);
}

void LevelEditorView::_update_orbit_camera() {
	if (!camera) {
		return;
	}
	const float horizontal = Math::cos(orbit_pitch) * orbit_distance;
	const Vector3 offset(
			Math::sin(orbit_yaw) * horizontal,
			Math::sin(orbit_pitch) * orbit_distance,
			Math::cos(orbit_yaw) * horizontal);
	camera->set_position(orbit_pivot + offset);
	camera->look_at_from_position(camera->get_position(), orbit_pivot, Vector3::UP);
	_update_grid_transform();
}

void LevelEditorView::_begin_freelook() {
	if (freelook || !camera) {
		return;
	}
	freelook = true;
	orbiting = false;
	panning = false;
	viewport_container->grab_focus();
	const Vector3 forward = -camera->get_global_transform().basis.get_column(2).normalized();
	freelook_yaw = Math::atan2(-forward.x, -forward.z);
	freelook_pitch = Math::asin(CLAMP(forward.y, -1.0f, 1.0f));
	Input *input = Input::get_singleton();
	previous_mouse_mode = input->get_mouse_mode();
	input->set_mouse_mode(Input::MouseMode::MOUSE_MODE_CAPTURED);
}

void LevelEditorView::_end_freelook() {
	if (!freelook) {
		return;
	}
	freelook = false;
	if (Input *input = Input::get_singleton()) {
		input->set_mouse_mode(previous_mouse_mode);
	}
	if (camera) {
		const Vector3 forward = -camera->get_global_transform().basis.get_column(2).normalized();
		orbit_pivot = camera->get_position() + forward * orbit_distance;
		const Vector3 offset = camera->get_position() - orbit_pivot;
		orbit_yaw = Math::atan2(offset.x, offset.z);
		orbit_pitch = Math::asin(CLAMP(offset.y / orbit_distance, -1.0f, 1.0f));
	}
}

void LevelEditorView::_tool_button_pressed(int p_mode) {
	LevelEditor *level_editor = LevelEditor::get_singleton();
	ERR_FAIL_NULL(level_editor);
	level_editor->set_tool_mode(LevelEditor::ToolMode(p_mode));
}

void LevelEditorView::set_tool_mode(LevelEditor::ToolMode p_mode) {
	ERR_FAIL_INDEX(int(p_mode), 2);
	if (active_tool.is_valid()) {
		active_tool->deactivate();
	}
	active_tool = tools[int(p_mode)];
	if (active_tool.is_valid()) {
		active_tool->activate();
		active_tool->set_view_visible(is_visible_in_tree());
	}

	if (select_tool_button) {
		select_tool_button->set_pressed_no_signal(p_mode == LevelEditor::TOOL_SELECT);
	}
	if (block_tool_button) {
		block_tool_button->set_pressed_no_signal(p_mode == LevelEditor::TOOL_BLOCK);
	}
	set_meta(StringName("_level_tool_mode"), int(p_mode));
}

void LevelEditorView::shortcut_input(const Ref<InputEvent> &p_event) {
	EditorNode *editor_node = EditorNode::get_singleton();
	if (!is_visible_in_tree() || !editor_node || editor_node->get_editor_data().get_active_document() != document) {
		return;
	}
	// Selection/tool shortcuts are pane-local. Do not steal numeric or editing
	// keys while the scene tree, Inspector, or another editor control has focus.
	if (!viewport_container || !viewport_container->has_focus()) {
		return;
	}
	Ref<InputEventKey> key = p_event;
	if (key.is_valid() && key->is_pressed() && !key->is_echo() && ED_IS_SHORTCUT("level_editor/add_block", p_event)) {
		if (LevelEditor *level_editor = LevelEditor::get_singleton()) {
			level_editor->set_tool_mode(LevelEditor::TOOL_BLOCK);
			accept_event();
		}
		return;
	}
	if (active_tool.is_valid() && active_tool->handle_input(camera, p_event)) {
		accept_event();
	}
}

void LevelEditorView::_viewport_gui_input(const Ref<InputEvent> &p_event) {
	Ref<InputEventMouseButton> button = p_event;
	if (button.is_valid()) {
		const MouseButton index = button->get_button_index();
		if (index == MouseButton::RIGHT) {
			if (button->is_pressed() && active_tool.is_valid() && active_tool->handle_input(camera, p_event)) {
				viewport_container->accept_event();
				return;
			}
			if (button->is_pressed()) {
				_begin_freelook();
			} else {
				_end_freelook();
			}
			viewport_container->accept_event();
			return;
		}
		if (index == MouseButton::MIDDLE) {
			if (button->is_pressed()) {
				viewport_container->grab_focus();
				panning = button->is_shift_pressed() || button->is_alt_pressed();
				orbiting = !panning;
			} else {
				orbiting = false;
				panning = false;
			}
			viewport_container->accept_event();
			return;
		}
		if (index == MouseButton::LEFT && button->is_alt_pressed()) {
			orbiting = button->is_pressed();
			if (button->is_pressed()) {
				viewport_container->grab_focus();
			}
			viewport_container->accept_event();
			return;
		}
		if (button->is_pressed() && (index == MouseButton::WHEEL_UP || index == MouseButton::WHEEL_DOWN)) {
			const float factor = index == MouseButton::WHEEL_UP ? 0.84f : (1.0f / 0.84f);
			orbit_distance = CLAMP(orbit_distance * factor, 0.25f, 4096.0f);
			_update_orbit_camera();
			viewport_container->accept_event();
			return;
		}
	}

	if (active_tool.is_valid() && active_tool->handle_input(camera, p_event)) {
		viewport_container->accept_event();
		return;
	}

	Ref<InputEventMouseMotion> motion = p_event;
	if (motion.is_null()) {
		return;
	}
	const Vector2 relative = motion->get_relative();
	if (freelook) {
		freelook_yaw -= relative.x * 0.006f;
		freelook_pitch = CLAMP(freelook_pitch - relative.y * 0.006f, Math::deg_to_rad(-89.0f), Math::deg_to_rad(89.0f));
		const Vector3 forward(
				-Math::sin(freelook_yaw) * Math::cos(freelook_pitch),
				Math::sin(freelook_pitch),
				-Math::cos(freelook_yaw) * Math::cos(freelook_pitch));
		camera->look_at(camera->get_position() + forward, Vector3::UP);
		viewport_container->accept_event();
	} else if (orbiting) {
		orbit_yaw -= relative.x * 0.008f;
		orbit_pitch = CLAMP(orbit_pitch + relative.y * 0.008f, Math::deg_to_rad(-89.0f), Math::deg_to_rad(89.0f));
		_update_orbit_camera();
		viewport_container->accept_event();
	} else if (panning) {
		const Basis basis = camera->get_global_transform().basis;
		const float scale = MAX(orbit_distance, 1.0f) * 0.0025f;
		orbit_pivot += (-basis.get_column(0) * relative.x + basis.get_column(1) * relative.y) * scale;
		_update_orbit_camera();
		viewport_container->accept_event();
	}
}

void LevelEditorView::_process_freelook(double p_delta) {
	if (!freelook || !camera) {
		return;
	}
	Input *input = Input::get_singleton();
	Vector3 direction;
	const Basis basis = camera->get_global_transform().basis;
	const Vector3 forward = -basis.get_column(2);
	const Vector3 right = basis.get_column(0);
	if (input->is_physical_key_pressed(Key::W)) {
		direction += forward;
	}
	if (input->is_physical_key_pressed(Key::S)) {
		direction -= forward;
	}
	if (input->is_physical_key_pressed(Key::D)) {
		direction += right;
	}
	if (input->is_physical_key_pressed(Key::A)) {
		direction -= right;
	}
	if (direction.is_zero_approx()) {
		return;
	}
	const float speed = MAX(2.0f, orbit_distance * 0.6f) * (input->is_key_pressed(Key::SHIFT) ? 4.0f : 1.0f);
	camera->set_position(camera->get_position() + direction.normalized() * speed * p_delta);
	_update_grid_transform();
}

void LevelEditorView::_selection_changed(const PackedInt64Array &p_dirty_blocks) {
	(void)p_dirty_blocks;
	_sync_selection_metadata();
}

void LevelEditorView::_sync_selection_metadata() {
	SelectionModel *selection_model = document ? document->get_selection_model() : nullptr;
	if (!selection_model) {
		return;
	}
	set_meta(StringName("_level_selection_mode"), int(selection_model->get_mode()));
	set_meta(StringName("_level_selection_tier"), int(selection_model->get_tier()));
	const uint64_t revision = selection_model->get_revision();
	if (last_selection_revision != revision) {
		last_selection_revision = revision;
		set_meta(StringName("_level_selection_vertex_count"), selection_model->get_count(SelectionModel::FEATURE_VERTEX));
		set_meta(StringName("_level_selection_edge_count"), selection_model->get_count(SelectionModel::FEATURE_EDGE));
		set_meta(StringName("_level_selection_face_count"), selection_model->get_count(SelectionModel::FEATURE_FACE));
		set_meta(StringName("_level_selection_entries"), selection_model->get_debug_entries());
		set_meta(StringName("_level_selection_active"), selection_model->get_debug_active());
	}

	if (selection_mode_indicator) {
		static const char *MODE_NAMES[] = { "Vertex", "Edge", "Face", "Object" };
		static const char *TIER_NAMES[] = { "Polygroup", "Triangle" };
		const int mode_index = int(selection_model->get_mode());
		if (selection_model->get_mode() == SelectionModel::MODE_OBJECT) {
			selection_mode_indicator->set_text(vformat(TTR("Selection: %s"), TTR(MODE_NAMES[mode_index])));
		} else {
			selection_mode_indicator->set_text(vformat(TTR("Selection: %s · %s"),
					TTR(MODE_NAMES[mode_index]), TTR(TIER_NAMES[int(selection_model->get_tier())])));
		}
	}
}

void LevelEditorView::set_marquee_rect(const Rect2 &p_rect, bool p_visible) {
	if (marquee_overlay) {
		marquee_overlay->set_marquee(p_rect, p_visible);
	}
}

void LevelEditorView::set_last_selection_action(const StringName &p_action) {
	set_meta(StringName("_level_last_selection_action"), p_action);
}

void LevelEditorView::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_PROCESS: {
			_process_freelook(get_process_delta_time());
		} break;
		case NOTIFICATION_VISIBILITY_CHANGED: {
			if (!is_visible_in_tree()) {
				_end_freelook();
			}
			if (grid_attached && grid_instance.is_valid() && RenderingServer::get_singleton()) {
				RenderingServer::get_singleton()->instance_set_visible(grid_instance, is_visible_in_tree());
			}
			if (active_tool.is_valid()) {
				active_tool->set_view_visible(is_visible_in_tree());
			}
			if (selection_overlay) {
				selection_overlay->set_view_visible(is_visible_in_tree());
			}
		} break;
		case NOTIFICATION_EXIT_TREE: {
			_end_freelook();
		} break;
	}
}

void LevelEditorView::mount_top_strip(Control *p_toolbar_host) {
	Control *target = p_toolbar_host ? p_toolbar_host : surface_layout;
	if (!target || !top_strip || top_strip->get_parent() == target) {
		return;
	}
	if (Node *parent = top_strip->get_parent()) {
		parent->remove_child(top_strip);
	}
	target->add_child(top_strip);
	if (target == surface_layout) {
		surface_layout->move_child(top_strip, 0);
	}
}

LevelEditorView::LevelEditorView(LevelDocument *p_document) :
		document(p_document) {
	ERR_FAIL_NULL(document);
	SelectionModel *selection_model = document->get_selection_model();
	ERR_FAIL_NULL(selection_model);
	selection_model->bind_document(document);
	if (Node3DEditor *node_3d_editor = Node3DEditor::get_singleton()) {
		gizmo_layer = node_3d_editor->allocate_gizmo_layer(document->get_world_3d());
		gizmo_layer_allocated = true;
	}
	set_name("LevelEditorView");
	set_h_size_flags(SIZE_EXPAND_FILL);
	set_v_size_flags(SIZE_EXPAND_FILL);
	set_process(true);

	// Smoke-visible facts are copied from the actual document, not synthesized by the test.
	set_meta(StringName("_level_document_type"), int(document->get_type()));
	set_meta(StringName("_level_document_world_3d"), document->get_world_3d());
	set_meta(StringName("_level_document_root"), document->get_root());

	surface_layout = memnew(VBoxContainer);
	surface_layout->set_anchors_and_offsets_preset(PRESET_FULL_RECT);
	surface_layout->set_h_size_flags(SIZE_EXPAND_FILL);
	surface_layout->set_v_size_flags(SIZE_EXPAND_FILL);
	surface_layout->add_theme_constant_override("separation", 0);
	add_child(surface_layout);

	top_strip = memnew(HBoxContainer);
	top_strip->set_name("LevelEditorTopStrip");
	top_strip->set_h_size_flags(SIZE_EXPAND_FILL);
	top_strip->add_theme_constant_override("separation", 8 * EDSCALE);
	top_strip->add_child(memnew(Label(TTRC("Level Editor"))));
	top_strip->add_child(memnew(VSeparator));
	selection_mode_indicator = memnew(Label);
	top_strip->add_child(selection_mode_indicator);
	top_strip->add_child(memnew(VSeparator));
	const String grid_step = String::num_real(LevelEditor::DEFAULT_SNAP_STEP, false);
	const String major_grid_step = String::num_real(LevelEditor::DEFAULT_SNAP_STEP * LevelEditor::MAJOR_GRID_MULTIPLE, false);
	top_strip->add_child(memnew(Label(vformat(TTRC("Grid: %s m / major %s m"), grid_step, major_grid_step))));
	surface_layout->add_child(top_strip); // Fallback until DocumentView supplies toolbar_host.

	HBoxContainer *body = memnew(HBoxContainer);
	body->set_h_size_flags(SIZE_EXPAND_FILL);
	body->set_v_size_flags(SIZE_EXPAND_FILL);
	body->add_theme_constant_override("separation", 0);
	surface_layout->add_child(body);

	MarginContainer *left_toolbar_margin = memnew(MarginContainer);
	left_toolbar_margin->add_theme_constant_override("margin_left", 6 * EDSCALE);
	left_toolbar_margin->add_theme_constant_override("margin_right", 6 * EDSCALE);
	left_toolbar_margin->add_theme_constant_override("margin_top", 6 * EDSCALE);
	left_toolbar_margin->add_theme_constant_override("margin_bottom", 6 * EDSCALE);
	body->add_child(left_toolbar_margin);

	VBoxContainer *left_toolbar = memnew(VBoxContainer);
	left_toolbar->set_name("LevelEditorLeftToolbar");
	left_toolbar->set_custom_minimum_size(Size2(72 * EDSCALE, 0));
	left_toolbar->set_v_size_flags(SIZE_EXPAND_FILL);
	left_toolbar->add_theme_constant_override("separation", 4 * EDSCALE);
	Ref<ButtonGroup> tool_button_group;
	tool_button_group.instantiate();
	select_tool_button = memnew(Button(TTRC("Select")));
	select_tool_button->set_toggle_mode(true);
	select_tool_button->set_button_group(tool_button_group);
	select_tool_button->set_tooltip_text(TTRC("Select vertices, edges, faces, or blocks"));
	select_tool_button->connect(SceneStringName(pressed), callable_mp(this, &LevelEditorView::_tool_button_pressed).bind(int(LevelEditor::TOOL_SELECT)));
	left_toolbar->add_child(select_tool_button);

	block_tool_button = memnew(Button(TTRC("Block")));
	block_tool_button->set_toggle_mode(true);
	block_tool_button->set_button_group(tool_button_group);
	block_tool_button->set_tooltip_text(TTRC("Add Block (Shift+B)"));
	block_tool_button->connect(SceneStringName(pressed), callable_mp(this, &LevelEditorView::_tool_button_pressed).bind(int(LevelEditor::TOOL_BLOCK)));
	left_toolbar->add_child(block_tool_button);
	left_toolbar_margin->add_child(left_toolbar);

	viewport_container = memnew(SubViewportContainer);
	viewport_container->set_name("LevelViewportContainer");
	viewport_container->set_stretch(true);
	viewport_container->set_focus_mode(FOCUS_ALL);
	viewport_container->set_h_size_flags(SIZE_EXPAND_FILL);
	viewport_container->set_v_size_flags(SIZE_EXPAND_FILL);
	viewport_container->connect(SceneStringName(gui_input), callable_mp(this, &LevelEditorView::_viewport_gui_input));
	body->add_child(viewport_container);

	viewport = memnew(SubViewport);
	viewport->set_name("LevelViewport");
	viewport->set_size(Size2i(1, 1));
	viewport->set_update_mode(SubViewport::UPDATE_WHEN_VISIBLE);
	viewport->set_disable_input(true);
	// DOCUMENT seam: render this document's world explicitly. Audio and physics remain untouched.
	viewport->set_world_3d(document->get_world_3d());
	viewport_container->add_child(viewport);

	marquee_overlay = memnew(LevelMarqueeOverlay);
	marquee_overlay->set_name("LevelSelectionMarquee");
	marquee_overlay->set_anchors_and_offsets_preset(PRESET_FULL_RECT);
	marquee_overlay->set_mouse_filter(MOUSE_FILTER_IGNORE);
	viewport_container->add_child(marquee_overlay);

	camera = memnew(Camera3D);
	camera->set_name("LevelCamera3D");
	camera->set_disable_gizmos(true);
	camera->set_fov(70.0f);
	camera->set_near(0.05f);
	camera->set_far(8192.0f);
	camera->set_cull_mask(((1u << 20) - 1u) | (1u << gizmo_layer));
	viewport->add_child(camera);
	camera->make_current();
	_update_orbit_camera();

	_create_grid();
	selection_overlay = memnew(SelectionHighlightOverlay);
	selection_overlay->initialize(selection_model, document->get_scenario(), gizmo_layer);
	selection_overlay->set_view_visible(is_visible_in_tree());
	selection_model->connect(SNAME("selection_changed"), callable_mp(this, &LevelEditorView::_selection_changed));
	_sync_selection_metadata();

	Ref<SelectTool> select_tool;
	select_tool.instantiate();
	tools[LevelEditor::TOOL_SELECT] = select_tool;
	Ref<BlockTool> block_tool;
	block_tool.instantiate();
	tools[LevelEditor::TOOL_BLOCK] = block_tool;
	for (Ref<LevelEditorTool> &tool : tools) {
		tool->initialize(this);
	}
	if (LevelEditor *level_editor = LevelEditor::get_singleton()) {
		level_editor->_register_view(this);
	}
}

LevelEditorView::~LevelEditorView() {
	if (LevelEditor *level_editor = LevelEditor::get_singleton()) {
		level_editor->_unregister_view(this);
	}
	if (active_tool.is_valid()) {
		active_tool->deactivate();
		active_tool.unref();
	}
	for (Ref<LevelEditorTool> &tool : tools) {
		tool.unref();
	}
	// Object teardown automatically removes inbound signal connections. Avoid
	// querying the document model here: editor shutdown may already be deleting it.
	if (selection_overlay) {
		memdelete(selection_overlay);
		selection_overlay = nullptr;
	}
	_end_freelook();
	if (grid_instance.is_valid()) {
		if (RenderingServer *rs = RenderingServer::get_singleton()) {
			rs->instance_set_scenario(grid_instance, RID());
			rs->free_rid(grid_instance);
		}
		grid_instance = RID();
	}
	grid_attached = false;
	if (gizmo_layer_allocated) {
		if (Node3DEditor *node_3d_editor = Node3DEditor::get_singleton()) {
			node_3d_editor->free_gizmo_layer(document->get_world_3d(), gizmo_layer);
		}
		gizmo_layer_allocated = false;
	}
}
