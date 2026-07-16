/**************************************************************************/
/*  select_tool_texture.cpp                                              */
/**************************************************************************/

#include "select_tool.h"

#include "core/io/resource_loader.h"
#include "editor/editor_document.h"
#include "editor/gui/editor_toaster.h"
#include "editor/level/level_editor_view.h"
#include "editor/level/texel_density_scanner.h"
#include "editor/settings/editor_settings.h"
#include "scene/3d/camera_3d.h"

#include "modules/level_kernel/level_block.h"
#include "modules/level_kernel/hotspot_atlas.h"
#include "modules/level_kernel/hotspot_fitter.h"
#include "modules/level_kernel/level_mesh.h"
#include "modules/level_kernel/level_mesh_adjacency.h"
#include "modules/level_kernel/level_mesh_baker.h"
#include "modules/level_kernel/level_mesh_data.h"
#include "modules/level_kernel/level_mesh_diff.h"

namespace {

struct HotspotApplyGroup {
	String material_path;
	String atlas_path;
	Size2i texture_size;
	Ref<HotspotAtlas> atlas;
	PackedInt32Array face_ids;
};

real_t texture_setting(const StringName &p_name, real_t p_default) {
	EditorSettings *settings = EditorSettings::get_singleton();
	if (!settings || !settings->has_setting(p_name)) {
		return p_default;
	}
	const real_t value = settings->get(p_name);
	return Math::is_finite(value) ? value : p_default;
}

int64_t hotspot_seed_setting() {
	EditorSettings *settings = EditorSettings::get_singleton();
	if (!settings || !settings->has_setting("level_editor/hotspot/random_seed")) {
		return 0;
	}
	const Variant configured = settings->get("level_editor/hotspot/random_seed");
	return configured.get_type() == Variant::INT ? int64_t(configured) : 0;
}

} // namespace

bool SelectTool::_collect_face_selection(Vector<MeshDragState> &r_states, LevelBlock *p_only_block) const {
	r_states.clear();
	SelectionModel *selection_model = get_selection_model();
	LevelEditorView *level_view = get_view();
	LevelDocument *document = level_view ? level_view->get_level_document() : nullptr;
	if (!selection_model || !document) {
		return false;
	}

	auto get_state = [&](LevelBlock *p_block, const Ref<LevelMesh> &p_mesh) -> MeshDragState * {
		for (MeshDragState &state : r_states) {
			if (state.block == p_block) {
				return &state;
			}
		}
		MeshDragState state;
		state.block = p_block;
		state.mesh = p_mesh;
		r_states.push_back(state);
		return &r_states.write[r_states.size() - 1];
	};

	if (selection_model->get_mode() == SelectionModel::MODE_OBJECT) {
		EditorSelection *selection = document->get_selection();
		if (!selection) {
			return false;
		}
		const TypedArray<Node> nodes = selection->get_selected_nodes();
		for (int i = 0; i < nodes.size(); i++) {
			LevelBlock *block = Object::cast_to<LevelBlock>(nodes[i]);
			if (!block || (p_only_block && block != p_only_block)) {
				continue;
			}
			const Ref<LevelMesh> mesh = block->get_level_mesh();
			const Ref<LevelMeshData> mesh_data = block->get_data();
			if (mesh.is_null() || mesh_data.is_null()) {
				continue;
			}
			MeshDragState *state = get_state(block, mesh);
			const PackedByteArray alive = mesh_data->get_face_alive();
			for (int face_id = 0; face_id < alive.size(); face_id++) {
				if (alive[face_id] != 0) {
					state->face_ids.push_back(face_id);
				}
			}
		}
	} else if (selection_model->get_mode() == SelectionModel::MODE_FACE) {
		for (const SelectionModel::Element &element : selection_model->get_selected(SelectionModel::FEATURE_FACE)) {
			LevelBlock *block = nullptr;
			Ref<LevelMesh> mesh;
			int face_id = -1;
			if (!selection_model->resolve(element, block, mesh, face_id) || !block || mesh.is_null() ||
					(p_only_block && block != p_only_block)) {
				continue;
			}
			MeshDragState *state = get_state(block, mesh);
			if (!state->face_ids.has(face_id)) {
				state->face_ids.push_back(face_id);
			}
		}
	}

	for (int i = r_states.size() - 1; i >= 0; i--) {
		if (r_states[i].face_ids.is_empty()) {
			r_states.remove_at(i);
		} else {
			r_states.write[i].face_ids.sort();
		}
	}
	return !r_states.is_empty();
}

