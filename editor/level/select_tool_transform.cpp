/**************************************************************************/
/*  select_tool_transform.cpp                                             */
/**************************************************************************/

#include "select_tool.h"

#include "editor/editor_data.h"
#include "editor/editor_document.h"
#include "editor/editor_undo_redo_manager.h"
#include "editor/level/level_editor.h"
#include "editor/level/level_editor_view.h"
#include "editor/level/level_snap_service.h"
#include "editor/themes/editor_scale.h"
#include "scene/3d/camera_3d.h"
#include "scene/main/viewport.h"

#include "modules/level_kernel/level_block.h"
#include "modules/level_kernel/level_mesh.h"
#include "modules/level_kernel/level_mesh_adjacency.h"
#include "modules/level_kernel/level_mesh_data.h"
#include "modules/level_kernel/level_mesh_diff.h"

namespace {

bool packed_has(const PackedInt32Array &p_values, int p_value) {
	return p_values.has(p_value);
}

bool element_is_selected(const Vector<SelectionModel::Element> &p_selected, const SelectionModel::Element &p_element) {
	return p_selected.has(p_element);
}

} // namespace

void SelectTool::MeshDragState::capture_original_positions() {
	const PackedVector3Array positions = block->get_data()->get_vertex_positions();
	original_positions.resize(vertex_ids.size());
	for (int i = 0; i < vertex_ids.size(); i++) {
		original_positions.set(i, positions[vertex_ids[i]]);
	}
}

bool SelectTool::_press_hits_current_selection(Camera3D *p_camera, const Vector2 &p_position) const {
	SelectionModel *selection_model = get_selection_model();
	if (!selection_model || !p_camera) {
		return false;
	}
	const Vector<SurfaceHit> hits = _query_surface_hits(p_camera, p_position);
	if (hits.is_empty()) {
		return false;
	}
	if (selection_model->get_mode() == SelectionModel::MODE_OBJECT) {
		LevelEditorView *level_view = get_view();
		LevelDocument *document = level_view ? level_view->get_level_document() : nullptr;
		EditorSelection *selection = document ? document->get_selection() : nullptr;
		return selection && selection->is_selected(hits[0].block);
	}

	const SelectionModel::Feature feature = _feature_for_mode(selection_model->get_mode());
	Vector<SelectionModel::Element> candidates;
	switch (selection_model->get_mode()) {
		case SelectionModel::MODE_VERTEX:
			candidates = _resolve_vertex(p_camera, p_position, hits);
			break;
		case SelectionModel::MODE_EDGE:
			candidates = _resolve_edge(p_camera, p_position, hits[0]);
			break;
		case SelectionModel::MODE_FACE:
			candidates = _resolve_face(hits[0], true);
			break;
		default:
			break;
	}
	const Vector<SelectionModel::Element> &selected = selection_model->get_selected(feature);
	for (const SelectionModel::Element &candidate : candidates) {
		if (element_is_selected(selected, candidate)) {
			return true;
		}
	}
	return false;
}

