/**************************************************************************/
/*  level_editor_view.cpp                                                 */
/**************************************************************************/
/*  G-Level LE0: per-pane VIEW state for a LevelDocument.                 */
/*  Grid RIDs follow create-detached -> deferred bind -> free-in-dtor.     */
/**************************************************************************/

#include "level_editor_view.h"

#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/object/object.h"
#include "core/templates/hash_set.h"
#include "editor/editor_data.h"
#include "editor/editor_document.h"
#include "editor/editor_node.h"
#include "editor/editor_string_names.h"
#include "editor/gui/editor_toaster.h"
#include "editor/level/block_tool.h"
#include "editor/level/fast_texture_overlay.h"
#include "editor/level/select_tool.h"
#include "editor/level/selection_highlight_overlay.h"
#include "editor/level/selection_model.h"
#include "editor/scene/3d/node_3d_editor_plugin.h"
#include "editor/settings/editor_settings.h"
#include "editor/themes/editor_scale.h"
#include "scene/3d/camera_3d.h"
#include "scene/gui/box_container.h"
#include "scene/gui/button.h"
#include "scene/gui/check_button.h"
#include "scene/gui/grid_container.h"
#include "scene/gui/label.h"
#include "scene/gui/line_edit.h"
#include "scene/gui/margin_container.h"
#include "scene/gui/option_button.h"
#include "scene/gui/panel_container.h"
#include "scene/gui/scroll_container.h"
#include "scene/gui/separator.h"
#include "scene/gui/spin_box.h"
#include "scene/gui/subviewport_container.h"
#include "scene/gui/text_edit.h"
#include "scene/gui/texture_rect.h"
#include "scene/main/viewport.h"
#include "scene/main/window.h"
#include "scene/resources/style_box_flat.h"
#include "servers/rendering/rendering_server.h"

#include "modules/level_kernel/hotspot_atlas.h"
#include "modules/level_kernel/hotspot_fitter.h"
#include "modules/level_kernel/level_block.h"
#include "modules/level_kernel/level_mesh.h"
#include "modules/level_kernel/level_mesh_data.h"

namespace {

struct HotspotFaceGroup {
	LevelBlock *block = nullptr;
	Ref<LevelMesh> mesh;
	PackedInt32Array face_ids;
};

HotspotFaceGroup *find_or_add_hotspot_group(Vector<HotspotFaceGroup> &r_groups, LevelBlock *p_block, const Ref<LevelMesh> &p_mesh) {
	for (HotspotFaceGroup &group : r_groups) {
		if (group.block == p_block) {
			return &group;
		}
	}
	HotspotFaceGroup group;
	group.block = p_block;
	group.mesh = p_mesh;
	r_groups.push_back(group);
	return &r_groups.write[r_groups.size() - 1];
}

bool collect_hotspot_face_groups(SelectionModel *p_selection_model, Vector<HotspotFaceGroup> &r_groups) {
	ERR_FAIL_NULL_V(p_selection_model, false);
	for (const SelectionModel::Element &element : p_selection_model->get_selected(SelectionModel::FEATURE_FACE)) {
		LevelBlock *block = nullptr;
		Ref<LevelMesh> mesh;
		int face_id = -1;
		if (!p_selection_model->resolve(element, block, mesh, face_id) || !block || mesh.is_null()) {
			continue;
		}
		HotspotFaceGroup *group = find_or_add_hotspot_group(r_groups, block, mesh);
		if (!group->face_ids.has(face_id)) {
			group->face_ids.push_back(face_id);
		}
	}
	for (HotspotFaceGroup &group : r_groups) {
		group.face_ids.sort();
	}
	return !r_groups.is_empty();
}

real_t hotspot_editor_setting(const StringName &p_name, real_t p_default) {
	EditorSettings *settings = EditorSettings::get_singleton();
	if (!settings || !settings->has_setting(p_name)) {
		return p_default;
	}
	const Variant value = settings->get(p_name);
	if (value.get_type() != Variant::FLOAT && value.get_type() != Variant::INT) {
		return p_default;
	}
	const real_t converted = value;
	return Math::is_finite(converted) ? converted : p_default;
}

int64_t hotspot_editor_seed() {
	EditorSettings *settings = EditorSettings::get_singleton();
	if (!settings || !settings->has_setting("level_editor/hotspot/random_seed")) {
		return 0;
	}
	const Variant value = settings->get("level_editor/hotspot/random_seed");
	return value.get_type() == Variant::INT ? int64_t(value) : 0;
}

Color hotspot_preview_color(const String &p_name, int p_island) {
	const uint32_t hash = p_name.is_empty() ? uint32_t(p_island * 2654435761U) : p_name.hash();
	return Color::from_hsv(float(hash % 360U) / 360.0f, 0.72f, 1.0f, 0.34f);
}

Color hotspot_decision_color(const String &p_decided_by) {
	if (p_decided_by == "density-unique") {
		return Color(0.24, 0.92, 0.38, 0.38);
	}
	if (p_decided_by == "aspect") {
		return Color(1.0, 0.66, 0.12, 0.42);
	}
	return Color(1.0, 0.18, 0.16, 0.44);
}

} // namespace

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

void LevelEditorView::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_context_panel_state"), &LevelEditorView::get_context_panel_state);
	ClassDB::bind_method(D_METHOD("set_context_panel_state", "state"), &LevelEditorView::set_context_panel_state);
	ClassDB::bind_method(D_METHOD("request_materials_drawer_reveal", "focus_search"), &LevelEditorView::request_materials_drawer_reveal, DEFVAL(false));

	BIND_ENUM_CONSTANT(MATERIALS_DRAWER_TOGGLE);
	BIND_ENUM_CONSTANT(MATERIALS_DRAWER_REVEAL_ACTIVE);
	ADD_SIGNAL(MethodInfo("materials_drawer_requested",
			PropertyInfo(Variant::INT, "request"),
			PropertyInfo(Variant::BOOL, "focus_search")));
}