void SelectTool::_show_texture_status(const String &p_message, bool p_warning) const {
	if (LevelEditorView *level_view = get_view()) {
		level_view->set_meta(StringName("_level_texture_status"), p_message);
	}
	if (EditorToaster *toaster = EditorToaster::get_singleton()) {
		toaster->popup_str(p_message, p_warning ? EditorToaster::SEVERITY_WARNING : EditorToaster::SEVERITY_INFO);
	}
}

bool SelectTool::apply_active_material() {
	LevelEditorView *level_view = get_view();
	LevelDocument *document = level_view ? level_view->get_level_document() : nullptr;
	LevelEditor *level_editor = LevelEditor::get_singleton();
	if (!document || !level_editor || level_editor->get_active_material(document).is_null() ||
			level_editor->get_active_material_binding_path(document).is_empty()) {
		return false;
	}
	Vector<MeshDragState> states;
	if (!_collect_face_selection(states)) {
		return false;
	}
	const String material_path = level_editor->get_active_material_binding_path(document);
	const Dictionary capture = level_editor->get_captured_mapping(document);
	bool changed = false;
	for (MeshDragState &state : states) {
		state.geometry_diff = state.mesh->apply_face_texture(state.face_ids, material_path, capture);
		changed = changed || state.geometry_diff.is_valid();
	}
	if (changed) {
		_register_mesh_undo(TTR("Apply Face Material"), states);
		level_view->set_last_selection_action(SNAME("texture_apply"));
	}
	return true;
}

bool SelectTool::modify_selected_texture(int p_operation, const Vector2 &p_value) {
	Vector<MeshDragState> states;
	if (!_collect_face_selection(states)) {
		return false;
	}
	Vector2 kernel_value = p_value;
	switch (LevelMesh::TextureModifyOperation(p_operation)) {
		case LevelMesh::TEXTURE_MODIFY_SHIFT: {
			kernel_value *= texture_setting("level_editor/modify_texture/shift_step", (real_t)0.125);
		} break;
		case LevelMesh::TEXTURE_MODIFY_SCALE: {
			const real_t scale_step = MAX((real_t)1.000001,
					texture_setting("level_editor/modify_texture/scale_step", Math::pow((real_t)2.0, (real_t)0.25)));
			Vector2 factors(1, 1);
			if (!Math::is_zero_approx(p_value.x)) {
				factors.x = Math::pow(scale_step, SIGN(p_value.x));
			}
			if (!Math::is_zero_approx(p_value.y)) {
				factors.y = Math::pow(scale_step, SIGN(p_value.y));
			}
			kernel_value = factors;
		} break;
		case LevelMesh::TEXTURE_MODIFY_ROTATE: {
			const real_t degrees = texture_setting("level_editor/modify_texture/rotation_step_degrees", (real_t)15.0);
			kernel_value = Vector2(Math::deg_to_rad(degrees) * p_value.x, 0);
		} break;
		case LevelMesh::TEXTURE_MODIFY_FIT:
		case LevelMesh::TEXTURE_MODIFY_JUSTIFY_LEFT:
		case LevelMesh::TEXTURE_MODIFY_JUSTIFY_RIGHT:
		case LevelMesh::TEXTURE_MODIFY_JUSTIFY_TOP:
		case LevelMesh::TEXTURE_MODIFY_JUSTIFY_BOTTOM:
		case LevelMesh::TEXTURE_MODIFY_JUSTIFY_CENTER:
		case LevelMesh::TEXTURE_MODIFY_FLIP_HORIZONTAL:
		case LevelMesh::TEXTURE_MODIFY_FLIP_VERTICAL: {
			kernel_value = Vector2(1, 1);
		} break;
		default:
			return false;
	}

	bool changed = false;
	for (MeshDragState &state : states) {
		state.geometry_diff = state.mesh->modify_face_uv(state.face_ids, p_operation, kernel_value);
		changed = changed || state.geometry_diff.is_valid();
	}
	if (!changed) {
		return false;
	}
	_register_mesh_undo(TTR("Modify Face Texture"), states);
	if (LevelEditorView *level_view = get_view()) {
		level_view->set_last_selection_action(SNAME("texture_modify"));
	}
	return true;
}