bool SelectTool::_collect_transform_selection() {
	mesh_drag_states.clear();
	object_drag_states.clear();
	SelectionModel *selection_model = get_selection_model();
	LevelEditorView *level_view = get_view();
	LevelDocument *document = level_view ? level_view->get_level_document() : nullptr;
	if (!selection_model || !document) {
		return false;
	}
	if (selection_model->get_mode() == SelectionModel::MODE_OBJECT) {
		EditorSelection *selection = document->get_selection();
		if (!selection) {
			return false;
		}
		const TypedArray<Node> nodes = selection->get_selected_nodes();
		for (int i = 0; i < nodes.size(); i++) {
			LevelBlock *block = Object::cast_to<LevelBlock>(nodes[i]);
			if (!block) {
				continue;
			}
			ObjectDragState state;
			state.block = block;
			state.original_transform = block->get_global_transform();
			state.preview_transform = state.original_transform;
			object_drag_states.push_back(state);
		}
		return !object_drag_states.is_empty();
	}

	const SelectionModel::Feature feature = _feature_for_mode(selection_model->get_mode());
	const Vector<SelectionModel::Element> &elements = selection_model->get_selected(feature);
	for (const SelectionModel::Element &element : elements) {
		LevelBlock *block = nullptr;
		Ref<LevelMesh> mesh;
		int slot = -1;
		if (!selection_model->resolve(element, block, mesh, slot)) {
			continue;
		}
		int state_index = -1;
		for (int i = 0; i < mesh_drag_states.size(); i++) {
			if (mesh_drag_states[i].block == block) {
				state_index = i;
				break;
			}
		}
		if (state_index < 0) {
			MeshDragState state;
			state.block = block;
			state.mesh = mesh;
			mesh_drag_states.push_back(state);
			state_index = mesh_drag_states.size() - 1;
		}
		MeshDragState &state = mesh_drag_states.write[state_index];
		auto append_vertex = [&](int p_vertex_id) {
			if (p_vertex_id >= 0 && !packed_has(state.vertex_ids, p_vertex_id)) {
				state.vertex_ids.push_back(p_vertex_id);
			}
		};

		if (selection_model->get_mode() == SelectionModel::MODE_VERTEX) {
			append_vertex(slot);
		} else if (selection_model->get_mode() == SelectionModel::MODE_EDGE) {
			if (element.handle_kind == SelectionModel::HANDLE_EDGE) {
				if (!packed_has(state.edge_ids, slot)) {
					state.edge_ids.push_back(slot);
				}
				for (const int vertex_id : mesh->get_adjacency()->get_edge_vertices(slot)) {
					append_vertex(vertex_id);
				}
			} else if (element.handle_kind == SelectionModel::HANDLE_FACE) {
				int corner_a = -1;
				int corner_b = -1;
				if (SelectionModel::decode_corner_pair(element.sub_index, corner_a, corner_b)) {
					const PackedInt32Array corners = mesh->get_face_corner_vertex_ids(slot);
					if (corner_a >= 0 && corner_a < corners.size() && corner_b >= 0 && corner_b < corners.size()) {
						append_vertex(corners[corner_a]);
						append_vertex(corners[corner_b]);
					}
				}
			}
		} else if (selection_model->get_mode() == SelectionModel::MODE_FACE) {
			if (!packed_has(state.face_ids, slot)) {
				state.face_ids.push_back(slot);
			}
			if (element.tier == SelectionModel::TIER_TRIANGLE && element.sub_index >= 0) {
				const PackedInt32Array triangle_vertices = mesh->get_face_triangle_vertex_ids(slot, (int)element.sub_index);
				if (triangle_vertices.size() == 3) {
					for (const int vertex_id : triangle_vertices) {
						append_vertex(vertex_id);
					}
				}
			} else {
				for (const int vertex_id : mesh->get_face_corner_vertex_ids(slot)) {
					append_vertex(vertex_id);
				}
			}
		}
	}
	for (int i = mesh_drag_states.size() - 1; i >= 0; i--) {
		if (mesh_drag_states[i].vertex_ids.is_empty()) {
			mesh_drag_states.remove_at(i);
		}
	}
	return !mesh_drag_states.is_empty();
}

bool SelectTool::_open_mesh_previews() {
	for (int i = 0; i < mesh_drag_states.size(); i++) {
		MeshDragState &state = mesh_drag_states.write[i];
		if (!state.mesh->begin_transform_preview(state.vertex_ids)) {
			for (int opened = 0; opened < i; opened++) {
				mesh_drag_states.write[opened].mesh->cancel_transform_preview();
			}
			return false;
		}
	}
	return true;
}

bool SelectTool::_is_move_drag() const {
	return transform_drag_mode == TRANSFORM_DRAG_MOVE || transform_drag_mode == TRANSFORM_DRAG_OBJECT_MOVE;
}

bool SelectTool::_is_rotation_drag() const {
	return transform_drag_mode == TRANSFORM_DRAG_ROTATE;
}

bool SelectTool::_is_object_drag() const {
	return transform_drag_mode == TRANSFORM_DRAG_OBJECT_MOVE ||
			(_is_rotation_drag() && !object_drag_states.is_empty());
}

void SelectTool::_reset_transform_constraint() {
	transform_constraint_mode = TRANSFORM_CONSTRAINT_FREE;
	transform_constraint_axis = Vector3::AXIS_X;
	get_overlay().clear();
}

void SelectTool::_end_transform_drag(bool p_committed) {
	transform_active = false;
	transform_committed = p_committed;
	_reset_transform_constraint();
}

bool SelectTool::_closest_axis_parameter(Camera3D *p_camera, const Vector2 &p_position, const Vector3 &p_axis, real_t &r_parameter) const {
	const Vector3 ray_origin = p_camera->project_ray_origin(p_position);
	const Vector3 ray_direction = p_camera->project_ray_normal(p_position);
	const Vector3 w0 = ray_origin - transform_pivot;
	const real_t ray_axis = ray_direction.dot(p_axis);
	const real_t denominator = 1.0 - ray_axis * ray_axis;
	if (denominator <= (real_t)1e-4) {
		return false;
	}
	r_parameter = (p_axis.dot(w0) - ray_axis * ray_direction.dot(w0)) / denominator;
	return true;
}