void LevelEditorView::_create_grid() {
	grid_material.instantiate();
	grid_material->set_shading_mode(BaseMaterial3D::SHADING_MODE_UNSHADED);
	grid_material->set_transparency(BaseMaterial3D::TRANSPARENCY_ALPHA);
	grid_material->set_flag(BaseMaterial3D::FLAG_ALBEDO_FROM_VERTEX_COLOR, true);

	grid_mesh.instantiate();
	grid_mesh->surface_begin(Mesh::PRIMITIVE_LINES, grid_material);
	constexpr int half_extent = 512;
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
	const LevelEditor *level_editor = LevelEditor::get_singleton();
	const real_t grid_step = level_editor ? level_editor->get_snap_step() : LevelEditor::DEFAULT_SNAP_STEP;
	const real_t major_grid_step = grid_step * LevelEditor::MAJOR_GRID_MULTIPLE;
	Transform3D grid_transform;
	grid_transform.basis = Basis().scaled(Vector3(grid_step, 1.0, grid_step));
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

void LevelEditorView::_register_icon_button(Button *p_button, const StringName &p_icon_name) {
	ERR_FAIL_NULL(p_button);
	p_button->set_text_alignment(HORIZONTAL_ALIGNMENT_CENTER);
	if (p_button->get_text().is_empty()) {
		p_button->set_icon_alignment(HORIZONTAL_ALIGNMENT_CENTER);
	}
	icon_buttons.push_back(p_button);
	icon_button_names.push_back(p_icon_name);
}

void LevelEditorView::_update_ui_theme() {
	Control *theme_source = is_inside_tree() ? this : (EditorNode::get_singleton() ? EditorNode::get_singleton()->get_gui_base() : nullptr);
	if (!theme_source) {
		return;
	}
	for (int i = 0; i < icon_buttons.size(); i++) {
		Button *button = icon_buttons[i];
		if (button) {
			button->set_button_icon(theme_source->get_theme_icon(icon_button_names[i], EditorStringName(EditorIcons)));
		}
	}
	static const StringName button_states[] = {
		SNAME("normal"), SNAME("hover"), SNAME("pressed"), SNAME("disabled"), SNAME("focus")
	};
	for (Button *button : compact_context_buttons) {
		if (!button) {
			continue;
		}
		button->add_theme_font_size_override(SceneStringName(font_size), 12 * EDSCALE);
		for (const StringName &state : button_states) {
			Ref<StyleBox> source = theme_source->get_theme_stylebox(state, SNAME("Button"));
			if (source.is_null()) {
				continue;
			}
			Ref<StyleBox> compact = source->duplicate();
			compact->set_content_margin(SIDE_TOP, 2 * EDSCALE);
			compact->set_content_margin(SIDE_BOTTOM, 2 * EDSCALE);
			button->add_theme_style_override(state, compact);
		}
	}
	if (grid_step_decrease_button && grid_step_increase_button) {
		const Color base = theme_source->get_theme_color(SNAME("dark_color_1"), EditorStringName(Editor));
		const Color accent = theme_source->get_theme_color(SNAME("accent_color"), EditorStringName(Editor));
		const Color border = theme_source->get_theme_color(SNAME("contrast_color_1"), EditorStringName(Editor));
		auto make_circle = [&](const Color &p_background) {
			Ref<StyleBoxFlat> style;
			style.instantiate();
			style->set_bg_color(p_background);
			style->set_border_width_all(MAX(1, int(EDSCALE)));
			style->set_border_color(border);
			style->set_corner_radius_all(9 * EDSCALE);
			style->set_content_margin_all(0);
			return style;
		};
		for (Button *button : { grid_step_decrease_button, grid_step_increase_button }) {
			button->add_theme_style_override(SNAME("normal"), make_circle(base));
			button->add_theme_style_override(SNAME("hover"), make_circle(base.lerp(accent, 0.24)));
			button->add_theme_style_override(SNAME("pressed"), make_circle(base.lerp(accent, 0.42)));
			button->add_theme_style_override(SNAME("focus"), make_circle(base.lerp(accent, 0.24)));
			button->add_theme_font_size_override(SceneStringName(font_size), 11 * EDSCALE);
		}
	}
	if (context_panel_toggle_button) {
		context_panel_toggle_button->set_icon_alignment(HORIZONTAL_ALIGNMENT_CENTER);
		context_panel_toggle_button->set_button_icon(theme_source->get_theme_icon(
				context_panel_expanded ? SNAME("GuiTreeArrowLeft") : SNAME("GuiTreeArrowRight"),
				EditorStringName(EditorIcons)));
	}
}

void LevelEditorView::_context_panel_toggle_pressed() {
	_set_context_panel_expanded(!context_panel_expanded);
}

void LevelEditorView::_set_context_panel_expanded(bool p_expanded) {
	context_panel_expanded = p_expanded;
	if (context_panel) {
		context_panel->set_visible(p_expanded);
	}
	if (context_panel_separator) {
		context_panel_separator->set_visible(p_expanded);
	}
	if (!p_expanded && selection_overlay) {
		selection_overlay->set_face_highlight_dimmed(false);
	}
	if (context_panel_toggle_button) {
		context_panel_toggle_button->set_pressed_no_signal(p_expanded);
		context_panel_toggle_button->set_tooltip_text(p_expanded ?
				TTR("Collapse contextual options") : TTR("Expand contextual options"));
		context_panel_toggle_button->set_accessibility_name(p_expanded ?
				TTR("Collapse contextual options") : TTR("Expand contextual options"));
	}
	set_meta(SNAME("_level_context_panel_expanded"), p_expanded);
	_update_ui_theme();
}

void LevelEditorView::_texture_context_mouse_entered() {
	texture_context_hovered = true;
	if (selection_overlay && texture_context && texture_context->is_visible_in_tree()) {
		selection_overlay->set_face_highlight_dimmed(true);
	}
}

void LevelEditorView::_texture_context_mouse_exited() {
	texture_context_hovered = false;
	if (selection_overlay) {
		selection_overlay->set_face_highlight_dimmed(false);
	}
}

int LevelEditorView::_get_selected_level_block_count() const {
	EditorSelection *selection = document ? document->get_selection() : nullptr;
	if (!selection) {
		return 0;
	}
	int count = 0;
	const TypedArray<Node> selected_nodes = selection->get_selected_nodes();
	for (int i = 0; i < selected_nodes.size(); i++) {
		if (Object::cast_to<LevelBlock>(selected_nodes[i])) {
			count++;
		}
	}
	return count;
}

void LevelEditorView::_refresh_context_panel() {
	if (!fast_texture_context || !block_tool_context || !texture_context || !selection_hint_context) {
		return;
	}

	fast_texture_context->hide();
	block_tool_context->hide();
	texture_context->hide();
	selection_hint_context->hide();
	if (selection_overlay) {
		selection_overlay->set_face_highlight_dimmed(false);
	}

	if (fast_texture_overlay) {
		fast_texture_context->show();
		if (context_title) {
			context_title->set_text(TTR("Fast Texture"));
		}
		set_meta(SNAME("_level_context_kind"), SNAME("fast_texture"));
		return;
	}

	LevelEditor *level_editor = LevelEditor::get_singleton();
	if (tool_mode == LevelEditor::TOOL_BLOCK) {
		block_tool_context->show();
		if (context_title) {
			context_title->set_text(TTR("Block"));
		}
		if (block_snap_enabled && level_editor) {
			block_snap_enabled->set_pressed_no_signal(level_editor->is_snap_enabled());
		}
		if (block_snap_step && level_editor) {
			block_snap_step->set_value_no_signal(level_editor->get_snap_step());
		}
		if (block_material_label && active_material_label) {
			block_material_label->set_text(active_material_label->get_text());
		}
		set_meta(SNAME("_level_context_kind"), SNAME("block"));
		return;
	}

	SelectionModel *selection = document ? document->get_selection_model() : nullptr;
	const int face_count = selection ? selection->get_count(SelectionModel::FEATURE_FACE) : 0;
	const int block_count = _get_selected_level_block_count();
	const bool face_context = selection && selection->get_mode() == SelectionModel::MODE_FACE && face_count > 0;
	const bool block_context = selection && selection->get_mode() == SelectionModel::MODE_OBJECT && block_count > 0;
	if (face_context || block_context) {
		texture_context->show();
		if (selection_overlay && texture_context_hovered) {
			selection_overlay->set_face_highlight_dimmed(true);
		}
		if (context_title) {
			context_title->set_text(TTR("Face Texture"));
		}
		if (texture_scope_label) {
			texture_scope_label->set_text(face_context ?
					vformat(TTRN("%d selected face", "%d selected faces", face_count), face_count) :
					vformat(TTRN("All faces of %d object", "All faces of %d objects", block_count), block_count));
		}
		if (captured_mapping_label && level_editor) {
			const Dictionary captured = level_editor->get_captured_mapping(document);
			if (captured.is_empty()) {
				captured_mapping_label->set_text(TTR("Lift: no captured mapping"));
			} else if (bool(captured.get("has_mapping", false))) {
				captured_mapping_label->set_text(TTR("Lift: material + mapping captured"));
			} else {
				captured_mapping_label->set_text(TTR("Lift: material captured (explicit UVs)"));
			}
		}
		if (hotspot_mapping_mode && level_editor) {
			const int target_id = level_editor->get_hotspot_mapping_mode_override(document);
			const int target_index = hotspot_mapping_mode->get_item_index(target_id);
			if (target_index >= 0) {
				hotspot_mapping_mode->select(target_index);
			}
		}
		if (hotspot_context) {
			bool hotspot_actionable = false;
			auto material_has_hotspot = [&](const String &p_material_path) {
				return level_editor && !p_material_path.is_empty() && !level_editor->resolve_hotspot_atlas(p_material_path).is_empty();
			};
			if (face_context) {
				for (const SelectionModel::Element &element : selection->get_selected(SelectionModel::FEATURE_FACE)) {
					LevelBlock *block = nullptr;
					Ref<LevelMesh> mesh;
					int face_id = -1;
					if (!selection->resolve(element, block, mesh, face_id) || mesh.is_null()) {
						continue;
					}
					const Ref<LevelMeshData> mesh_data = mesh->get_data();
					const PackedInt32Array material_indices = mesh_data.is_valid() ? mesh_data->get_face_material_indices() : PackedInt32Array();
					if (face_id >= 0 && face_id < material_indices.size() && material_has_hotspot(mesh_data->get_material_path(material_indices[face_id]))) {
						hotspot_actionable = true;
						break;
					}
				}
			} else if (EditorSelection *object_selection = document->get_selection()) {
				const TypedArray<Node> nodes = object_selection->get_selected_nodes();
				for (int i = 0; i < nodes.size() && !hotspot_actionable; i++) {
					LevelBlock *block = Object::cast_to<LevelBlock>(nodes[i]);
					const Ref<LevelMeshData> mesh_data = block ? block->get_data() : Ref<LevelMeshData>();
					if (mesh_data.is_null()) {
						continue;
					}
					for (const String &path : mesh_data->get_material_paths()) {
						if (material_has_hotspot(path)) {
							hotspot_actionable = true;
							break;
						}
					}
				}
			}
			hotspot_context->set_visible(hotspot_actionable);
			set_meta(SNAME("_level_hotspot_context_actionable"), hotspot_actionable);
		}
		set_meta(SNAME("_level_context_kind"), face_context ? SNAME("faces") : SNAME("objects"));
		return;
	}

	selection_hint_context->show();
	if (context_title) {
		context_title->set_text(TTR("Selection"));
	}
	if (selection_hint_label) {
		if (!selection) {
			selection_hint_label->set_text(TTR("Select a LevelBlock or face to edit its material and UVs."));
		} else {
			switch (selection->get_mode()) {
				case SelectionModel::MODE_VERTEX:
					selection_hint_label->set_text(TTR("Vertex selection is active. Choose Face mode to edit materials and UVs."));
					break;
				case SelectionModel::MODE_EDGE:
					selection_hint_label->set_text(TTR("Edge selection is active. Choose Face mode to edit materials and UVs."));
					break;
				case SelectionModel::MODE_FACE:
					selection_hint_label->set_text(TTR("Select one or more faces to show material and UV tools."));
					break;
				case SelectionModel::MODE_OBJECT:
					selection_hint_label->set_text(TTR("Select one or more LevelBlock objects to edit all of their faces."));
					break;
			}
		}
	}
	set_meta(SNAME("_level_context_kind"), SNAME("selection_hint"));
}

void LevelEditorView::_modify_texture_pressed(int p_operation, const Vector2 &p_value) {
	modify_selected_texture(p_operation, p_value);
}

void LevelEditorView::_hotspot_mapping_mode_selected(int p_index) {
	LevelEditor *level_editor = LevelEditor::get_singleton();
	if (level_editor && hotspot_mapping_mode && p_index >= 0 && p_index < hotspot_mapping_mode->get_item_count()) {
		level_editor->set_hotspot_mapping_mode_override(document, hotspot_mapping_mode->get_item_id(p_index));
	}
}

bool LevelEditorView::_invoke_select_tool_shortcut(Key p_key, bool p_shift, bool p_ctrl, bool p_alt) {
	Ref<InputEventKey> event;
	event.instantiate();
	event->set_pressed(true);
	event->set_keycode(p_key);
	event->set_physical_keycode(p_key);
	event->set_shift_pressed(p_shift);
	event->set_ctrl_pressed(p_ctrl);
	event->set_alt_pressed(p_alt);
	Ref<LevelEditorTool> select_tool = tools[LevelEditor::TOOL_SELECT];
	return select_tool.is_valid() && select_tool->handle_input(camera, event);
}

void LevelEditorView::_hotspot_fit_pressed(bool p_individual) {
	_invoke_select_tool_shortcut(p_individual ? Key::F : Key::H, true);
}

void LevelEditorView::_hotspot_debug_toggled(bool p_enabled) {
	set_hotspot_fit_debug_enabled(p_enabled);
}

void LevelEditorView::_block_snap_toggled(bool p_enabled) {
	if (LevelEditor *level_editor = LevelEditor::get_singleton()) {
		level_editor->set_snap_enabled(p_enabled);
	}
}

void LevelEditorView::_block_snap_step_changed(double p_value) {
	if (LevelEditor *level_editor = LevelEditor::get_singleton()) {
		level_editor->set_snap_step(p_value);
	}
}

void LevelEditorView::_grid_step_decrease_pressed() {
	if (LevelEditor *level_editor = LevelEditor::get_singleton()) {
		level_editor->set_snap_step(MAX((real_t)0.125, level_editor->get_snap_step() * (real_t)0.5));
	}
}

void LevelEditorView::_grid_step_increase_pressed() {
	if (LevelEditor *level_editor = LevelEditor::get_singleton()) {
		level_editor->set_snap_step(MIN((real_t)64.0, level_editor->get_snap_step() * (real_t)2.0));
	}
}

void LevelEditorView::_snap_settings_changed(double p_step, bool p_enabled) {
	const real_t step = (real_t)p_step;
	if (grid_step_label) {
		grid_step_label->set_text(vformat(TTR("Grid: %s m / major %s m"),
				String::num_real(step, false),
				String::num_real(step * LevelEditor::MAJOR_GRID_MULTIPLE, false)));
	}
	if (block_snap_enabled) {
		block_snap_enabled->set_pressed_no_signal(p_enabled);
	}
	if (block_snap_step) {
		block_snap_step->set_value_no_signal(step);
	}
	_update_grid_transform();
}

void LevelEditorView::_fast_texture_accept_pressed() {
	_close_fast_texture(true);
}

void LevelEditorView::_fast_texture_cancel_pressed() {
	_close_fast_texture(false);
}

void LevelEditorView::_emit_materials_drawer_request(MaterialsDrawerRequest p_request, bool p_focus_search) {
	emit_signal(SNAME("materials_drawer_requested"), int(p_request), p_focus_search);
}

void LevelEditorView::_active_material_swatch_pressed() {
	_emit_materials_drawer_request(MATERIALS_DRAWER_REVEAL_ACTIVE, true);
}

void LevelEditorView::request_materials_drawer_reveal(bool p_focus_search) {
	_emit_materials_drawer_request(MATERIALS_DRAWER_REVEAL_ACTIVE, p_focus_search);
}

bool LevelEditorView::_try_toggle_materials_shortcut(const Ref<InputEvent> &p_event) {
	Ref<InputEventKey> key = p_event;
	if (key.is_null() || !key->is_pressed() || key->is_echo() || get_last_exclusive_window() != get_window() ||
			!ED_IS_SHORTCUT("level_editor/toggle_materials", p_event)) {
		return false;
	}
	Control *focus_owner = get_viewport() ? get_viewport()->gui_get_focus_owner() : nullptr;
	if (Object::cast_to<LineEdit>(focus_owner) || Object::cast_to<TextEdit>(focus_owner)) {
		return false;
	}
	_emit_materials_drawer_request(MATERIALS_DRAWER_TOGGLE, true);
	return true;
}

void LevelEditorView::_tool_button_pressed(int p_mode) {
	LevelEditor *level_editor = LevelEditor::get_singleton();
	ERR_FAIL_NULL(level_editor);
	level_editor->set_tool_mode(this, LevelEditor::ToolMode(p_mode));
}

void LevelEditorView::set_tool_mode(LevelEditor::ToolMode p_mode) {
	ERR_FAIL_INDEX(int(p_mode), 2);
	if (fast_texture_overlay) {
		_close_fast_texture(false, false);
	}
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
	tool_mode = p_mode;
	set_meta(StringName("_level_tool_mode"), int(p_mode));
	_refresh_context_panel();
}

void LevelEditorView::_active_material_changed(const Ref<Material> &p_material, const String &p_path) {
	LevelEditor *level_editor = LevelEditor::get_singleton();
	if (!level_editor || !active_material_preview || !active_material_label) {
		return;
	}
	const String display_name = level_editor->get_active_material_display_name(document);
	active_material_label->set_text(display_name);
	active_material_label->set_tooltip_text(display_name);
	if (active_material_swatch) {
		active_material_swatch->set_tooltip_text(vformat(TTR("Active material: %s\nOpen Materials and reveal it"), display_name));
		active_material_swatch->set_accessibility_name(vformat(TTR("Active material %s; open Materials"), display_name));
	}
	set_meta(StringName("_level_active_material_path"), p_path);
	set_meta(StringName("_level_active_material_name"), display_name);
	Ref<Texture2D> fallback = level_editor->get_material_albedo_texture(p_material, p_path);
	active_material_preview->set_texture(fallback);
	set_meta(StringName("_level_active_material_texture"), fallback);
	if (!p_path.is_empty()) {
		level_editor->request_material_preview(p_path, callable_mp(this, &LevelEditorView::_active_material_preview_ready));
	}
	_refresh_context_panel();
}

void LevelEditorView::_active_material_changed_for_document(int64_t p_document_history_id, const Ref<Material> &p_material, const String &p_path) {
	if (!document || p_document_history_id != document->get_history_id()) {
		return;
	}
	_active_material_changed(p_material, p_path);
}

void LevelEditorView::_active_material_preview_ready(const String &p_path, const Ref<Texture2D> &p_texture) {
	LevelEditor *level_editor = LevelEditor::get_singleton();
	if (!level_editor || p_path != level_editor->get_active_material_path(document) || !active_material_preview) {
		return;
	}
	if (p_texture.is_valid()) {
		active_material_preview->set_texture(p_texture);
		set_meta(StringName("_level_active_material_texture"), p_texture);
	}
}

bool LevelEditorView::_try_activate_blockout_shortcut(const Ref<InputEvent> &p_event) {
	if (!viewport_container || !viewport_container->has_focus() || get_last_exclusive_window() != get_window()) {
		return false;
	}
	Control *focus_owner = get_viewport() ? get_viewport()->gui_get_focus_owner() : nullptr;
	if (Object::cast_to<LineEdit>(focus_owner) || Object::cast_to<TextEdit>(focus_owner)) {
		return false;
	}
	Ref<InputEventKey> key = p_event;
	if (key.is_null() || !key->is_pressed() || key->is_echo()) {
		return false;
	}
	static const char *blockout_shortcuts[10] = {
		"level_editor/blockout_material_1", "level_editor/blockout_material_2",
		"level_editor/blockout_material_3", "level_editor/blockout_material_4",
		"level_editor/blockout_material_5", "level_editor/blockout_material_6",
		"level_editor/blockout_material_7", "level_editor/blockout_material_8",
		"level_editor/blockout_material_9", "level_editor/blockout_material_0"
	};
	for (int slot = 0; slot < 10; slot++) {
		if (!ED_IS_SHORTCUT(blockout_shortcuts[slot], p_event)) {
			continue;
		}
		if (LevelEditor *level_editor = LevelEditor::get_singleton()) {
			if (level_editor->activate_blockout_slot(document, slot)) {
				set_meta(StringName("_level_blockout_shortcut_slot"), slot);
				apply_active_material_to_selection();
				return true;
			}
		}
		return false;
	}
	return false;
}

void LevelEditorView::_show_fast_texture_status(const String &p_message, bool p_warning) {
	set_meta(StringName("_level_fast_texture_status"), p_message);
	if (EditorToaster *toaster = EditorToaster::get_singleton()) {
		toaster->popup_str(p_message, p_warning ? EditorToaster::SEVERITY_WARNING : EditorToaster::SEVERITY_INFO);
	}
}

bool LevelEditorView::_try_open_fast_texture_shortcut(const Ref<InputEvent> &p_event) {
	Ref<InputEventKey> key = p_event;
	if (key.is_null() || !key->is_pressed() || key->is_echo() || !key->is_shift_pressed() ||
			key->is_ctrl_pressed() || key->is_alt_pressed() || key->is_meta_pressed()) {
		return false;
	}
	const Key code = key->get_keycode() != Key::NONE ? key->get_keycode() : key->get_physical_keycode();
	if (code != Key::Q) {
		return false;
	}
	_open_fast_texture();
	return true;
}

bool LevelEditorView::_open_fast_texture() {
	if (fast_texture_overlay) {
		fast_texture_overlay->grab_focus();
		return true;
	}
	SelectionModel *selection = document ? document->get_selection_model() : nullptr;
	if (!selection || selection->get_mode() != SelectionModel::MODE_FACE) {
		_show_fast_texture_status(TTR("Fast Texture requires Face selection mode (3)."), true);
		return false;
	}
	if (selection->get_selected(SelectionModel::FEATURE_FACE).is_empty()) {
		_show_fast_texture_status(TTR("Fast Texture needs at least one selected face."), true);
		return false;
	}
	if (active_tool.is_valid() && active_tool->has_active_gesture()) {
		_show_fast_texture_status(TTR("Finish or cancel the active level-editor gesture first."), true);
		return false;
	}

	_end_freelook();
	orbiting = false;
	panning = false;
	FastTextureOverlay *overlay = memnew(FastTextureOverlay(this));
	viewport_container->add_child(overlay);
	if (!overlay->open_from_selection()) {
		_show_fast_texture_status(TTR("Fast Texture could not open the selected faces."), true);
		viewport_container->remove_child(overlay);
		memdelete(overlay);
		return false;
	}
	fast_texture_overlay = overlay;
	overlay->connect(SceneStringName(focus_exited), callable_mp(this, &LevelEditorView::_fast_texture_focus_exited));
	_push_fast_texture_input_context();
	set_meta(StringName("_level_fast_texture_open"), true);
	set_meta(StringName("_level_fast_texture_session"), overlay->get_primary_session());
	set_meta(StringName("_level_fast_texture_session_count"), overlay->get_session_count());
	set_meta(StringName("_level_fast_texture_status"), TTR("Fast Texture opened."));
	set_last_selection_action(SNAME("fast_texture_open"));
	_refresh_context_panel();
	return true;
}

void LevelEditorView::_push_fast_texture_input_context() {
	if (fast_texture_input_context_active || !fast_texture_overlay) {
		return;
	}
	Control *focus_owner = get_viewport() ? get_viewport()->gui_get_focus_owner() : nullptr;
	fast_texture_previous_focus_owner = focus_owner ? focus_owner->get_instance_id() : ObjectID();
	fast_texture_overlay->set_shortcut_context(fast_texture_overlay);
	fast_texture_overlay->grab_focus();
	fast_texture_input_context_active = true;
	set_meta(StringName("_level_fast_texture_input_context"), true);
}

void LevelEditorView::_pop_fast_texture_input_context(bool p_restore_focus) {
	if (!fast_texture_input_context_active) {
		return;
	}
	if (fast_texture_overlay) {
		fast_texture_overlay->set_shortcut_context(nullptr);
	}
	fast_texture_input_context_active = false;
	set_meta(StringName("_level_fast_texture_input_context"), false);
	if (p_restore_focus) {
		Control *restore = Object::cast_to<Control>(ObjectDB::get_instance(fast_texture_previous_focus_owner));
		if (!restore || restore == fast_texture_overlay || !restore->is_inside_tree()) {
			restore = viewport_container;
		}
		if (restore) {
			restore->grab_focus();
		}
	}
	fast_texture_previous_focus_owner = ObjectID();
}

void LevelEditorView::_close_fast_texture(bool p_accept, bool p_restore_focus) {
	if (!fast_texture_overlay || fast_texture_closing) {
		return;
	}
	fast_texture_closing = true;
	FastTextureOverlay *overlay = fast_texture_overlay;
	if (p_accept) {
		if (!overlay->accept()) {
			fast_texture_closing = false;
			overlay->grab_focus();
			return;
		}
		set_last_selection_action(SNAME("fast_texture_accept"));
		set_meta(StringName("_level_fast_texture_status"), TTR("Fast Texture accepted."));
	} else {
		overlay->cancel();
		set_last_selection_action(SNAME("fast_texture_cancel"));
		set_meta(StringName("_level_fast_texture_status"), TTR("Fast Texture canceled."));
	}
	_pop_fast_texture_input_context(p_restore_focus);
	fast_texture_overlay = nullptr;
	remove_meta(StringName("_level_fast_texture_session"));
	set_meta(StringName("_level_fast_texture_session_count"), 0);
	set_meta(StringName("_level_fast_texture_open"), false);
	overlay->set_mouse_filter(MOUSE_FILTER_IGNORE);
	overlay->hide();
	overlay->queue_free();
	fast_texture_closing = false;
	_refresh_context_panel();
}

void LevelEditorView::_fast_texture_focus_exited() {
	if (!fast_texture_closing) {
		callable_mp(this, &LevelEditorView::_cancel_fast_texture_if_unfocused).call_deferred();
	}
}

void LevelEditorView::_cancel_fast_texture_if_unfocused() {
	if (fast_texture_overlay && !fast_texture_overlay->has_focus()) {
		_close_fast_texture(false, false);
	}
}

bool LevelEditorView::apply_active_material_to_selection() {
	SelectTool *select_tool = Object::cast_to<SelectTool>(tools[LevelEditor::TOOL_SELECT].ptr());
	return select_tool && select_tool->apply_active_material();
}

bool LevelEditorView::modify_selected_texture(int p_operation, const Vector2 &p_value) {
	SelectTool *select_tool = Object::cast_to<SelectTool>(tools[LevelEditor::TOOL_SELECT].ptr());
	return select_tool && select_tool->modify_selected_texture(p_operation, p_value);
}

void LevelEditorView::shortcut_input(const Ref<InputEvent> &p_event) {
	EditorNode *editor_node = EditorNode::get_singleton();
	if (!context_active || !is_visible_in_tree() || !editor_node || editor_node->get_editor_data().get_active_document() != document) {
		return;
	}
	if (fast_texture_overlay) {
		fast_texture_overlay->handle_modal_input(p_event);
		accept_event();
		return;
	}
	if (_try_toggle_materials_shortcut(p_event)) {
		accept_event();
		return;
	}
	// Selection/tool shortcuts are pane-local. Do not steal numeric or editing
	// keys while the scene tree, Inspector, or another editor control has focus.
	if (!viewport_container || !viewport_container->has_focus()) {
		return;
	}
	if (get_last_exclusive_window() != get_window()) {
		return;
	}
	if (_try_activate_blockout_shortcut(p_event)) {
		accept_event();
		return;
	}
	if (_try_open_fast_texture_shortcut(p_event)) {
		accept_event();
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
	if (fast_texture_overlay) {
		fast_texture_overlay->handle_modal_input(p_event);
		viewport_container->accept_event();
		return;
	}
	if (_try_toggle_materials_shortcut(p_event)) {
		viewport_container->accept_event();
		return;
	}
	if (_try_open_fast_texture_shortcut(p_event)) {
		viewport_container->accept_event();
		return;
	}
	if (_try_activate_blockout_shortcut(p_event)) {
		viewport_container->accept_event();
		return;
	}
	Ref<InputEventMouseButton> button = p_event;
	if (button.is_valid()) {
		const MouseButton index = button->get_button_index();
		if (index == MouseButton::RIGHT) {
			if (active_tool.is_valid() && active_tool->handle_input(camera, p_event)) {
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
	_refresh_context_panel();
	_update_transform_gizmo();
	if (LevelEditor *level_editor = LevelEditor::get_singleton()) {
		level_editor->notify_level_view_selection_changed(this);
	} else if (hotspot_fit_debug_enabled) {
		_refresh_hotspot_fit_overlay();
	}
}

void LevelEditorView::_object_selection_changed() {
	_update_transform_gizmo();
	_refresh_context_panel();
}

void LevelEditorView::_update_transform_gizmo() {
	SelectTool *select_tool = Object::cast_to<SelectTool>(tools[LevelEditor::TOOL_SELECT].ptr());
	if (select_tool) {
		select_tool->update_transform_gizmo(camera);
	}
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

bool LevelEditorView::has_hotspot_face_selection() const {
	SelectionModel *selection_model = document ? document->get_selection_model() : nullptr;
	return selection_model && selection_model->get_count(SelectionModel::FEATURE_FACE) > 0;
}

bool LevelEditorView::show_hotspot_preview(ObjectID p_owner, const Ref<HotspotAtlas> &p_atlas) {
	if (p_owner.is_null() || p_atlas.is_null() || !has_hotspot_face_selection()) {
		clear_hotspot_preview(p_owner);
		return false;
	}
	hotspot_preview_owner = p_owner;
	hotspot_preview_atlas = p_atlas;
	hotspot_preview_active = true;
	set_meta(StringName("_level_hotspot_preview_enabled"), true);
	return _run_hotspot_preview();
}

void LevelEditorView::clear_hotspot_preview(ObjectID p_owner) {
	if (!p_owner.is_null() && !hotspot_preview_owner.is_null() && p_owner != hotspot_preview_owner) {
		return;
	}
	hotspot_preview_owner = ObjectID();
	hotspot_preview_atlas.unref();
	hotspot_preview_diagnostics.clear();
	hotspot_preview_active = false;
	set_meta(StringName("_level_hotspot_preview_enabled"), false);
	if (hotspot_fit_debug_enabled) {
		_refresh_hotspot_fit_overlay();
	} else {
		_clear_hotspot_fit_overlay();
	}
}

void LevelEditorView::set_hotspot_fit_debug_enabled(bool p_enabled) {
	hotspot_fit_debug_enabled = p_enabled;
	if (hotspot_debug_toggle) {
		hotspot_debug_toggle->set_pressed_no_signal(p_enabled);
	}
	set_meta(StringName("_level_hotspot_fit_debug_enabled"), p_enabled);
	if (p_enabled || hotspot_preview_active) {
		_refresh_hotspot_fit_overlay();
	} else {
		_clear_hotspot_fit_overlay();
	}
}

Dictionary LevelEditorView::get_context_panel_state() const {
	Dictionary state;
	state["expanded"] = context_panel_expanded;
	return state;
}

void LevelEditorView::set_context_panel_state(const Dictionary &p_state) {
	_set_context_panel_expanded(bool(p_state.get("expanded", true)));
}

bool LevelEditorView::_run_hotspot_preview() {
	hotspot_preview_diagnostics.clear();
	SelectionModel *selection_model = document ? document->get_selection_model() : nullptr;
	Vector<HotspotFaceGroup> groups;
	const Size2i texture_size = hotspot_preview_atlas.is_valid() ? hotspot_preview_atlas->get_reference_texture_size() : Size2i();
	if (!selection_model || !collect_hotspot_face_groups(selection_model, groups) || texture_size.x <= 0 || texture_size.y <= 0) {
		set_meta(StringName("_level_hotspot_preview_ok"), false);
		_clear_hotspot_fit_overlay();
		return false;
	}

	Ref<HotspotFitter> fitter;
	fitter.instantiate();
	bool fitted_any = false;
	for (const HotspotFaceGroup &group : groups) {
		Dictionary options;
		options["texture_size"] = texture_size;
		options["mesh_to_world"] = group.block ? group.block->get_global_transform() : Transform3D();
		const LevelEditor *level_editor = LevelEditor::get_singleton();
		const int mapping_override = level_editor ? level_editor->get_hotspot_mapping_mode_override(document) : -1;
		options["mapping_mode"] = mapping_override >= 0 ? mapping_override : hotspot_preview_atlas->get_default_mapping_mode();
		options["density_margin"] = hotspot_editor_setting("level_editor/hotspot/density_margin_octaves", HotspotFitter::DEFAULT_DENSITY_MARGIN);
		options["aspect_margin"] = hotspot_editor_setting("level_editor/hotspot/aspect_margin_octaves", HotspotFitter::DEFAULT_ASPECT_MARGIN);
		options["cos_coplanar"] = hotspot_editor_setting("level_editor/hotspot/cos_coplanar", HotspotFitter::DEFAULT_COS_COPLANAR);
		options["cos_collinear"] = hotspot_editor_setting("level_editor/hotspot/cos_collinear", HotspotFitter::DEFAULT_COS_COLLINEAR);
		options["horizontal_bias_degrees"] = hotspot_editor_setting("level_editor/hotspot/horizontal_bias_degrees", HotspotFitter::DEFAULT_HORIZONTAL_BIAS_DEGREES);
		options["inset_mip_bleed"] = hotspot_editor_setting("level_editor/hotspot/inset_mip_bleed_texels_per_level", HotspotFitter::DEFAULT_INSET_MIP_BLEED);
		options["automatic_distortion_threshold"] = Math::deg_to_rad(hotspot_editor_setting(
				"level_editor/hotspot/automatic_distortion_threshold_degrees",
				Math::rad_to_deg(LevelMesh::DEFAULT_CONFORMING_DISTORTION_THRESHOLD)));
		const Dictionary result = fitter->fit(group.face_ids, group.mesh, hotspot_preview_atlas,
				HotspotFitter::ISLAND_GROUPED, hotspot_editor_seed(), options);
		if (!bool(result.get("ok", false))) {
			continue;
		}
		const Array diagnostics = result.get("diagnostics", Array());
		for (int i = 0; i < diagnostics.size(); i++) {
			if (diagnostics[i].get_type() != Variant::DICTIONARY) {
				continue;
			}
			Dictionary diagnostic = Dictionary(diagnostics[i]).duplicate(true);
			diagnostic["block_id"] = int64_t(uint64_t(group.block->get_instance_id()));
			hotspot_preview_diagnostics.push_back(diagnostic);
		}
		fitted_any = fitted_any || !diagnostics.is_empty();
	}

	const int run_count = int(get_meta(StringName("_level_hotspot_preview_run_count"), 0)) + 1;
	set_meta(StringName("_level_hotspot_preview_run_count"), run_count);
	set_meta(StringName("_level_hotspot_preview_ok"), fitted_any);
	set_meta(StringName("_level_hotspot_preview_island_count"), hotspot_preview_diagnostics.size());
	_refresh_hotspot_fit_overlay();
	return fitted_any;
}

void LevelEditorView::_capture_committed_hotspot_diagnostics() {
	if (!document) {
		return;
	}
	SelectionModel *selection_model = document->get_selection_model();
	Vector<HotspotFaceGroup> groups;
	Array captured;
	if (selection_model && collect_hotspot_face_groups(selection_model, groups)) {
		for (const HotspotFaceGroup &group : groups) {
			const Array diagnostics = group.mesh->get_last_hotspot_fit_diagnostics();
			for (int i = 0; i < diagnostics.size(); i++) {
				if (diagnostics[i].get_type() != Variant::DICTIONARY) {
					continue;
				}
				const Dictionary source = diagnostics[i];
				const PackedInt32Array diagnostic_faces = source.get("face_ids", PackedInt32Array());
				bool selected = false;
				for (const int face_id : diagnostic_faces) {
					if (group.face_ids.has(face_id)) {
						selected = true;
						break;
					}
				}
				if (!selected) {
					continue;
				}
				Dictionary diagnostic = source.duplicate(true);
				diagnostic["block_id"] = int64_t(uint64_t(group.block->get_instance_id()));
				captured.push_back(diagnostic);
			}
		}
	}
	document->set_last_hotspot_fit_diagnostics(captured);
	set_meta(StringName("_level_hotspot_last_fit_island_count"), captured.size());
	if (hotspot_fit_debug_enabled && !hotspot_preview_active) {
		_refresh_hotspot_fit_overlay();
	}
}

void LevelEditorView::_refresh_hotspot_fit_overlay() {
	const Array diagnostics = hotspot_preview_active ? hotspot_preview_diagnostics :
			(document ? document->get_last_hotspot_fit_diagnostics() : Array());
	if (diagnostics.is_empty() || (!hotspot_preview_active && !hotspot_fit_debug_enabled)) {
		_clear_hotspot_fit_overlay();
		return;
	}

	ObjectID active_block_id;
	int active_face_id = -1;
	SelectionModel *selection_model = document ? document->get_selection_model() : nullptr;
	SelectionModel::Element active_element;
	if (selection_model && selection_model->get_active(SelectionModel::FEATURE_FACE, active_element)) {
		LevelBlock *active_block = nullptr;
		Ref<LevelMesh> active_mesh;
		if (selection_model->resolve(active_element, active_block, active_mesh, active_face_id) && active_block) {
			active_block_id = active_block->get_instance_id();
		}
	}

	Vector<Vector3> vertices;
	Vector<Color> colors;
	Dictionary hud_diagnostic;
	for (int diagnostic_index = 0; diagnostic_index < diagnostics.size(); diagnostic_index++) {
		if (diagnostics[diagnostic_index].get_type() != Variant::DICTIONARY) {
			continue;
		}
		const Dictionary diagnostic = diagnostics[diagnostic_index];
		const ObjectID block_id = ObjectID(uint64_t(int64_t(diagnostic.get("block_id", int64_t(0)))));
		LevelBlock *block = Object::cast_to<LevelBlock>(ObjectDB::get_instance(block_id));
		if (!block) {
			continue;
		}
		const Ref<LevelMesh> mesh = block->get_level_mesh();
		if (mesh.is_null()) {
			continue;
		}
		const PackedInt32Array face_ids = diagnostic.get("face_ids", PackedInt32Array());
		const String chosen = diagnostic.get("chosen", String());
		const String decided_by = diagnostic.get("decided_by", String());
		const Color color = hotspot_preview_active ? hotspot_preview_color(chosen, diagnostic.get("island_index", diagnostic_index)) :
				hotspot_decision_color(decided_by);
		const Transform3D block_xform = block->get_global_transform();
		for (const int face_id : face_ids) {
			const PackedVector3Array corners = mesh->get_face_corner_positions(face_id);
			for (int corner = 1; corner + 1 < corners.size(); corner++) {
				vertices.push_back(block_xform.xform(corners[0]));
				vertices.push_back(block_xform.xform(corners[corner]));
				vertices.push_back(block_xform.xform(corners[corner + 1]));
				colors.push_back(color);
				colors.push_back(color);
				colors.push_back(color);
			}
		}
		if (hud_diagnostic.is_empty() || (block_id == active_block_id && face_ids.has(active_face_id))) {
			hud_diagnostic = diagnostic;
		}
	}

	if (vertices.is_empty()) {
		_clear_hotspot_fit_overlay();
		return;
	}
	hotspot_fit_overlay.update_colored_faces(vertices, colors);
	hotspot_fit_overlay.set_view_visible(is_visible_in_tree());
	if (hotspot_fit_hud && hotspot_fit_hud_label && !hud_diagnostic.is_empty()) {
		const PackedStringArray finalists = hud_diagnostic.get("finalist_names", PackedStringArray());
		const String mode = hotspot_preview_active ? TTR("Hotspot preview (UVs unchanged)") : TTR("Last hotspot fit");
		hotspot_fit_hud_label->set_text(vformat("%s\nwant_area: %.4f   want_aspect: %.4f\nfinalists: %s\nchosen: %s   decision: %s",
				mode, double(hud_diagnostic.get("want_area", 0.0)), double(hud_diagnostic.get("want_aspect", 0.0)),
				String(", ").join(finalists), String(hud_diagnostic.get("chosen", String())), String(hud_diagnostic.get("decided_by", String()))));
		hotspot_fit_hud->show();
	}
	set_meta(StringName("_level_hotspot_overlay_face_triangle_count"), vertices.size() / 3);
	set_meta(StringName("_level_hotspot_overlay_mode"), hotspot_preview_active ? String("preview") : String("debug"));
}

void LevelEditorView::_clear_hotspot_fit_overlay() {
	hotspot_fit_overlay.clear();
	hotspot_fit_overlay.set_view_visible(false);
	if (hotspot_fit_hud) {
		hotspot_fit_hud->hide();
	}
	set_meta(StringName("_level_hotspot_overlay_face_triangle_count"), 0);
	set_meta(StringName("_level_hotspot_overlay_mode"), String());
}

void LevelEditorView::set_last_selection_action(const StringName &p_action) {
	set_meta(StringName("_level_last_selection_action"), p_action);
	if (p_action == SNAME("hotspot_fit_individual") || p_action == SNAME("hotspot_fit_grouped")) {
		_capture_committed_hotspot_diagnostics();
	}
}

void LevelEditorView::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_THEME_CHANGED: {
			_update_ui_theme();
		} break;
		case NOTIFICATION_PROCESS: {
			_process_freelook(get_process_delta_time());
			_update_transform_gizmo();
		} break;
		case NOTIFICATION_VISIBILITY_CHANGED: {
			if (!is_visible_in_tree()) {
				_close_fast_texture(false, false);
				_end_freelook();
			}
			if (grid_attached && grid_instance.is_valid() && RenderingServer::get_singleton()) {
				RenderingServer::get_singleton()->instance_set_visible(grid_instance, is_visible_in_tree());
			}
			if (active_tool.is_valid()) {
				active_tool->set_view_visible(is_visible_in_tree());
			}
			if (SelectTool *select_tool = Object::cast_to<SelectTool>(tools[LevelEditor::TOOL_SELECT].ptr())) {
				select_tool->set_transform_gizmo_view_visible(is_visible_in_tree());
			}
			if (selection_overlay) {
				selection_overlay->set_view_visible(is_visible_in_tree());
			}
			if (is_visible_in_tree() && (hotspot_preview_active || hotspot_fit_debug_enabled)) {
				_refresh_hotspot_fit_overlay();
			} else {
				hotspot_fit_overlay.set_view_visible(false);
				if (hotspot_fit_hud) {
					hotspot_fit_hud->hide();
				}
			}
		} break;
		case NOTIFICATION_EXIT_TREE: {
			_close_fast_texture(false, false);
			_end_freelook();
			if (SelectTool *select_tool = Object::cast_to<SelectTool>(tools[LevelEditor::TOOL_SELECT].ptr())) {
				// Release idle LevelMesh refs before document undo histories and
				// kernel diff snapshots enter shutdown teardown.
				select_tool->exit_gesture();
			}
		} break;
		case NOTIFICATION_PREDELETE: {
			// Node's predelete handler frees child Controls before the C++ destructor
			// runs. Release every service callback that can touch those children here.
			_close_fast_texture(false, false);
			_clear_hotspot_fit_overlay();
			if (registered_with_level_editor) {
				if (LevelEditor *level_editor = LevelEditor::get_singleton()) {
					level_editor->_unregister_view(this);
				}
				registered_with_level_editor = false;
			}
			if (active_tool.is_valid()) {
				active_tool->deactivate();
				active_tool.unref();
			}
			for (Ref<LevelEditorTool> &tool : tools) {
				tool.unref();
			}
			hotspot_fit_hud = nullptr;
			hotspot_fit_hud_label = nullptr;
			marquee_overlay = nullptr;
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
	HBoxContainer *grid_controls = memnew(HBoxContainer);
	grid_controls->set_name("LevelGridControls");
	grid_controls->add_theme_constant_override("separation", 4 * EDSCALE);
	top_strip->add_child(grid_controls);
	grid_step_label = memnew(Label);
	grid_step_label->set_name("LevelGridStepLabel");
	grid_controls->add_child(grid_step_label);
	grid_step_decrease_button = memnew(Button("-"));
	grid_step_decrease_button->set_name("LevelGridStepDecrease");
	grid_step_decrease_button->set_custom_minimum_size(Size2(18, 18) * EDSCALE);
	grid_step_decrease_button->set_focus_mode(FOCUS_NONE);
	grid_step_decrease_button->set_tooltip_text(TTRC("Halve the level-editor grid step"));
	grid_step_decrease_button->set_accessibility_name(TTRC("Decrease grid step"));
	grid_step_decrease_button->connect(SceneStringName(pressed), callable_mp(this, &LevelEditorView::_grid_step_decrease_pressed));
	grid_controls->add_child(grid_step_decrease_button);
	grid_step_increase_button = memnew(Button("+"));
	grid_step_increase_button->set_name("LevelGridStepIncrease");
	grid_step_increase_button->set_custom_minimum_size(Size2(18, 18) * EDSCALE);
	grid_step_increase_button->set_focus_mode(FOCUS_NONE);
	grid_step_increase_button->set_tooltip_text(TTRC("Double the level-editor grid step"));
	grid_step_increase_button->set_accessibility_name(TTRC("Increase grid step"));
	grid_step_increase_button->connect(SceneStringName(pressed), callable_mp(this, &LevelEditorView::_grid_step_increase_pressed));
	grid_controls->add_child(grid_step_increase_button);
	surface_layout->add_child(top_strip); // Fallback until DocumentView supplies toolbar_host.

	HBoxContainer *body = memnew(HBoxContainer);
	body->set_h_size_flags(SIZE_EXPAND_FILL);
	body->set_v_size_flags(SIZE_EXPAND_FILL);
	body->add_theme_constant_override("separation", 0);
	surface_layout->add_child(body);

	MarginContainer *tool_rail_margin = memnew(MarginContainer);
	tool_rail_margin->set_name("LevelToolRailMargin");
	tool_rail_margin->add_theme_constant_override("margin_left", 4 * EDSCALE);
	tool_rail_margin->add_theme_constant_override("margin_right", 4 * EDSCALE);
	tool_rail_margin->add_theme_constant_override("margin_top", 5 * EDSCALE);
	tool_rail_margin->add_theme_constant_override("margin_bottom", 5 * EDSCALE);
	body->add_child(tool_rail_margin);

	tool_rail = memnew(VBoxContainer);
	tool_rail->set_name("LevelToolRail");
	tool_rail->set_custom_minimum_size(Size2(36 * EDSCALE, 0));
	tool_rail->set_v_size_flags(SIZE_EXPAND_FILL);
	tool_rail->add_theme_constant_override("separation", 4 * EDSCALE);
	Ref<ButtonGroup> tool_button_group;
	tool_button_group.instantiate();
	select_tool_button = memnew(Button);
	select_tool_button->set_name("LevelSelectToolButton");
	select_tool_button->set_custom_minimum_size(Size2(36, 36) * EDSCALE);
	select_tool_button->set_toggle_mode(true);
	select_tool_button->set_button_group(tool_button_group);
	select_tool_button->set_tooltip_text(TTRC("Select vertices, edges, faces, or blocks"));
	select_tool_button->set_accessibility_name(TTRC("Select tool"));
	select_tool_button->connect(SceneStringName(pressed), callable_mp(this, &LevelEditorView::_tool_button_pressed).bind(int(LevelEditor::TOOL_SELECT)));
	tool_rail->add_child(select_tool_button);
	_register_icon_button(select_tool_button, SNAME("ToolSelect"));

	block_tool_button = memnew(Button);
	block_tool_button->set_name("LevelBlockToolButton");
	block_tool_button->set_custom_minimum_size(Size2(36, 36) * EDSCALE);
	block_tool_button->set_toggle_mode(true);
	block_tool_button->set_button_group(tool_button_group);
	block_tool_button->set_tooltip_text(TTRC("Add Block (Shift+B)"));
	block_tool_button->set_accessibility_name(TTRC("Block tool"));
	block_tool_button->connect(SceneStringName(pressed), callable_mp(this, &LevelEditorView::_tool_button_pressed).bind(int(LevelEditor::TOOL_BLOCK)));
	tool_rail->add_child(block_tool_button);
	_register_icon_button(block_tool_button, SNAME("BoxMesh"));

	tool_rail->add_child(memnew(HSeparator));
	context_panel_toggle_button = memnew(Button);
	context_panel_toggle_button->set_name("LevelContextPanelToggle");
	context_panel_toggle_button->set_custom_minimum_size(Size2(36, 30) * EDSCALE);
	context_panel_toggle_button->set_toggle_mode(true);
	context_panel_toggle_button->set_pressed(true);
	context_panel_toggle_button->set_accessibility_name(TTRC("Collapse contextual options"));
	context_panel_toggle_button->connect(SceneStringName(pressed), callable_mp(this, &LevelEditorView::_context_panel_toggle_pressed));
	tool_rail->add_child(context_panel_toggle_button);

	Control *rail_spacer = memnew(Control);
	rail_spacer->set_v_size_flags(SIZE_EXPAND_FILL);
	tool_rail->add_child(rail_spacer);

	active_material_swatch = memnew(Button);
	active_material_swatch->set_name("LevelActiveMaterialSwatch");
	active_material_swatch->set_custom_minimum_size(Size2(36, 36) * EDSCALE);
	active_material_swatch->set_tooltip_text(TTRC("Open Materials and reveal the active material"));
	active_material_swatch->set_accessibility_name(TTRC("Open active material in Materials"));
	active_material_swatch->connect(SceneStringName(pressed), callable_mp(this, &LevelEditorView::_active_material_swatch_pressed));
	tool_rail->add_child(active_material_swatch);
	_register_icon_button(active_material_swatch, SNAME("StandardMaterial3D"));

	active_material_preview = memnew(TextureRect);
	active_material_preview->set_name("ActiveMaterialPreview");
	active_material_preview->set_anchors_and_offsets_preset(PRESET_FULL_RECT, PRESET_MODE_MINSIZE, 5 * EDSCALE);
	active_material_preview->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
	active_material_preview->set_stretch_mode(TextureRect::STRETCH_KEEP_ASPECT_CENTERED);
	active_material_preview->set_mouse_filter(MOUSE_FILTER_IGNORE);
	active_material_swatch->add_child(active_material_preview);
	tool_rail_margin->add_child(tool_rail);
	context_panel_separator = memnew(VSeparator);
	context_panel_separator->set_name("LevelToolContextSeparator");
	body->add_child(context_panel_separator);

	context_panel = memnew(PanelContainer);
	context_panel->set_name("LevelContextPanel");
	context_panel->set_custom_minimum_size(Size2(252 * EDSCALE, 0));
	context_panel->set_h_size_flags(SIZE_SHRINK_BEGIN);
	context_panel->set_v_size_flags(SIZE_EXPAND_FILL);
	body->add_child(context_panel);

	VBoxContainer *context_frame = memnew(VBoxContainer);
	context_frame->set_name("LevelContextFrame");
	context_frame->add_theme_constant_override("separation", 4 * EDSCALE);
	context_panel->add_child(context_frame);
	MarginContainer *context_header_margin = memnew(MarginContainer);
	context_header_margin->add_theme_constant_override("margin_left", 8 * EDSCALE);
	context_header_margin->add_theme_constant_override("margin_right", 8 * EDSCALE);
	context_header_margin->add_theme_constant_override("margin_top", 6 * EDSCALE);
	context_frame->add_child(context_header_margin);
	context_title = memnew(Label(TTRC("Selection")));
	context_title->set_name("LevelContextTitle");
	context_title->add_theme_font_size_override("font_size", 14 * EDSCALE);
	context_header_margin->add_child(context_title);
	context_frame->add_child(memnew(HSeparator));

	ScrollContainer *context_scroll = memnew(ScrollContainer);
	context_scroll->set_name("LevelContextScroll");
	context_scroll->set_horizontal_scroll_mode(ScrollContainer::SCROLL_MODE_DISABLED);
	context_scroll->set_vertical_scroll_mode(ScrollContainer::SCROLL_MODE_AUTO);
	context_scroll->set_h_size_flags(SIZE_EXPAND_FILL);
	context_scroll->set_v_size_flags(SIZE_EXPAND_FILL);
	context_frame->add_child(context_scroll);
	MarginContainer *context_margin = memnew(MarginContainer);
	context_margin->set_h_size_flags(SIZE_EXPAND_FILL);
	context_margin->add_theme_constant_override("margin_left", 8 * EDSCALE);
	context_margin->add_theme_constant_override("margin_right", 8 * EDSCALE);
	context_margin->add_theme_constant_override("margin_top", 4 * EDSCALE);
	context_margin->add_theme_constant_override("margin_bottom", 8 * EDSCALE);
	context_scroll->add_child(context_margin);
	VBoxContainer *context_content = memnew(VBoxContainer);
	context_content->set_name("LevelContextContent");
	context_content->set_h_size_flags(SIZE_EXPAND_FILL);
	context_content->add_theme_constant_override("separation", 8 * EDSCALE);
	context_margin->add_child(context_content);

	auto add_context_section = [](VBoxContainer *p_parent, const String &p_title, const StringName &p_name) {
		VBoxContainer *section = memnew(VBoxContainer);
		section->set_name(p_name);
		section->set_h_size_flags(SIZE_EXPAND_FILL);
		section->add_theme_constant_override("separation", 3 * EDSCALE);
		Label *title = memnew(Label(p_title));
		title->set_name(String(p_name) + "Title");
		title->add_theme_font_size_override("font_size", 12 * EDSCALE);
		section->add_child(title);
		p_parent->add_child(section);
		return section;
	};
	auto add_context_button = [&](Container *p_parent, const String &p_text, const StringName &p_name,
			const String &p_tooltip, const StringName &p_icon = StringName()) {
		Button *button = memnew(Button(p_text));
		button->set_name(p_name);
		button->set_h_size_flags(SIZE_EXPAND_FILL);
		button->set_custom_minimum_size(Size2(0, 27) * EDSCALE);
		button->set_text_alignment(HORIZONTAL_ALIGNMENT_CENTER);
		button->set_focus_mode(FOCUS_NONE);
		button->set_tooltip_text(p_tooltip);
		button->set_accessibility_name(p_tooltip);
		p_parent->add_child(button);
		compact_context_buttons.push_back(button);
		if (!p_icon.is_empty()) {
			_register_icon_button(button, p_icon);
		}
		return button;
	};
	auto add_modify_button = [&](Container *p_parent, const String &p_text, const StringName &p_name,
			int p_operation, const Vector2 &p_value, const String &p_tooltip, const StringName &p_icon = StringName()) {
		Button *button = add_context_button(p_parent, p_text, p_name, p_tooltip, p_icon);
		button->connect(SceneStringName(pressed), callable_mp(this, &LevelEditorView::_modify_texture_pressed).bind(p_operation, p_value));
		return button;
	};

	// Modal content has first priority and deliberately hides all ordinary tools.
	VBoxContainer *fast_context = memnew(VBoxContainer);
	fast_context->set_name("LevelFastTextureContext");
	fast_context->set_h_size_flags(SIZE_EXPAND_FILL);
	fast_context->add_theme_constant_override("separation", 8 * EDSCALE);
	context_content->add_child(fast_context);
	fast_texture_context = fast_context;
	Label *fast_hint = memnew(Label(TTRC("A modal texture session is active in the viewport. Accept its changes or cancel to return to ordinary tools.")));
	fast_hint->set_name("FastTextureContextStatus");
	fast_hint->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	fast_context->add_child(fast_hint);
	HBoxContainer *fast_actions = memnew(HBoxContainer);
	fast_actions->add_theme_constant_override("separation", 4 * EDSCALE);
	fast_context->add_child(fast_actions);
	Button *fast_accept = add_context_button(fast_actions, TTRC("Accept"), SNAME("FastTextureAccept"), TTRC("Accept Fast Texture changes"), SNAME("ImportCheck"));
	fast_accept->connect(SceneStringName(pressed), callable_mp(this, &LevelEditorView::_fast_texture_accept_pressed));
	Button *fast_cancel = add_context_button(fast_actions, TTRC("Cancel"), SNAME("FastTextureCancel"), TTRC("Cancel Fast Texture changes"), SNAME("Close"));
	fast_cancel->connect(SceneStringName(pressed), callable_mp(this, &LevelEditorView::_fast_texture_cancel_pressed));

	VBoxContainer *block_context = memnew(VBoxContainer);
	block_context->set_name("LevelBlockContext");
	block_context->set_h_size_flags(SIZE_EXPAND_FILL);
	block_context->add_theme_constant_override("separation", 8 * EDSCALE);
	context_content->add_child(block_context);
	block_tool_context = block_context;
	Label *block_hint = memnew(Label(TTRC("Drag a base in the viewport, then drag its height. Shift+B returns to this tool.")));
	block_hint->set_name("BlockToolHint");
	block_hint->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	block_context->add_child(block_hint);
	VBoxContainer *block_create_section = add_context_section(block_context, TTRC("Creation"), SNAME("BlockCreationSection"));
	block_snap_enabled = memnew(CheckButton(TTRC("Snap to grid")));
	block_snap_enabled->set_name("BlockSnapEnabled");
	block_snap_enabled->set_accessibility_name(TTRC("Snap block creation to grid"));
	block_snap_enabled->connect(SceneStringName(toggled), callable_mp(this, &LevelEditorView::_block_snap_toggled));
	block_create_section->add_child(block_snap_enabled);
	HBoxContainer *snap_step_row = memnew(HBoxContainer);
	snap_step_row->set_name("BlockSnapStepRow");
	snap_step_row->add_child(memnew(Label(TTRC("Grid step"))));
	block_snap_step = memnew(SpinBox);
	block_snap_step->set_name("BlockSnapStep");
	block_snap_step->set_h_size_flags(SIZE_EXPAND_FILL);
	block_snap_step->set_min(0.001);
	block_snap_step->set_max(1024.0);
	block_snap_step->set_step(0.125);
	block_snap_step->set_suffix(TTRC(" m"));
	block_snap_step->set_accessibility_name(TTRC("Block creation grid step"));
	block_snap_step->connect(SceneStringName(value_changed), callable_mp(this, &LevelEditorView::_block_snap_step_changed));
	snap_step_row->add_child(block_snap_step);
	block_create_section->add_child(snap_step_row);
	Label *default_height = memnew(Label(vformat(TTRC("Default height: %s m"), String::num_real(LevelEditor::DEFAULT_BLOCK_HEIGHT, false))));
	default_height->set_name("BlockDefaultHeight");
	block_create_section->add_child(default_height);
	VBoxContainer *block_material_section = add_context_section(block_context, TTRC("Material"), SNAME("BlockMaterialSection"));
	block_material_label = memnew(Label(TTRC("No Active Material")));
	block_material_label->set_name("BlockActiveMaterialName");
	block_material_label->set_clip_text(true);
	block_material_section->add_child(block_material_label);
	Button *block_open_materials = add_context_button(block_material_section, TTRC("Choose Material"), SNAME("BlockChooseMaterial"), TTRC("Open Materials and reveal the active material"), SNAME("StandardMaterial3D"));
	block_open_materials->connect(SceneStringName(pressed), callable_mp(this, &LevelEditorView::_active_material_swatch_pressed));

	VBoxContainer *hint_context = memnew(VBoxContainer);
	hint_context->set_name("LevelSelectionHintContext");
	hint_context->set_h_size_flags(SIZE_EXPAND_FILL);
	context_content->add_child(hint_context);
	selection_hint_context = hint_context;
	selection_hint_label = memnew(Label);
	selection_hint_label->set_name("LevelSelectionHint");
	selection_hint_label->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	hint_context->add_child(selection_hint_label);

	VBoxContainer *face_context = memnew(VBoxContainer);
	face_context->set_name("LevelTextureContext");
	face_context->set_h_size_flags(SIZE_EXPAND_FILL);
	face_context->add_theme_constant_override("separation", 10 * EDSCALE);
	face_context->connect(SceneStringName(mouse_entered), callable_mp(this, &LevelEditorView::_texture_context_mouse_entered));
	face_context->connect(SceneStringName(mouse_exited), callable_mp(this, &LevelEditorView::_texture_context_mouse_exited));
	context_content->add_child(face_context);
	texture_context = face_context;
	texture_scope_label = memnew(Label);
	texture_scope_label->set_name("TextureScopeLabel");
	texture_scope_label->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	face_context->add_child(texture_scope_label);

	VBoxContainer *material_section = add_context_section(face_context, TTRC("Material"), SNAME("FaceMaterialSection"));
	active_material_label = memnew(Label(TTRC("No Active Material")));
	active_material_label->set_name("ActiveMaterialName");
	active_material_label->set_clip_text(true);
	material_section->add_child(active_material_label);
	Button *apply_material = add_context_button(material_section, TTRC("Apply Active Material"), SNAME("ApplyActiveMaterial"), TTRC("Apply active material to the selected faces (Shift+T)"), SNAME("StandardMaterial3D"));
	apply_material->connect(SceneStringName(pressed), callable_mp(this, &LevelEditorView::apply_active_material_to_selection));
	captured_mapping_label = memnew(Label(TTRC("Lift: no captured mapping")));
	captured_mapping_label->set_name("CapturedMappingStatus");
	captured_mapping_label->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	captured_mapping_label->set_tooltip_text(TTRC("Shift+right-click a face in the viewport to lift its material and compatible projected mapping."));
	material_section->add_child(captured_mapping_label);

	VBoxContainer *nudge_section = add_context_section(face_context, TTRC("Nudge"), SNAME("TextureNudgeSection"));
	GridContainer *nudge_grid = memnew(GridContainer);
	nudge_grid->set_name("TextureNudgeGrid");
	nudge_grid->set_columns(3);
	nudge_grid->add_theme_constant_override("h_separation", 3 * EDSCALE);
	nudge_grid->add_theme_constant_override("v_separation", 3 * EDSCALE);
	nudge_section->add_child(nudge_grid);
	add_modify_button(nudge_grid, "NW", SNAME("ModifyShiftNW"), LevelMesh::TEXTURE_MODIFY_SHIFT, Vector2(-1, -1), TTRC("Shift northwest (Numpad 7)"));
	add_modify_button(nudge_grid, "", SNAME("ModifyShiftN"), LevelMesh::TEXTURE_MODIFY_SHIFT, Vector2(0, -1), TTRC("Shift north (Numpad 8)"), SNAME("ArrowUp"));
	add_modify_button(nudge_grid, "NE", SNAME("ModifyShiftNE"), LevelMesh::TEXTURE_MODIFY_SHIFT, Vector2(1, -1), TTRC("Shift northeast (Numpad 9)"));
	add_modify_button(nudge_grid, "", SNAME("ModifyShiftW"), LevelMesh::TEXTURE_MODIFY_SHIFT, Vector2(-1, 0), TTRC("Shift west (Numpad 4)"), SNAME("ArrowLeft"));
	Control *nudge_center = memnew(Control);
	nudge_center->set_custom_minimum_size(Size2(27, 24) * EDSCALE);
	nudge_grid->add_child(nudge_center);
	add_modify_button(nudge_grid, "", SNAME("ModifyShiftE"), LevelMesh::TEXTURE_MODIFY_SHIFT, Vector2(1, 0), TTRC("Shift east (Numpad 6)"), SNAME("ArrowRight"));
	add_modify_button(nudge_grid, "SW", SNAME("ModifyShiftSW"), LevelMesh::TEXTURE_MODIFY_SHIFT, Vector2(-1, 1), TTRC("Shift southwest (Numpad 1)"));
	add_modify_button(nudge_grid, "", SNAME("ModifyShiftS"), LevelMesh::TEXTURE_MODIFY_SHIFT, Vector2(0, 1), TTRC("Shift south (Numpad 2)"), SNAME("ArrowDown"));
	add_modify_button(nudge_grid, "SE", SNAME("ModifyShiftSE"), LevelMesh::TEXTURE_MODIFY_SHIFT, Vector2(1, 1), TTRC("Shift southeast (Numpad 3)"));

	VBoxContainer *scale_section = add_context_section(face_context, TTRC("Scale"), SNAME("TextureScaleSection"));
	GridContainer *scale_grid = memnew(GridContainer);
	scale_grid->set_name("TextureScaleGrid");
	scale_grid->set_columns(2);
	scale_grid->add_theme_constant_override("h_separation", 3 * EDSCALE);
	scale_grid->add_theme_constant_override("v_separation", 3 * EDSCALE);
	scale_section->add_child(scale_grid);
	add_modify_button(scale_grid, TTRC("U−"), SNAME("ModifyScaleXDown"), LevelMesh::TEXTURE_MODIFY_SCALE, Vector2(-1, 0), TTRC("Scale U down (Ctrl+Alt+Numpad 4)"));
	add_modify_button(scale_grid, TTRC("U+"), SNAME("ModifyScaleXUp"), LevelMesh::TEXTURE_MODIFY_SCALE, Vector2(1, 0), TTRC("Scale U up (Ctrl+Alt+Numpad 6)"));
	add_modify_button(scale_grid, TTRC("V−"), SNAME("ModifyScaleYDown"), LevelMesh::TEXTURE_MODIFY_SCALE, Vector2(0, -1), TTRC("Scale V down (Ctrl+Alt+Numpad 2)"));
	add_modify_button(scale_grid, TTRC("V+"), SNAME("ModifyScaleYUp"), LevelMesh::TEXTURE_MODIFY_SCALE, Vector2(0, 1), TTRC("Scale V up (Ctrl+Alt+Numpad 8)"));

	VBoxContainer *rotate_section = add_context_section(face_context, TTRC("Rotate / Flip"), SNAME("TextureRotateFlipSection"));
	GridContainer *rotate_grid = memnew(GridContainer);
	rotate_grid->set_name("TextureRotateFlipGrid");
	rotate_grid->set_columns(2);
	rotate_grid->add_theme_constant_override("h_separation", 3 * EDSCALE);
	rotate_grid->add_theme_constant_override("v_separation", 3 * EDSCALE);
	rotate_section->add_child(rotate_grid);
	add_modify_button(rotate_grid, TTRC("CCW"), SNAME("ModifyRotateCCW"), LevelMesh::TEXTURE_MODIFY_ROTATE, Vector2(1, 0), TTRC("Rotate counter-clockwise (Alt+Numpad 7)"), SNAME("RotateLeft"));
	add_modify_button(rotate_grid, TTRC("CW"), SNAME("ModifyRotateCW"), LevelMesh::TEXTURE_MODIFY_ROTATE, Vector2(-1, 0), TTRC("Rotate clockwise (Alt+Numpad 9)"), SNAME("RotateRight"));
	add_modify_button(rotate_grid, TTRC("Flip U"), SNAME("ModifyFlipHorizontal"), LevelMesh::TEXTURE_MODIFY_FLIP_HORIZONTAL, Vector2(1, 1), TTRC("Flip face UV horizontally (Alt+R)"), SNAME("MirrorX"));
	add_modify_button(rotate_grid, TTRC("Flip V"), SNAME("ModifyFlipVertical"), LevelMesh::TEXTURE_MODIFY_FLIP_VERTICAL, Vector2(1, 1), TTRC("Flip face UV vertically (Alt+T)"), SNAME("MirrorY"));

	VBoxContainer *align_section = add_context_section(face_context, TTRC("Align"), SNAME("TextureAlignSection"));
	GridContainer *align_grid = memnew(GridContainer);
	align_grid->set_name("TextureAlignGrid");
	align_grid->set_columns(3);
	align_grid->add_theme_constant_override("h_separation", 3 * EDSCALE);
	align_grid->add_theme_constant_override("v_separation", 3 * EDSCALE);
	align_section->add_child(align_grid);
	add_modify_button(align_grid, TTRC("Left"), SNAME("ModifyJustifyLeft"), LevelMesh::TEXTURE_MODIFY_JUSTIFY_LEFT, Vector2(1, 1), TTRC("Justify left (Alt+Numpad 4)"));
	add_modify_button(align_grid, TTRC("Center"), SNAME("ModifyJustifyCenter"), LevelMesh::TEXTURE_MODIFY_JUSTIFY_CENTER, Vector2(1, 1), TTRC("Center horizontally and vertically (Alt+Numpad 5)"));
	add_modify_button(align_grid, TTRC("Right"), SNAME("ModifyJustifyRight"), LevelMesh::TEXTURE_MODIFY_JUSTIFY_RIGHT, Vector2(1, 1), TTRC("Justify right (Alt+Numpad 6)"));
	add_modify_button(align_grid, TTRC("Top"), SNAME("ModifyJustifyTop"), LevelMesh::TEXTURE_MODIFY_JUSTIFY_TOP, Vector2(1, 1), TTRC("Justify top (Alt+Numpad 8)"));
	add_modify_button(align_grid, TTRC("Middle"), SNAME("ModifyJustifyMiddle"), LevelMesh::TEXTURE_MODIFY_JUSTIFY_CENTER, Vector2(1, 1), TTRC("Center horizontally and vertically (Alt+Numpad 5)"));
	add_modify_button(align_grid, TTRC("Bottom"), SNAME("ModifyJustifyBottom"), LevelMesh::TEXTURE_MODIFY_JUSTIFY_BOTTOM, Vector2(1, 1), TTRC("Justify bottom (Alt+Numpad 2)"));
	Control *fit_spacer = memnew(Control);
	fit_spacer->set_custom_minimum_size(Size2(0, 2) * EDSCALE);
	align_section->add_child(fit_spacer);
	add_modify_button(align_section, TTRC("Fit to Texture"), SNAME("ModifyFit"), LevelMesh::TEXTURE_MODIFY_FIT, Vector2(1, 1), TTRC("Fit to one texture footprint (Ctrl+Alt+Numpad 5)"), SNAME("AnimationAutoFit"));

	VBoxContainer *hotspot_section = add_context_section(face_context, TTRC("Hotspot"), SNAME("TextureHotspotSection"));
	hotspot_context = hotspot_section;
	hotspot_mapping_mode = memnew(OptionButton);
	hotspot_mapping_mode->set_name("HotspotMappingModeOverride");
	hotspot_mapping_mode->set_h_size_flags(SIZE_EXPAND_FILL);
	hotspot_mapping_mode->set_fit_to_longest_item(false);
	hotspot_mapping_mode->set_tooltip_text(TTRC("Override the bound atlas mapping mode for Hotspot Fit"));
	hotspot_mapping_mode->set_accessibility_name(TTRC("Hotspot mapping mode override"));
	hotspot_mapping_mode->add_item(TTRC("Atlas Default"), -1);
	hotspot_mapping_mode->add_item(TTRC("Automatic"), 0);
	hotspot_mapping_mode->add_item(TTRC("Square"), 1);
	hotspot_mapping_mode->add_item(TTRC("Conforming"), 2);
	hotspot_mapping_mode->add_item(TTRC("Follow Active Quads"), 3);
	hotspot_mapping_mode->connect(SceneStringName(item_selected), callable_mp(this, &LevelEditorView::_hotspot_mapping_mode_selected));
	hotspot_section->add_child(hotspot_mapping_mode);
	HBoxContainer *hotspot_fit_row = memnew(HBoxContainer);
	hotspot_fit_row->set_name("HotspotFitActions");
	hotspot_fit_row->add_theme_constant_override("separation", 3 * EDSCALE);
	hotspot_section->add_child(hotspot_fit_row);
	Button *fit_individual = add_context_button(hotspot_fit_row, TTRC("Individual"), SNAME("HotspotFitIndividual"), TTRC("Fit each selected island independently (Shift+F)"));
	fit_individual->connect(SceneStringName(pressed), callable_mp(this, &LevelEditorView::_hotspot_fit_pressed).bind(true));
	Button *fit_grouped = add_context_button(hotspot_fit_row, TTRC("Grouped"), SNAME("HotspotFitGrouped"), TTRC("Fit connected selected islands as groups (Shift+H)"));
	fit_grouped->connect(SceneStringName(pressed), callable_mp(this, &LevelEditorView::_hotspot_fit_pressed).bind(false));
	hotspot_debug_toggle = memnew(CheckButton(TTRC("Show fit diagnostics")));
	hotspot_debug_toggle->set_name("HotspotFitDebugToggle");
	hotspot_debug_toggle->set_tooltip_text(TTRC("Show the last Hotspot Fit decisions over the viewport"));
	hotspot_debug_toggle->set_accessibility_name(TTRC("Show Hotspot Fit diagnostics"));
	hotspot_debug_toggle->connect(SceneStringName(toggled), callable_mp(this, &LevelEditorView::_hotspot_debug_toggled));
	hotspot_section->add_child(hotspot_debug_toggle);

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

	hotspot_fit_hud = memnew(PanelContainer);
	hotspot_fit_hud->set_name("HotspotFitDebugHUD");
	hotspot_fit_hud->set_anchors_preset(PRESET_TOP_RIGHT);
	hotspot_fit_hud->set_offset(SIDE_LEFT, -470 * EDSCALE);
	hotspot_fit_hud->set_offset(SIDE_TOP, 8 * EDSCALE);
	hotspot_fit_hud->set_offset(SIDE_RIGHT, -8 * EDSCALE);
	hotspot_fit_hud->set_offset(SIDE_BOTTOM, 112 * EDSCALE);
	hotspot_fit_hud->set_mouse_filter(MOUSE_FILTER_IGNORE);
	Ref<StyleBoxFlat> hotspot_hud_style;
	hotspot_hud_style.instantiate();
	hotspot_hud_style->set_bg_color(Color(0.035, 0.038, 0.045, 0.90f));
	hotspot_hud_style->set_border_width_all(MAX(1, int(EDSCALE)));
	hotspot_hud_style->set_border_color(Color(0.34, 0.37, 0.44, 0.92f));
	hotspot_hud_style->set_corner_radius_all(3 * EDSCALE);
	hotspot_fit_hud->add_theme_style_override(SceneStringName(panel), hotspot_hud_style);
	hotspot_fit_hud_label = memnew(Label);
	hotspot_fit_hud_label->set_name("HotspotFitDebugReadout");
	hotspot_fit_hud_label->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	hotspot_fit_hud_label->set_mouse_filter(MOUSE_FILTER_IGNORE);
	hotspot_fit_hud->add_child(hotspot_fit_hud_label);
	hotspot_fit_hud->hide();
	viewport_container->add_child(hotspot_fit_hud);

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
	hotspot_fit_overlay.set_render_layer(gizmo_layer);
	hotspot_fit_overlay.set_scenario(document->get_scenario());
	hotspot_fit_overlay.set_view_visible(false);
	selection_overlay = memnew(SelectionHighlightOverlay);
	selection_overlay->initialize(selection_model, document->get_scenario(), gizmo_layer);
	selection_overlay->set_view_visible(is_visible_in_tree());
	selection_model->connect(SNAME("selection_changed"), callable_mp(this, &LevelEditorView::_selection_changed));
	if (EditorSelection *object_selection = document->get_selection()) {
		object_selection->connect(SNAME("selection_changed"), callable_mp(this, &LevelEditorView::_object_selection_changed));
	}
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
	ED_SHORTCUT("level_editor/toggle_materials", TTRC("Toggle Materials Drawer"), Key::M);
	if (LevelEditor *level_editor = LevelEditor::get_singleton()) {
		level_editor->_register_view(this);
		registered_with_level_editor = true;
		level_editor->connect(SNAME("active_material_changed_for_document"), callable_mp(this, &LevelEditorView::_active_material_changed_for_document));
		level_editor->connect(SNAME("snap_settings_changed"), callable_mp(this, &LevelEditorView::_snap_settings_changed));
		_active_material_changed(level_editor->get_active_material(document), level_editor->get_active_material_path(document));
		_snap_settings_changed(level_editor->get_snap_step(), level_editor->is_snap_enabled());
	}
	_set_context_panel_expanded(true);
	_refresh_context_panel();
	_update_ui_theme();
}

LevelEditorView::~LevelEditorView() {
	if (fast_texture_overlay) {
		fast_texture_overlay->cancel();
		fast_texture_overlay = nullptr;
		fast_texture_input_context_active = false;
	}
	if (registered_with_level_editor) {
		if (LevelEditor *level_editor = LevelEditor::get_singleton()) {
			level_editor->_unregister_view(this);
		}
		registered_with_level_editor = false;
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