bool SelectTool::_apply_hotspot_fit(bool p_individual) {
	Vector<MeshDragState> states;
	if (!_collect_face_selection(states)) {
		return false;
	}
	LevelEditor *level_editor = LevelEditor::get_singleton();
	LevelEditorView *level_view = get_view();
	LevelDocument *document = level_view ? level_view->get_level_document() : nullptr;
	const Ref<TexelDensityScanner> scanner = level_editor ? level_editor->get_texel_density_scanner() : Ref<TexelDensityScanner>();
	if (!document || !level_editor || scanner.is_null()) {
		return false;
	}

	Ref<HotspotFitter> fitter;
	fitter.instantiate();
	const int64_t seed = hotspot_seed_setting();
	int selected_count = 0;
	int applied_count = 0;
	int unbound_count = 0;
	int rejected_count = 0;
	bool changed = false;
	for (MeshDragState &state : states) {
		const Ref<LevelMeshData> mesh_data = state.mesh->get_data();
		if (mesh_data.is_null()) {
			rejected_count += state.face_ids.size();
			continue;
		}
		const PackedInt32Array material_indices = mesh_data->get_face_material_indices();
		Vector<HotspotApplyGroup> groups;
		for (const int face_id : state.face_ids) {
			selected_count++;
			if (face_id < 0 || face_id >= material_indices.size()) {
				rejected_count++;
				continue;
			}
			const String material_path = mesh_data->get_material_path(material_indices[face_id]);
			const String atlas_path = level_editor->resolve_hotspot_atlas(material_path);
			if (material_path.is_empty() || atlas_path.is_empty()) {
				unbound_count++;
				continue;
			}
			const Dictionary scan = scanner->scan_path(material_path);
			const Size2i texture_size = scan.get("dimensions", Size2i());
			if (!bool(scan.get("found", false)) || texture_size.x <= 0 || texture_size.y <= 0) {
				rejected_count++;
				continue;
			}

			int group_index = -1;
			for (int i = 0; i < groups.size(); i++) {
				if (groups[i].material_path == material_path && groups[i].atlas_path == atlas_path &&
						groups[i].texture_size == texture_size) {
					group_index = i;
					break;
				}
			}
			if (group_index < 0) {
				Ref<HotspotAtlas> atlas = ResourceLoader::load(atlas_path, "HotspotAtlas");
				if (atlas.is_null()) {
					rejected_count++;
					continue;
				}
				HotspotApplyGroup group;
				group.material_path = material_path;
				group.atlas_path = atlas_path;
				group.texture_size = texture_size;
				group.atlas = atlas;
				groups.push_back(group);
				group_index = groups.size() - 1;
			}
			groups.write[group_index].face_ids.push_back(face_id);
		}

		Array aggregate_results;
		for (const HotspotApplyGroup &group : groups) {
			Dictionary options;
			options["texture_size"] = group.texture_size;
			options["mesh_to_world"] = state.block ? state.block->get_global_transform() : Transform3D();
			const int mapping_override = level_editor->get_hotspot_mapping_mode_override(document);
			options["mapping_mode"] = mapping_override >= 0 ? mapping_override : group.atlas->get_default_mapping_mode();
			options["density_margin"] = texture_setting("level_editor/hotspot/density_margin_octaves", HotspotFitter::DEFAULT_DENSITY_MARGIN);
			options["aspect_margin"] = texture_setting("level_editor/hotspot/aspect_margin_octaves", HotspotFitter::DEFAULT_ASPECT_MARGIN);
			options["cos_coplanar"] = texture_setting("level_editor/hotspot/cos_coplanar", HotspotFitter::DEFAULT_COS_COPLANAR);
			options["cos_collinear"] = texture_setting("level_editor/hotspot/cos_collinear", HotspotFitter::DEFAULT_COS_COLLINEAR);
			options["horizontal_bias_degrees"] = texture_setting("level_editor/hotspot/horizontal_bias_degrees", HotspotFitter::DEFAULT_HORIZONTAL_BIAS_DEGREES);
			options["inset_mip_bleed"] = texture_setting("level_editor/hotspot/inset_mip_bleed_texels_per_level", HotspotFitter::DEFAULT_INSET_MIP_BLEED);
			options["automatic_distortion_threshold"] = Math::deg_to_rad(texture_setting(
					"level_editor/hotspot/automatic_distortion_threshold_degrees",
					Math::rad_to_deg(LevelMesh::DEFAULT_CONFORMING_DISTORTION_THRESHOLD)));
			const Dictionary fitted = fitter->fit(group.face_ids, state.mesh, group.atlas,
					p_individual ? HotspotFitter::ISLAND_INDIVIDUAL : HotspotFitter::ISLAND_GROUPED,
					seed, options);
			if (!bool(fitted.get("ok", false))) {
				rejected_count += group.face_ids.size();
				continue;
			}
			const Array fitted_faces = fitted.get("faces", Array());
			for (int i = 0; i < fitted_faces.size(); i++) {
				aggregate_results.push_back(fitted_faces[i]);
			}
		}
		if (!aggregate_results.is_empty()) {
			state.geometry_diff = state.mesh->apply_hotspot_fit(aggregate_results);
			if (state.geometry_diff.is_valid()) {
				changed = true;
				applied_count += aggregate_results.size();
			} else {
				rejected_count += aggregate_results.size();
			}
		}
	}

	if (changed) {
		_register_mesh_undo(TTR("Hotspot Fit"), states);
		level_view->set_last_selection_action(p_individual ? SNAME("hotspot_fit_individual") : SNAME("hotspot_fit_grouped"));
	}
	if (unbound_count > 0 || rejected_count > 0) {
		_show_texture_status(vformat(TTR("Hotspot Fit applied %d of %d selected faces; skipped %d without a binding and rejected %d."),
				applied_count, selected_count, unbound_count, rejected_count), true);
	} else if (changed) {
		_show_texture_status(vformat(TTR("Hotspot Fit applied to %d selected faces."), applied_count));
	}
	return true;
}