bool SelectTool::_rederive_transform_press_reference(Camera3D *p_camera, TransformConstraintMode p_mode, int p_axis) {
	if (!p_camera) {
		return false;
	}

	Vector3 axis = transform_axis;
	if (_is_move_drag()) {
		axis = Vector3();
		if (p_mode == TRANSFORM_CONSTRAINT_AXIS) {
			axis[p_axis] = 1.0;
		}
	}

	Plane drag_plane;
	Vector3 press_point;
	real_t press_axis_parameter = 0.0;
	bool screen_fallback = false;
	if (axis.length_squared() > CMP_EPSILON2) {
		screen_fallback = !_closest_axis_parameter(p_camera, press_position, axis, press_axis_parameter);
	} else {
		Vector3 plane_normal = -p_camera->get_global_transform().basis.get_column(2).normalized();
		if (_is_move_drag() && p_mode == TRANSFORM_CONSTRAINT_PLANE) {
			plane_normal = Vector3();
			plane_normal[p_axis] = 1.0;
		}
		drag_plane = Plane(plane_normal, transform_pivot);
		if (!drag_plane.intersects_ray(p_camera->project_ray_origin(press_position),
					p_camera->project_ray_normal(press_position), &press_point)) {
			return false;
		}
	}

	transform_constraint_mode = p_mode;
	transform_constraint_axis = p_axis;
	if (_is_move_drag()) {
		transform_axis = axis;
	}
	transform_drag_plane = drag_plane;
	transform_press_point = press_point;
	transform_press_axis_parameter = press_axis_parameter;
	transform_axis_screen_fallback = screen_fallback;
	return true;
}

bool SelectTool::_cycle_transform_constraint(Camera3D *p_camera, int p_axis) {
	ERR_FAIL_INDEX_V(p_axis, 3, false);
	if (!transform_active) {
		return false;
	}
	if (_is_rotation_drag()) {
		const bool return_to_view_axis = transform_constraint_mode == TRANSFORM_CONSTRAINT_AXIS && transform_constraint_axis == p_axis;
		transform_constraint_mode = return_to_view_axis ? TRANSFORM_CONSTRAINT_FREE : TRANSFORM_CONSTRAINT_AXIS;
		transform_constraint_axis = p_axis;
		if (transform_constraint_mode == TRANSFORM_CONSTRAINT_FREE) {
			transform_axis = transform_view_axis;
			get_overlay().clear();
		} else {
			transform_axis = Vector3();
			transform_axis[p_axis] = 1.0;
			get_overlay().update_constraint_guides(transform_pivot, p_axis, false);
		}
		// Axis changes preserve the already resolved (and possibly snapped)
		// total screen-space sweep; only the world rotation basis changes.
		_apply_rotation_preview(transform_rotation_angle);
		return true;
	}
	if (!_is_move_drag()) {
		return false;
	}

	TransformConstraintMode next_mode = TRANSFORM_CONSTRAINT_AXIS;
	if (transform_constraint_axis == p_axis && transform_constraint_mode == TRANSFORM_CONSTRAINT_AXIS) {
		next_mode = TRANSFORM_CONSTRAINT_PLANE;
	} else if (transform_constraint_axis == p_axis && transform_constraint_mode == TRANSFORM_CONSTRAINT_PLANE) {
		next_mode = TRANSFORM_CONSTRAINT_FREE;
	}

	if (!_rederive_transform_press_reference(p_camera, next_mode, p_axis)) {
		// Derivation failed (degenerate plane intersect); constraint state is untouched.
		return true;
	}

	if (transform_constraint_mode == TRANSFORM_CONSTRAINT_FREE) {
		get_overlay().clear();
	} else {
		get_overlay().update_constraint_guides(transform_pivot, transform_constraint_axis,
				transform_constraint_mode == TRANSFORM_CONSTRAINT_PLANE);
	}
	_update_transform_drag(p_camera, current_position, false);
	return true;
}