bool SelectTool::_align_selected_texture(bool p_to_grid) {
	Vector<MeshDragState> states;
	if (!_collect_face_selection(states)) {
		return false;
	}
	bool changed = false;
	for (MeshDragState &state : states) {
		state.geometry_diff = p_to_grid ? state.mesh->align_faces_to_grid(state.face_ids) :
				state.mesh->align_faces_to_face(state.face_ids);
		changed = changed || state.geometry_diff.is_valid();
	}
	if (!changed) {
		return false;
	}
	_register_mesh_undo(p_to_grid ? TTR("Align Face Texture to Grid") : TTR("Align Face Texture to Face"), states);
	if (LevelEditorView *level_view = get_view()) {
		level_view->set_last_selection_action(p_to_grid ? SNAME("texture_align_grid") : SNAME("texture_align_face"));
	}
	return true;
}

bool SelectTool::_pick_texture_face(Camera3D *p_camera, const Vector2 &p_position,
		LevelBlock *&r_block, Ref<LevelMesh> &r_mesh, int &r_face_id) const {
	r_block = nullptr;
	r_mesh.unref();
	r_face_id = -1;
	const Vector<SurfaceHit> hits = _query_surface_hits(p_camera, p_position);
	if (hits.is_empty() || !hits[0].block || hits[0].face_id < 0) {
		return false;
	}
	r_block = hits[0].block;
	r_mesh = r_block->get_level_mesh();
	r_face_id = hits[0].face_id;
	return r_mesh.is_valid();
}

bool SelectTool::_lift_face_texture(Camera3D *p_camera, const Vector2 &p_position) {
	LevelBlock *block = nullptr;
	Ref<LevelMesh> mesh;
	int face_id = -1;
	if (!_pick_texture_face(p_camera, p_position, block, mesh, face_id)) {
		return false;
	}
	const Dictionary capture = mesh->capture_face_texture(face_id);
	if (!bool(capture.get("valid", false))) {
		_show_texture_status(TTR("The picked face has no bound material to lift."), true);
		return true;
	}
	const String material_path = capture.get("material_path", String());
	const Ref<Material> material = LevelMeshBaker::resolve_material_path(material_path);
	if (material.is_null()) {
		_show_texture_status(TTR("The picked face's material could not be loaded."), true);
		return true;
	}
	LevelEditor *level_editor = LevelEditor::get_singleton();
	LevelEditorView *level_view = get_view();
	LevelDocument *document = level_view ? level_view->get_level_document() : nullptr;
	if (!document || !level_editor) {
		return false;
	}
	const String source_path = material_path.begins_with("builtin://") ? String() : material_path;
	// This existing setter emits active_material_changed, which keeps WP14's
	// browser scroll-to behavior on the same signal path.
	level_editor->set_active_material(document, material, source_path);
	level_editor->set_active_material_binding_path(document, material_path);
	level_editor->set_captured_mapping(document, capture);
	level_view->set_meta(StringName("_level_lifted_face"), face_id);
	level_view->set_meta(StringName("_level_lift_has_mapping"), bool(capture.get("has_mapping", false)));
	level_view->set_last_selection_action(SNAME("texture_lift"));
	if (!bool(capture.get("has_mapping", false))) {
		_show_texture_status(TTR("Lift captured the material only because the source uses explicit UVs."));
	}
	return true;
}

bool SelectTool::_wrap_face_texture(Camera3D *p_camera, const Vector2 &p_position) {
	LevelBlock *block = nullptr;
	Ref<LevelMesh> mesh;
	int destination_face = -1;
	if (!_pick_texture_face(p_camera, p_position, block, mesh, destination_face)) {
		return false;
	}
	const Ref<LevelMeshData> mesh_data = mesh->get_data();
	if (mesh_data.is_null()) {
		return false;
	}

	Vector<int> candidates;
	for (const int edge_id : mesh->get_adjacency()->get_face_edges(destination_face)) {
		if (edge_id < 0) {
			continue;
		}
		for (const int neighbor : mesh->get_adjacency()->get_edge_faces(edge_id)) {
			if (neighbor == destination_face || candidates.has(neighbor) ||
					neighbor < 0 || neighbor >= mesh_data->get_face_uv_modes().size() ||
					mesh_data->get_face_uv_modes()[neighbor] != LevelMeshData::UV_MODE_PROJECTED ||
					neighbor >= mesh_data->get_face_material_indices().size() ||
					mesh_data->get_material_path(mesh_data->get_face_material_indices()[neighbor]).is_empty()) {
				continue;
			}
			const Dictionary solution = mesh->calculate_wrap_transform(neighbor, destination_face);
			if (bool(solution.get("valid", false))) {
				candidates.push_back(neighbor);
			}
		}
	}
	if (candidates.is_empty()) {
		_show_texture_status(TTR("Wrap needs an adjacent projected, textured source face."), true);
		return true;
	}

	Vector<MeshDragState> selected_states;
	_collect_face_selection(selected_states, block);
	PackedInt32Array selected_faces;
	for (const MeshDragState &state : selected_states) {
		if (state.block == block) {
			selected_faces = state.face_ids;
			break;
		}
	}
	Vector<int> preferred;
	for (const int candidate : candidates) {
		if (selected_faces.has(candidate)) {
			preferred.push_back(candidate);
		}
	}
	if (preferred.is_empty()) {
		preferred = candidates;
	}
	preferred.sort();

	Transform2D reference_transform;
	bool have_reference = false;
	for (const int candidate : preferred) {
		const Dictionary solution = mesh->calculate_wrap_transform(candidate, destination_face);
		const Transform2D transform = solution.get("transform", Transform2D());
		if (!have_reference) {
			reference_transform = transform;
			have_reference = true;
		} else if (!reference_transform.is_equal_approx(transform)) {
			_show_texture_status(TTR("Wrap rejected conflicting projected neighbors; resolve the seam or select one source."), true);
			return true;
		}
	}

	MeshDragState state;
	state.block = block;
	state.mesh = mesh;
	state.face_ids.push_back(destination_face);
	state.geometry_diff = mesh->wrap_faces(preferred[0], state.face_ids);
	if (state.geometry_diff.is_null()) {
		_show_texture_status(TTR("Wrap could not solve the picked shared edge."), true);
		return true;
	}
	Vector<MeshDragState> states;
	states.push_back(state);
	_register_mesh_undo(TTR("Wrap Face Texture"), states);
	if (LevelEditorView *level_view = get_view()) {
		level_view->set_last_selection_action(SNAME("texture_wrap"));
	}
	return true;
}