bool SelectTool::_begin_transform_drag(Camera3D *p_camera, TransformDragMode p_requested_mode) {
	if (transform_active) {
		return false;
	}
	ERR_FAIL_COND_V(p_requested_mode != TRANSFORM_DRAG_NONE && p_requested_mode != TRANSFORM_DRAG_ROTATE, false);
	_reset_transform_constraint();
	if (!p_camera || !_collect_transform_selection()) {
		return false;
	}
	SelectionModel *selection_model = get_selection_model();
	if (!selection_model) {
		return false;
	}
	const bool rotation_requested = p_requested_mode == TRANSFORM_DRAG_ROTATE;
	transform_drag_mode = rotation_requested ? TRANSFORM_DRAG_ROTATE :
			(selection_model->get_mode() == SelectionModel::MODE_OBJECT ? TRANSFORM_DRAG_OBJECT_MOVE : TRANSFORM_DRAG_MOVE);
	if (!rotation_requested && selection_model->get_mode() == SelectionModel::MODE_FACE && gesture_shift) {
		transform_drag_mode = gesture_ctrl ? TRANSFORM_DRAG_PUSH_PULL : TRANSFORM_DRAG_FACE_EXTRUDE;
	} else if (!rotation_requested && selection_model->get_mode() == SelectionModel::MODE_EDGE && gesture_shift && !gesture_ctrl) {
		transform_drag_mode = TRANSFORM_DRAG_EDGE_EXTRUDE;
	}

	transform_pivot = Vector3();
	int pivot_count = 0;
	if (_is_object_drag()) {
		for (const ObjectDragState &state : object_drag_states) {
			transform_pivot += state.original_transform.origin;
			pivot_count++;
		}
	} else {
		for (MeshDragState &state : mesh_drag_states) {
			const PackedVector3Array positions = state.block->get_data()->get_vertex_positions();
			for (const int vertex_id : state.vertex_ids) {
				if (vertex_id >= 0 && vertex_id < positions.size()) {
					transform_pivot += state.block->get_global_transform().xform(positions[vertex_id]);
					pivot_count++;
				}
			}
		}
	}
	if (pivot_count == 0) {
		return false;
	}
	transform_pivot /= (real_t)pivot_count;

	transform_axis = Vector3();
	if (transform_drag_mode == TRANSFORM_DRAG_FACE_EXTRUDE || transform_drag_mode == TRANSFORM_DRAG_PUSH_PULL) {
		for (const MeshDragState &state : mesh_drag_states) {
			for (const int face_id : state.face_ids) {
				const Vector3 local_normal = state.mesh->get_face_normal(face_id);
				if (local_normal.length_squared() <= CMP_EPSILON2) {
					return false;
				}
				const Basis normal_basis = state.block->get_global_transform().basis.inverse().transposed();
				transform_axis += normal_basis.xform(local_normal).normalized();
			}
		}
		if (transform_axis.length_squared() <= CMP_EPSILON2) {
			return false;
		}
		transform_axis.normalize();
	}

	if (transform_drag_mode == TRANSFORM_DRAG_FACE_EXTRUDE || transform_drag_mode == TRANSFORM_DRAG_EDGE_EXTRUDE) {
		for (int i = 0; i < mesh_drag_states.size(); i++) {
			MeshDragState &state = mesh_drag_states.write[i];
			state.topology_diff = transform_drag_mode == TRANSFORM_DRAG_FACE_EXTRUDE ? state.mesh->extrude_faces(state.face_ids) : state.mesh->extrude_boundary_edges(state.edge_ids);
			if (state.topology_diff.is_null()) {
				_revert_topology_diffs();
				return false;
			}
			state.created_face_handles = state.topology_diff->get_created_face_handles();
			state.vertex_ids.clear();
			for (const int64_t handle : state.topology_diff->get_created_vertex_handles()) {
				const int vertex_id = state.mesh->resolve_vertex(handle);
				if (vertex_id >= 0) {
					state.vertex_ids.push_back(vertex_id);
				}
			}
			if (state.vertex_ids.is_empty()) {
				_revert_topology_diffs();
				return false;
			}
		}
	} else if (transform_drag_mode == TRANSFORM_DRAG_PUSH_PULL) {
		for (MeshDragState &state : mesh_drag_states) {
			const Dictionary push_pull = state.mesh->calculate_push_pull(state.face_ids, 1.0);
			if (!(bool)push_pull.get("valid", false)) {
				return false;
			}
			state.vertex_ids = push_pull.get("vertex_ids", PackedInt32Array());
			const PackedVector3Array unit_positions = push_pull.get("positions", PackedVector3Array());
			const PackedVector3Array current_positions = state.block->get_data()->get_vertex_positions();
			if (state.vertex_ids.size() != unit_positions.size()) {
				return false;
			}
			state.push_directions.resize(state.vertex_ids.size());
			for (int vertex = 0; vertex < state.vertex_ids.size(); vertex++) {
				state.push_directions.set(vertex, unit_positions[vertex] - current_positions[state.vertex_ids[vertex]]);
			}
		}
	}

	if (!_is_object_drag()) {
		for (MeshDragState &state : mesh_drag_states) {
			state.capture_original_positions();
		}
		if (!_open_mesh_previews()) {
			_revert_topology_diffs();
			return false;
		}
	}

	transform_snap_step = LevelEditor::snap_step_or_default();
	if (_is_rotation_drag()) {
		transform_view_axis = -p_camera->get_global_transform().basis.get_column(2).normalized();
		transform_axis = transform_view_axis;
		transform_rotation_pivot_screen = p_camera->unproject_position(transform_pivot);
		transform_rotation_press_vector = press_position - transform_rotation_pivot_screen;
		const real_t pivot_guard = 4.0 * EDSCALE;
		transform_rotation_reference_valid = transform_rotation_press_vector.length_squared() >= pivot_guard * pivot_guard;
		transform_rotation_angle = 0.0;
	} else if (!_rederive_transform_press_reference(p_camera, transform_constraint_mode, transform_constraint_axis)) {
		_cancel_transform_drag();
		return false;
	}
	transform_active = true;
	transform_committed = false;
	return true;
}

bool SelectTool::_resolve_drag_delta(Camera3D *p_camera, const Vector2 &p_position, Vector3 &r_delta) const {
	if (!p_camera) {
		return false;
	}
	if (transform_axis.length_squared() > CMP_EPSILON2) {
		real_t parameter = 0.0;
		if (transform_axis_screen_fallback || !_closest_axis_parameter(p_camera, p_position, transform_axis, parameter)) {
			const Vector3 ray_origin = p_camera->project_ray_origin(p_position);
			const Vector3 ray_direction = p_camera->project_ray_normal(p_position);
			const real_t depth = MAX((real_t)0.1, (transform_pivot - ray_origin).dot(ray_direction));
			const real_t viewport_height = MAX((real_t)1.0, (real_t)p_camera->get_viewport()->get_visible_rect().size.y);
			const real_t world_per_pixel = 2.0 * depth * Math::tan(Math::deg_to_rad(p_camera->get_fov()) * (real_t)0.5) / viewport_height;
			parameter = -(p_position.y - press_position.y) * world_per_pixel;
		}
		r_delta = transform_axis * (parameter - transform_press_axis_parameter);
		return true;
	}
	Vector3 point;
	if (!transform_drag_plane.intersects_ray(p_camera->project_ray_origin(p_position), p_camera->project_ray_normal(p_position), &point)) {
		return false;
	}
	r_delta = point - transform_press_point;
	if (_is_move_drag() && transform_constraint_mode == TRANSFORM_CONSTRAINT_PLANE) {
		// The drag plane's normal is already the constraint axis; this only scrubs float noise to an exact zero.
		r_delta[transform_constraint_axis] = 0.0;
	}
	return true;
}

bool SelectTool::_apply_mesh_preview_delta(const Vector3 &p_world_delta) {
	for (MeshDragState &state : mesh_drag_states) {
		PackedVector3Array positions;
		positions.resize(state.vertex_ids.size());
		if (transform_drag_mode == TRANSFORM_DRAG_PUSH_PULL) {
			const real_t signed_distance = p_world_delta.dot(transform_axis);
			for (int i = 0; i < state.vertex_ids.size(); i++) {
				const Vector3 world_direction = state.block->get_global_transform().basis.xform(state.push_directions[i]);
				const real_t world_length = world_direction.length();
				if (world_length <= CMP_EPSILON) {
					return false;
				}
				positions.set(i, state.original_positions[i] + state.push_directions[i] * (signed_distance / world_length));
			}
		} else {
			const Vector3 local_delta = state.block->get_global_transform().basis.inverse().xform(p_world_delta);
			for (int i = 0; i < state.vertex_ids.size(); i++) {
				positions.set(i, state.original_positions[i] + local_delta);
			}
		}
		if (!state.mesh->preview_transform_vertices(positions)) {
			return false;
		}
	}
	return true;
}

bool SelectTool::_apply_rotation_preview(real_t p_angle) {
	ERR_FAIL_COND_V(!_is_rotation_drag() || !Math::is_finite(p_angle), false);
	ERR_FAIL_COND_V(transform_axis.length_squared() <= CMP_EPSILON2, false);
	const Basis rotation(transform_axis.normalized(), p_angle);
	if (_is_object_drag()) {
		for (ObjectDragState &state : object_drag_states) {
			state.preview_transform = state.original_transform;
			state.preview_transform.basis = rotation * state.original_transform.basis;
			state.preview_transform.origin = transform_pivot + rotation.xform(state.original_transform.origin - transform_pivot);
			state.block->set_global_transform(state.preview_transform);
		}
		return true;
	}

	for (MeshDragState &state : mesh_drag_states) {
		PackedVector3Array positions;
		positions.resize(state.vertex_ids.size());
		const Transform3D block_transform = state.block->get_global_transform();
		const Transform3D block_inverse = block_transform.affine_inverse();
		for (int i = 0; i < state.vertex_ids.size(); i++) {
			const Vector3 original_world = block_transform.xform(state.original_positions[i]);
			const Vector3 rotated_world = transform_pivot + rotation.xform(original_world - transform_pivot);
			positions.set(i, block_inverse.xform(rotated_world));
		}
		if (!state.mesh->preview_transform_vertices(positions)) {
			return false;
		}
	}
	return true;
}