bool SelectTool::_wrap_texture_to_selection(Camera3D *p_camera, const Vector2 &p_position) {
	LevelBlock *block = nullptr;
	Ref<LevelMesh> mesh;
	int source_face = -1;
	if (!_pick_texture_face(p_camera, p_position, block, mesh, source_face)) {
		return false;
	}
	if (!bool(mesh->capture_face_texture(source_face).get("valid", false))) {
		_show_texture_status(TTR("Wrap to Selection needs a textured source face."), true);
		return true;
	}
	Vector<MeshDragState> selected_states;
	if (!_collect_face_selection(selected_states, block)) {
		return false;
	}
	PackedInt32Array destinations;
	for (const MeshDragState &state : selected_states) {
		if (state.block != block) {
			continue;
		}
		for (const int face_id : state.face_ids) {
			if (face_id != source_face) {
				destinations.push_back(face_id);
			}
		}
	}
	if (destinations.is_empty()) {
		return false;
	}
	MeshDragState state;
	state.block = block;
	state.mesh = mesh;
	state.face_ids = destinations;
	state.geometry_diff = mesh->wrap_faces(source_face, destinations);
	if (state.geometry_diff.is_null()) {
		_show_texture_status(TTR("Wrap to Selection requires one edge-connected face region."), true);
		return true;
	}
	Vector<MeshDragState> states;
	states.push_back(state);
	_register_mesh_undo(TTR("Wrap Texture to Selection"), states);
	if (LevelEditorView *level_view = get_view()) {
		level_view->set_last_selection_action(SNAME("texture_wrap_selection"));
	}
	return true;
}

bool SelectTool::_begin_texture_flow(Camera3D *p_camera, const Vector2 &p_position) {
	LevelBlock *block = nullptr;
	Ref<LevelMesh> mesh;
	int source_face = -1;
	if (!_pick_texture_face(p_camera, p_position, block, mesh, source_face)) {
		return false;
	}
	if (!bool(mesh->capture_face_texture(source_face).get("valid", false))) {
		_show_texture_status(TTR("Flow needs a textured source face."), true);
		return true;
	}
	texture_flow_active = true;
	texture_flow_block = block;
	texture_flow_mesh = mesh;
	texture_flow_faces.clear();
	texture_flow_faces.push_back(source_face);
	if (LevelEditorView *level_view = get_view()) {
		level_view->set_meta(StringName("_level_texture_flow_count"), 1);
	}
	return true;
}

bool SelectTool::_append_texture_flow_hover(Camera3D *p_camera, const Vector2 &p_position) {
	if (!texture_flow_active || texture_flow_mesh.is_null() || texture_flow_faces.is_empty()) {
		return false;
	}
	LevelBlock *block = nullptr;
	Ref<LevelMesh> mesh;
	int face_id = -1;
	if (!_pick_texture_face(p_camera, p_position, block, mesh, face_id) || block != texture_flow_block ||
			mesh != texture_flow_mesh) {
		return true;
	}
	const int previous = texture_flow_faces[texture_flow_faces.size() - 1];
	if (face_id == previous || texture_flow_faces.has(face_id)) {
		return true;
	}
	const Dictionary edge_check = texture_flow_mesh->calculate_wrap_transform(previous, face_id);
	if (!bool(edge_check.get("valid", false))) {
		return true;
	}
	texture_flow_faces.push_back(face_id);
	if (LevelEditorView *level_view = get_view()) {
		level_view->set_meta(StringName("_level_texture_flow_count"), texture_flow_faces.size());
	}
	return true;
}

bool SelectTool::_commit_texture_flow() {
	if (!texture_flow_active || texture_flow_mesh.is_null() || texture_flow_faces.size() < 2) {
		return false;
	}
	MeshDragState state;
	state.block = texture_flow_block;
	state.mesh = texture_flow_mesh;
	state.face_ids = texture_flow_faces;
	state.geometry_diff = texture_flow_mesh->flow_faces(texture_flow_faces);
	if (state.geometry_diff.is_null()) {
		_show_texture_status(TTR("Flow could not propagate across the hovered face chain."), true);
		return false;
	}
	Vector<MeshDragState> states;
	states.push_back(state);
	_register_mesh_undo(TTR("Flow Face Texture"), states);
	if (LevelEditorView *level_view = get_view()) {
		level_view->set_last_selection_action(SNAME("texture_flow"));
	}
	return true;
}