void SelectTool::_revert_topology_diffs() {
	for (int i = mesh_drag_states.size() - 1; i >= 0; i--) {
		MeshDragState &state = mesh_drag_states.write[i];
		if (state.mesh.is_valid() && state.topology_diff.is_valid()) {
			state.mesh->revert_diff(state.topology_diff);
		}
	}
}

bool SelectTool::_update_transform_drag(Camera3D *p_camera, const Vector2 &p_position, bool p_ctrl_pressed) {
	if (!transform_active) {
		return false;
	}
	if (_is_rotation_drag()) {
		if (!p_camera) {
			return false;
		}
		const Vector2 current_vector = p_position - transform_rotation_pivot_screen;
		const real_t pivot_guard = 4.0 * EDSCALE;
		if (current_vector.length_squared() < pivot_guard * pivot_guard) {
			return true;
		}
		if (!transform_rotation_reference_valid) {
			// A modal begun directly over the pivot arms at the first safe
			// pointer position instead of manufacturing an unstable angle.
			transform_rotation_press_vector = current_vector;
			transform_rotation_reference_valid = true;
			return true;
		}
		real_t angle = transform_rotation_press_vector.angle_to(current_vector);
		LevelEditor *level_editor = LevelEditor::get_singleton();
		const bool snap_enabled = level_editor ? level_editor->is_snap_enabled() : true;
		if (snap_enabled != p_ctrl_pressed) {
			angle = LevelSnapService::snap_angle(angle);
		}
		transform_rotation_angle = angle;
		return _apply_rotation_preview(angle);
	}
	Vector3 delta;
	if (!_resolve_drag_delta(p_camera, p_position, delta)) {
		return false;
	}
	LevelEditor *level_editor = LevelEditor::get_singleton();
	const bool snap_enabled = level_editor ? level_editor->is_snap_enabled() : true;
	if (snap_enabled != p_ctrl_pressed) {
		delta = LevelSnapService::snap_delta(delta, transform_snap_step);
	}
	if (transform_drag_mode == TRANSFORM_DRAG_OBJECT_MOVE) {
		for (ObjectDragState &state : object_drag_states) {
			state.preview_transform = state.original_transform;
			state.preview_transform.origin += delta;
			state.block->set_global_transform(state.preview_transform);
		}
		return true;
	}
	return _apply_mesh_preview_delta(delta);
}

void SelectTool::_register_mesh_undo(const String &p_action_name, const Vector<MeshDragState> &p_states) {
	LevelEditorView *level_view = get_view();
	LevelDocument *document = level_view ? level_view->get_level_document() : nullptr;
	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	if (!document || !undo_redo) {
		return;
	}
	undo_redo->create_action_for_history(p_action_name, document->get_history_id());
	for (const MeshDragState &state : p_states) {
		if (state.topology_diff.is_valid()) {
			undo_redo->add_do_method(state.mesh.ptr(), SNAME("apply_diff"), state.topology_diff);
		}
		if (state.geometry_diff.is_valid()) {
			undo_redo->add_do_method(state.mesh.ptr(), SNAME("apply_diff"), state.geometry_diff);
		}
	}
	for (const MeshDragState &state : p_states) {
		if (state.geometry_diff.is_valid()) {
			undo_redo->add_undo_method(state.mesh.ptr(), SNAME("revert_diff"), state.geometry_diff);
		}
		if (state.topology_diff.is_valid()) {
			undo_redo->add_undo_method(state.mesh.ptr(), SNAME("revert_diff"), state.topology_diff);
		}
	}
	undo_redo->commit_action(false);
}

bool SelectTool::_commit_transform_drag() {
	if (!transform_active) {
		return false;
	}
	LevelEditorView *level_view = get_view();
	LevelDocument *document = level_view ? level_view->get_level_document() : nullptr;
	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	if (!document || !undo_redo) {
		return false;
	}
	if (_is_object_drag()) {
		bool changed = false;
		for (const ObjectDragState &state : object_drag_states) {
			changed = changed || state.preview_transform != state.original_transform;
		}
		if (!changed) {
			_cancel_transform_drag();
			return false;
		}
		const String action_name = _is_rotation_drag() ? TTR("Rotate Level Blocks") : TTR("Move Level Blocks");
		undo_redo->create_action_for_history(action_name, document->get_history_id());
		for (const ObjectDragState &state : object_drag_states) {
			undo_redo->add_do_method(state.block, SNAME("set_global_transform"), state.preview_transform);
			undo_redo->add_undo_method(state.block, SNAME("set_global_transform"), state.original_transform);
		}
		undo_redo->commit_action(false);
	} else {
		bool has_geometry = false;
		for (MeshDragState &state : mesh_drag_states) {
			state.geometry_diff = state.mesh->commit_transform_preview();
			has_geometry = has_geometry || state.geometry_diff.is_valid();
		}
		if (!has_geometry) {
			_revert_topology_diffs();
			_end_transform_drag(false);
			return false;
		}
		String action_name = _is_rotation_drag() ? TTR("Rotate Level Selection") : TTR("Move Level Selection");
		if (transform_drag_mode == TRANSFORM_DRAG_FACE_EXTRUDE) {
			action_name = TTR("Extrude Level Faces");
		} else if (transform_drag_mode == TRANSFORM_DRAG_PUSH_PULL) {
			action_name = TTR("Push/Pull Level Faces");
		} else if (transform_drag_mode == TRANSFORM_DRAG_EDGE_EXTRUDE) {
			action_name = TTR("Extrude Level Boundary Edges");
		}
		_register_mesh_undo(action_name, mesh_drag_states);

		if (transform_drag_mode == TRANSFORM_DRAG_EDGE_EXTRUDE) {
			SelectionModel *selection_model = get_selection_model();
			if (selection_model) {
				selection_model->clear();
				selection_model->set_mode_and_tier(SelectionModel::MODE_FACE, SelectionModel::TIER_POLYGROUP);
				SelectionModel::SelectionOp operation;
				operation.feature = SelectionModel::FEATURE_FACE;
				operation.tier = SelectionModel::TIER_POLYGROUP;
				operation.operation = SelectionModel::OP_REPLACE;
				for (const MeshDragState &state : mesh_drag_states) {
					for (const int64_t handle : state.created_face_handles) {
						if (state.mesh->resolve_face(handle) < 0) {
							continue;
						}
						SelectionModel::Element element;
						element.block_id = state.block->get_instance_id();
						element.handle = handle;
						element.feature = SelectionModel::FEATURE_FACE;
						element.tier = SelectionModel::TIER_POLYGROUP;
						element.handle_kind = SelectionModel::HANDLE_FACE;
						operation.elements.push_back(element);
					}
				}
				selection_model->apply(operation);
			}
		}
	}
	_end_transform_drag(true);
	if (level_view) {
		level_view->set_last_selection_action(SNAME("transform_commit"));
	}
	return true;
}

void SelectTool::_cancel_transform_drag() {
	if (_is_object_drag()) {
		for (ObjectDragState &state : object_drag_states) {
			if (state.block) {
				state.block->set_global_transform(state.original_transform);
			}
		}
	} else {
		for (MeshDragState &state : mesh_drag_states) {
			if (state.mesh.is_valid() && state.mesh->is_transform_preview_active()) {
				state.mesh->cancel_transform_preview();
			}
		}
		_revert_topology_diffs();
	}
	_end_transform_drag(false);
}

Vector3 SelectTool::_nearest_world_axis(const Vector3 &p_direction) {
	const Vector3 absolute = p_direction.abs();
	if (absolute.x >= absolute.y && absolute.x >= absolute.z) {
		return Vector3(p_direction.x < 0.0 ? -1.0 : 1.0, 0, 0);
	}
	if (absolute.y >= absolute.z) {
		return Vector3(0, p_direction.y < 0.0 ? -1.0 : 1.0, 0);
	}
	return Vector3(0, 0, p_direction.z < 0.0 ? -1.0 : 1.0);
}

bool SelectTool::_nudge(Camera3D *p_camera, Key p_key) {
	if (!p_camera || !_collect_transform_selection()) {
		return false;
	}
	const Basis camera_basis = p_camera->get_global_transform().basis;
	Vector3 direction;
	switch (p_key) {
		case Key::LEFT:
			direction = -camera_basis.get_column(0);
			break;
		case Key::RIGHT:
			direction = camera_basis.get_column(0);
			break;
		case Key::UP:
			direction = camera_basis.get_column(1);
			break;
		case Key::DOWN:
			direction = -camera_basis.get_column(1);
			break;
		case Key::PAGEUP:
			direction = -camera_basis.get_column(2);
			break;
		case Key::PAGEDOWN:
			direction = camera_basis.get_column(2);
			break;
		default:
			return false;
	}
	const real_t step = LevelEditor::snap_step_or_default();
	const Vector3 delta = _nearest_world_axis(direction) * step;
	SelectionModel *selection_model = get_selection_model();
	if (selection_model && selection_model->get_mode() == SelectionModel::MODE_OBJECT) {
		LevelEditorView *level_view = get_view();
		LevelDocument *document = level_view ? level_view->get_level_document() : nullptr;
		EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
		if (!document || !undo_redo) {
			return false;
		}
		undo_redo->create_action_for_history(TTR("Nudge Level Blocks"), document->get_history_id());
		for (ObjectDragState &state : object_drag_states) {
			state.preview_transform = state.original_transform;
			state.preview_transform.origin += delta;
			state.block->set_global_transform(state.preview_transform);
			undo_redo->add_do_method(state.block, SNAME("set_global_transform"), state.preview_transform);
			undo_redo->add_undo_method(state.block, SNAME("set_global_transform"), state.original_transform);
		}
		undo_redo->commit_action(false);
		return true;
	}
	transform_drag_mode = TRANSFORM_DRAG_MOVE;
	for (MeshDragState &state : mesh_drag_states) {
		state.capture_original_positions();
	}
	if (!_open_mesh_previews() || !_apply_mesh_preview_delta(delta)) {
		_cancel_transform_drag();
		return false;
	}
	for (MeshDragState &state : mesh_drag_states) {
		state.geometry_diff = state.mesh->commit_transform_preview();
	}
	_register_mesh_undo(TTR("Nudge Level Selection"), mesh_drag_states);
	return true;
}