bool SelectTool::_commit_gesture() {
	if (texture_flow_active) {
		return _commit_texture_flow();
	}
	return transform_active && _commit_transform_drag();
}

bool SelectTool::_handle_texture_key(const Ref<InputEventKey> &p_key) {
	if (p_key.is_null() || !p_key->is_pressed() || p_key->is_echo()) {
		return false;
	}
	const Key code = p_key->get_keycode() != Key::NONE ? p_key->get_keycode() : p_key->get_physical_keycode();
	const bool shift = p_key->is_shift_pressed();
	const bool ctrl = p_key->is_ctrl_pressed();
	const bool alt = p_key->is_alt_pressed();
	if (shift && !ctrl && !alt && code == Key::T) {
		return apply_active_material();
	}
	if (shift && !ctrl && !alt && code == Key::H) {
		return _apply_hotspot_fit(false);
	}
	if (shift && !ctrl && !alt && code == Key::F) {
		return _apply_hotspot_fit(true);
	}
	if (shift && ctrl && !alt && code == Key::G) {
		return _align_selected_texture(true);
	}
	if (shift && ctrl && !alt && code == Key::F) {
		return _align_selected_texture(false);
	}
	if (!shift && !ctrl && !alt) {
		Vector2 direction;
		switch (code) {
			case Key::KP_1: direction = Vector2(-1, 1); break;
			case Key::KP_2: direction = Vector2(0, 1); break;
			case Key::KP_3: direction = Vector2(1, 1); break;
			case Key::KP_4: direction = Vector2(-1, 0); break;
			case Key::KP_6: direction = Vector2(1, 0); break;
			case Key::KP_7: direction = Vector2(-1, -1); break;
			case Key::KP_8: direction = Vector2(0, -1); break;
			case Key::KP_9: direction = Vector2(1, -1); break;
			default: return false;
		}
		return modify_selected_texture(LevelMesh::TEXTURE_MODIFY_SHIFT, direction);
	}
	if (!shift && ctrl && alt) {
		switch (code) {
			case Key::KP_4: return modify_selected_texture(LevelMesh::TEXTURE_MODIFY_SCALE, Vector2(-1, 0));
			case Key::KP_6: return modify_selected_texture(LevelMesh::TEXTURE_MODIFY_SCALE, Vector2(1, 0));
			case Key::KP_2: return modify_selected_texture(LevelMesh::TEXTURE_MODIFY_SCALE, Vector2(0, -1));
			case Key::KP_8: return modify_selected_texture(LevelMesh::TEXTURE_MODIFY_SCALE, Vector2(0, 1));
			case Key::KP_5: return modify_selected_texture(LevelMesh::TEXTURE_MODIFY_FIT, Vector2(1, 1));
			default: return false;
		}
	}
	if (!shift && !ctrl && alt) {
		switch (code) {
			case Key::KP_7: return modify_selected_texture(LevelMesh::TEXTURE_MODIFY_ROTATE, Vector2(1, 0));
			case Key::KP_9: return modify_selected_texture(LevelMesh::TEXTURE_MODIFY_ROTATE, Vector2(-1, 0));
			case Key::KP_4: return modify_selected_texture(LevelMesh::TEXTURE_MODIFY_JUSTIFY_LEFT, Vector2(1, 1));
			case Key::KP_6: return modify_selected_texture(LevelMesh::TEXTURE_MODIFY_JUSTIFY_RIGHT, Vector2(1, 1));
			case Key::KP_8: return modify_selected_texture(LevelMesh::TEXTURE_MODIFY_JUSTIFY_TOP, Vector2(1, 1));
			case Key::KP_2: return modify_selected_texture(LevelMesh::TEXTURE_MODIFY_JUSTIFY_BOTTOM, Vector2(1, 1));
			case Key::KP_5: return modify_selected_texture(LevelMesh::TEXTURE_MODIFY_JUSTIFY_CENTER, Vector2(1, 1));
			case Key::T: return modify_selected_texture(LevelMesh::TEXTURE_MODIFY_FLIP_VERTICAL, Vector2(1, 1));
			case Key::R: return modify_selected_texture(LevelMesh::TEXTURE_MODIFY_FLIP_HORIZONTAL, Vector2(1, 1));
			default: return false;
		}
	}
	return false;
}