bool SelectTool::_vertices_to_grid() {
	SelectionModel *selection_model = get_selection_model();
	if (!selection_model || selection_model->get_mode() == SelectionModel::MODE_OBJECT || !_collect_transform_selection()) {
		return false;
	}
	const real_t step = LevelEditor::snap_step_or_default();
	bool changed = false;
	for (MeshDragState &state : mesh_drag_states) {
		state.capture_original_positions();
		if (!state.mesh->begin_transform_preview(state.vertex_ids)) {
			_cancel_transform_drag();
			return false;
		}
		PackedVector3Array snapped;
		snapped.resize(state.vertex_ids.size());
		const Transform3D inverse = state.block->get_global_transform().affine_inverse();
		for (int i = 0; i < state.vertex_ids.size(); i++) {
			const Vector3 world = state.block->get_global_transform().xform(state.original_positions[i]);
			const Vector3 snapped_world = LevelSnapService::snap_point_absolute(world, step);
			snapped.set(i, inverse.xform(snapped_world));
			changed = changed || snapped[i] != state.original_positions[i];
		}
		if (!state.mesh->preview_transform_vertices(snapped)) {
			_cancel_transform_drag();
			return false;
		}
	}
	if (!changed) {
		_cancel_transform_drag();
		return true;
	}
	for (MeshDragState &state : mesh_drag_states) {
		state.geometry_diff = state.mesh->commit_transform_preview();
	}
	_register_mesh_undo(TTR("Vertices to Grid"), mesh_drag_states);
	return true;
}

bool SelectTool::_duplicate_objects() {
	SelectionModel *selection_model = get_selection_model();
	LevelEditorView *level_view = get_view();
	LevelDocument *document = level_view ? level_view->get_level_document() : nullptr;
	EditorSelection *selection = document ? document->get_selection() : nullptr;
	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	if (!selection_model || selection_model->get_mode() != SelectionModel::MODE_OBJECT || !document || !selection || !undo_redo) {
		return false;
	}
	Vector<LevelBlock *> duplicates;
	Vector<Node *> parents;
	Vector<Node *> owners;
	const TypedArray<Node> nodes = selection->get_selected_nodes();
	for (int i = 0; i < nodes.size(); i++) {
		LevelBlock *source = Object::cast_to<LevelBlock>(nodes[i]);
		if (!source || !source->get_parent()) {
			continue;
		}
		LevelBlock *duplicate = memnew(LevelBlock);
		duplicate->set_name(source->get_name());
		duplicate->set_transform(source->get_transform());
		duplicate->set_data(source->get_data()->duplicate_data());
		duplicates.push_back(duplicate);
		parents.push_back(source->get_parent());
		owners.push_back(source->get_owner());
	}
	if (duplicates.is_empty()) {
		return false;
	}
	undo_redo->create_action_for_history(TTR("Duplicate Level Blocks"), document->get_history_id());
	for (int i = 0; i < duplicates.size(); i++) {
		undo_redo->add_do_method(parents[i], SNAME("add_child"), duplicates[i], true);
		if (owners[i]) {
			undo_redo->add_do_method(duplicates[i], SNAME("set_owner"), owners[i]);
		}
		undo_redo->add_do_reference(duplicates[i]);
		undo_redo->add_undo_method(parents[i], SNAME("remove_child"), duplicates[i]);
	}
	undo_redo->commit_action();
	selection->clear();
	for (LevelBlock *duplicate : duplicates) {
		selection->add_node(duplicate);
	}
	selection->update();
	return true;
}
