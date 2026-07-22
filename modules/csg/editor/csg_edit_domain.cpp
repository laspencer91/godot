/**************************************************************************/
/*  csg_edit_domain.cpp                                                   */
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

#include "csg_edit_domain.h"

#include "core/input/input_event.h"
#include "core/object/callable_mp.h"
#include "editor/editor_data.h"
#include "editor/editor_document.h"
#include "editor/editor_node.h"
#include "editor/editor_undo_redo_manager.h"
#include "editor/scene/3d/node_3d_editor_plugin.h"
#include "editor/scene/3d/node_3d_editor_viewport.h"
#include "scene/3d/camera_3d.h"
#include "scene/gui/box_container.h"
#include "scene/gui/button.h"
#include "scene/gui/label.h"
#include "scene/gui/line_edit.h"
#include "scene/gui/panel_container.h"

static bool _get_box_surface_axis(uint32_t p_surface, int &r_axis, real_t &r_sign, Vector3 &r_outward) {
	r_outward = Vector3();
	switch (p_surface) {
		case CSGBox3D::SURFACE_POSITIVE_X:
			r_axis = Vector3::AXIS_X;
			r_sign = 1.0;
			break;
		case CSGBox3D::SURFACE_NEGATIVE_X:
			r_axis = Vector3::AXIS_X;
			r_sign = -1.0;
			break;
		case CSGBox3D::SURFACE_POSITIVE_Y:
			r_axis = Vector3::AXIS_Y;
			r_sign = 1.0;
			break;
		case CSGBox3D::SURFACE_NEGATIVE_Y:
			r_axis = Vector3::AXIS_Y;
			r_sign = -1.0;
			break;
		case CSGBox3D::SURFACE_POSITIVE_Z:
			r_axis = Vector3::AXIS_Z;
			r_sign = 1.0;
			break;
		case CSGBox3D::SURFACE_NEGATIVE_Z:
			r_axis = Vector3::AXIS_Z;
			r_sign = -1.0;
			break;
		default:
			return false;
	}
	r_outward[r_axis] = r_sign;
	return true;
}

CSGPushPullResult csg_push_pull_apply(const Vector3 &p_start_size, const Transform3D &p_start_transform, uint32_t p_semantic_surface, real_t p_displacement, bool p_symmetric) {
	CSGPushPullResult result;
	result.size = p_start_size;
	result.transform = p_start_transform;

	int axis = 0;
	real_t sign = 1.0;
	Vector3 outward;
	if (!_get_box_surface_axis(p_semantic_surface, axis, sign, outward)) {
		return result;
	}

	const real_t size_multiplier = p_symmetric ? 2.0 : 1.0;
	const real_t unclamped_size = p_start_size[axis] + p_displacement * size_multiplier;
	result.size[axis] = MAX(unclamped_size, (real_t)0.001);
	const real_t effective_displacement = (result.size[axis] - p_start_size[axis]) / size_multiplier;
	if (!p_symmetric) {
		result.transform.origin += p_start_transform.basis.xform(outward * (effective_displacement * 0.5));
	}
	return result;
}

static CSGShape3D *_get_single_selected_csg_shape(const EditorEditDomainContext *p_context = nullptr) {
	EditorSelection *selection = p_context && p_context->document ? p_context->document->get_selection() : nullptr;
	if (!selection) {
		EditorNode *editor_node = EditorNode::get_singleton();
		selection = editor_node ? editor_node->get_editor_selection() : nullptr;
	}
	if (!selection) {
		return nullptr;
	}
	const List<Node *> selected = selection->get_full_selected_node_list();
	if (selected.size() != 1) {
		return nullptr;
	}
	return Object::cast_to<CSGShape3D>(selected.front()->get());
}

static CSGShape3D *_find_csg_root(CSGShape3D *p_shape) {
	CSGShape3D *root = p_shape;
	while (root) {
		CSGShape3D *parent = Object::cast_to<CSGShape3D>(root->get_parent());
		if (!parent) {
			break;
		}
		root = parent;
	}
	return root;
}

void CSGSurfaceSession::_resolve_active_root(const EditorEditDomainContext &p_context) {
	CSGShape3D *selected_shape = _get_single_selected_csg_shape(&p_context);
	CSGShape3D *root = _find_csg_root(selected_shape);
	active_root_id = root ? root->get_instance_id() : ObjectID();
}

CSGShape3D *CSGSurfaceSession::_get_active_root() const {
	return ObjectDB::get_instance<CSGShape3D>(active_root_id);
}

CSGBox3D *CSGSurfaceSession::_get_active_box() const {
	return ObjectDB::get_instance<CSGBox3D>(active_box_id);
}

Node3DEditorViewport *CSGSurfaceSession::_get_active_viewport() const {
	return ObjectDB::get_instance<Node3DEditorViewport>(active_viewport_id);
}

void CSGSurfaceSession::_clear_pick_state() {
	pick_mesh.unref();
	pick_faces.clear();
	pick_mesh_generation = UINT64_MAX;
	hover_hit = CSGSurfaceHit();
	has_hover = false;
}

void CSGSurfaceSession::_clear_selection() {
	active_box_id = ObjectID();
	selected_hit = CSGSurfaceHit();
	has_selection = false;
	has_ghost = false;
	gesture_state = has_hover ? GestureState::HOVER : GestureState::IDLE;
	_update_context_panel();
}

void CSGSurfaceSession::_queue_redraw(Node3DEditorViewport *p_viewport) const {
	if (p_viewport) {
		p_viewport->update_surface();
	}
}

bool CSGSurfaceSession::_pick(Node3DEditorViewport *p_viewport, const Vector2 &p_position) {
	CSGShape3D *root = _get_active_root();
	if (!root || !p_viewport) {
		has_hover = false;
		return false;
	}

	uint64_t result_generation = root->get_result_generation();
	if (result_generation != pick_mesh_generation) {
		// Brush-face order is the result-triangle order. The render ArrayMesh is
		// deliberately not used because material grouping changes that order.
		pick_faces = root->get_brush_faces();
		result_generation = root->get_result_generation();
		pick_mesh.unref();
		if (!pick_faces.is_empty()) {
			pick_mesh.instantiate();
			pick_mesh->create(pick_faces);
		}
		pick_mesh_generation = result_generation;
	}

	if (pick_mesh.is_null() || !pick_mesh->is_valid()) {
		has_hover = false;
		return false;
	}

	const Transform3D root_inverse = root->get_global_transform().affine_inverse();
	const Vector3 ray_position = root_inverse.xform(p_viewport->get_ray_pos(p_position));
	const Vector3 ray_direction = root_inverse.basis.xform(p_viewport->get_ray(p_position)).normalized();
	Vector3 hit_position;
	Vector3 hit_normal;
	int32_t face_index = -1;
	if (!pick_mesh->intersect_ray(ray_position, ray_direction, hit_position, hit_normal, nullptr, &face_index) || face_index < 0) {
		has_hover = false;
		return false;
	}

	CSGSurfaceKey surface;
	uint32_t face_id = 0;
	if (!root->resolve_result_triangle((uint32_t)face_index, result_generation, surface, face_id)) {
		has_hover = false;
		return false;
	}

	hover_hit.surface = surface;
	hover_hit.result_generation = result_generation;
	hover_hit.face_id = face_id;
	hover_hit.triangle = (uint32_t)face_index;
	has_hover = true;
	return true;
}

static real_t _closest_parameter_on_line_to_ray(const Vector3 &p_line_origin, const Vector3 &p_line_direction, const Vector3 &p_ray_origin, const Vector3 &p_ray_direction) {
	const Vector3 offset = p_line_origin - p_ray_origin;
	const real_t line_dot_ray = p_line_direction.dot(p_ray_direction);
	const real_t line_dot_offset = p_line_direction.dot(offset);
	const real_t ray_dot_offset = p_ray_direction.dot(offset);
	const real_t denominator = 1.0 - line_dot_ray * line_dot_ray;
	if (Math::is_zero_approx(denominator)) {
		return -line_dot_offset;
	}

	real_t line_parameter = (line_dot_ray * ray_dot_offset - line_dot_offset) / denominator;
	const real_t ray_parameter = (ray_dot_offset - line_dot_ray * line_dot_offset) / denominator;
	if (ray_parameter < 0.0) {
		line_parameter = -line_dot_offset;
	}
	return line_parameter;
}

bool CSGSurfaceSession::_begin_gesture(Node3DEditorViewport *p_viewport, const Ref<InputEventMouseButton> &p_event) {
	CSGShape3D *root = _get_active_root();
	if (!root || !p_viewport || p_event.is_null()) {
		return false;
	}
	if (!has_hover || hover_hit.result_generation != root->get_result_generation()) {
		if (!_pick(p_viewport, p_event->get_position())) {
			return false;
		}
	}

	CSGBox3D *box = ObjectDB::get_instance<CSGBox3D>(hover_hit.surface.source_shape);
	Vector3 outward;
	if (!box || !_get_box_surface_axis(hover_hit.surface.semantic_surface, drag_axis, drag_axis_sign, outward)) {
		return false;
	}

	active_box_id = box->get_instance_id();
	active_viewport_id = p_viewport->get_instance_id();
	selected_hit = hover_hit;
	has_selection = true;
	gesture_state = GestureState::PRESSED;
	press_position = p_event->get_position();
	symmetric_drag = p_event->is_alt_pressed();
	start_size = box->get_size();
	start_transform = box->get_transform();
	start_global_transform = box->get_global_transform();
	start_plane_coordinate = drag_axis_sign * start_size[drag_axis] * 0.5;
	target_plane_coordinate = start_plane_coordinate;
	drag_displacement = 0.0;
	ghost_result.size = start_size;
	ghost_result.transform = start_transform;
	has_ghost = false;

	drag_line_origin_world = start_global_transform.xform(outward * (start_size[drag_axis] * 0.5));
	const Vector3 world_axis = start_global_transform.basis.xform(outward);
	drag_axis_world_scale = world_axis.length();
	if (Math::is_zero_approx(drag_axis_world_scale)) {
		_cancel_gesture();
		return false;
	}
	drag_line_direction_world = world_axis / drag_axis_world_scale;
	drag_start_parameter = _closest_parameter_on_line_to_ray(
			drag_line_origin_world,
			drag_line_direction_world,
			p_viewport->get_ray_pos(press_position),
			p_viewport->get_ray(press_position).normalized());
	_update_context_panel();
	_queue_redraw(p_viewport);
	return true;
}

void CSGSurfaceSession::_update_drag(Node3DEditorViewport *p_viewport, const Vector2 &p_position) {
	if (!p_viewport || (gesture_state != GestureState::PRESSED && gesture_state != GestureState::DRAGGING)) {
		return;
	}
	if (gesture_state == GestureState::PRESSED && press_position.distance_to(p_position) < 4.0 * EDSCALE) {
		return;
	}
	gesture_state = GestureState::DRAGGING;

	const real_t current_parameter = _closest_parameter_on_line_to_ray(
			drag_line_origin_world,
			drag_line_direction_world,
			p_viewport->get_ray_pos(p_position),
			p_viewport->get_ray(p_position).normalized());
	const real_t outward_displacement = (current_parameter - drag_start_parameter) / drag_axis_world_scale;
	target_plane_coordinate = start_plane_coordinate + drag_axis_sign * outward_displacement;
	Node3DEditor *node_3d_editor = Node3DEditor::get_singleton();
	if (node_3d_editor && node_3d_editor->is_snap_enabled()) {
		const real_t snap_step = node_3d_editor->get_translate_snap();
		if (snap_space == SnapSpace::LOCAL && snap_step > 0.0) {
			target_plane_coordinate = Math::snapped(target_plane_coordinate, snap_step);
		}
	}

	drag_displacement = (target_plane_coordinate - start_plane_coordinate) * drag_axis_sign;
	ghost_result = csg_push_pull_apply(start_size, start_transform, selected_hit.surface.semantic_surface, drag_displacement, symmetric_drag);
	const real_t multiplier = symmetric_drag ? 2.0 : 1.0;
	drag_displacement = (ghost_result.size[drag_axis] - start_size[drag_axis]) / multiplier;
	target_plane_coordinate = start_plane_coordinate + drag_axis_sign * drag_displacement;
	has_ghost = true;
	_update_context_panel();
	_queue_redraw(p_viewport);
}

void CSGSurfaceSession::_cancel_gesture() {
	gesture_state = GestureState::CANCEL;
	has_ghost = false;
	drag_displacement = 0.0;
	target_plane_coordinate = start_plane_coordinate;
	gesture_state = has_hover ? GestureState::HOVER : GestureState::IDLE;
	_update_context_panel();
	_queue_redraw(_get_active_viewport());
}

void CSGSurfaceSession::_finish_without_commit() {
	gesture_state = GestureState::COMMIT;
	has_ghost = false;
	gesture_state = has_hover ? GestureState::HOVER : GestureState::IDLE;
	_update_context_panel();
	_queue_redraw(_get_active_viewport());
}

void CSGSurfaceSession::_commit_gesture() {
	CSGBox3D *box = _get_active_box();
	CSGShape3D *root = _get_active_root();
	if (!has_ghost || !box || !root) {
		_finish_without_commit();
		return;
	}
	if (ghost_result.size.is_equal_approx(start_size) && ghost_result.transform.is_equal_approx(start_transform)) {
		_finish_without_commit();
		return;
	}

	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	if (!undo_redo) {
		_finish_without_commit();
		return;
	}

	gesture_state = GestureState::COMMIT;
	undo_redo->create_action(TTR("CSG Push/Pull Face"));
	undo_redo->add_do_property(box, SNAME("size"), ghost_result.size);
	undo_redo->add_undo_property(box, SNAME("size"), start_size);
	undo_redo->add_do_property(box, SNAME("transform"), ghost_result.transform);
	undo_redo->add_undo_property(box, SNAME("transform"), start_transform);
	// Property setters queue the ordinary deferred update first. This final
	// request snapshots both properties and supersedes that queued sync update.
	undo_redo->add_do_method(root, SNAME("_request_final_async_evaluation"));
	undo_redo->add_undo_method(root, SNAME("_request_final_async_evaluation"));
	undo_redo->commit_action();

	has_ghost = false;
	has_hover = false;
	gesture_state = GestureState::IDLE;
	drag_displacement = 0.0;
	start_size = box->get_size();
	start_transform = box->get_transform();
	start_plane_coordinate = drag_axis_sign * start_size[drag_axis] * 0.5;
	target_plane_coordinate = start_plane_coordinate;
	_update_context_panel();
	_queue_redraw(_get_active_viewport());
}

void CSGSurfaceSession::_update_context_panel() {
	Label *distance_label = ObjectDB::get_instance<Label>(distance_label_id);
	LineEdit *coordinate_edit = ObjectDB::get_instance<LineEdit>(coordinate_edit_id);
	if (!has_selection) {
		if (distance_label) {
			distance_label->set_text(TTR("Select a box face"));
		}
		if (coordinate_edit && !coordinate_edit->has_focus()) {
			coordinate_edit->clear();
		}
		return;
	}

	if (distance_label) {
		distance_label->set_text(vformat(TTR("Distance: %s m"), String::num(drag_displacement, 4)));
	}
	if (coordinate_edit && !coordinate_edit->has_focus()) {
		coordinate_edit->set_text(String::num(target_plane_coordinate, 4));
	}
}

void CSGSurfaceSession::_numeric_coordinate_submitted(const String &p_text) {
	if (!has_selection || !p_text.is_valid_float()) {
		return;
	}
	CSGBox3D *box = _get_active_box();
	if (!box) {
		_clear_selection();
		return;
	}

	Vector3 outward;
	if (gesture_state != GestureState::PRESSED && gesture_state != GestureState::DRAGGING) {
		if (!_get_box_surface_axis(selected_hit.surface.semantic_surface, drag_axis, drag_axis_sign, outward)) {
			return;
		}
		start_size = box->get_size();
		start_transform = box->get_transform();
		start_global_transform = box->get_global_transform();
		start_plane_coordinate = drag_axis_sign * start_size[drag_axis] * 0.5;
		symmetric_drag = false;
	}

	target_plane_coordinate = p_text.to_float();
	drag_displacement = (target_plane_coordinate - start_plane_coordinate) * drag_axis_sign;
	ghost_result = csg_push_pull_apply(start_size, start_transform, selected_hit.surface.semantic_surface, drag_displacement, symmetric_drag);
	const real_t multiplier = symmetric_drag ? 2.0 : 1.0;
	drag_displacement = (ghost_result.size[drag_axis] - start_size[drag_axis]) / multiplier;
	target_plane_coordinate = start_plane_coordinate + drag_axis_sign * drag_displacement;
	gesture_state = GestureState::DRAGGING;
	has_ghost = true;
	_update_context_panel();
	_queue_redraw(_get_active_viewport());
	_commit_gesture();
}

static void _get_box_face_corners(const Vector3 &p_size, uint32_t p_surface, Vector3 r_corners[4]) {
	const Vector3 half_size = p_size * 0.5;
	switch (p_surface) {
		case CSGBox3D::SURFACE_POSITIVE_X:
			r_corners[0] = Vector3(half_size.x, -half_size.y, -half_size.z);
			r_corners[1] = Vector3(half_size.x, half_size.y, -half_size.z);
			r_corners[2] = Vector3(half_size.x, half_size.y, half_size.z);
			r_corners[3] = Vector3(half_size.x, -half_size.y, half_size.z);
			break;
		case CSGBox3D::SURFACE_NEGATIVE_X:
			r_corners[0] = Vector3(-half_size.x, -half_size.y, half_size.z);
			r_corners[1] = Vector3(-half_size.x, half_size.y, half_size.z);
			r_corners[2] = Vector3(-half_size.x, half_size.y, -half_size.z);
			r_corners[3] = Vector3(-half_size.x, -half_size.y, -half_size.z);
			break;
		case CSGBox3D::SURFACE_POSITIVE_Y:
			r_corners[0] = Vector3(-half_size.x, half_size.y, half_size.z);
			r_corners[1] = Vector3(half_size.x, half_size.y, half_size.z);
			r_corners[2] = Vector3(half_size.x, half_size.y, -half_size.z);
			r_corners[3] = Vector3(-half_size.x, half_size.y, -half_size.z);
			break;
		case CSGBox3D::SURFACE_NEGATIVE_Y:
			r_corners[0] = Vector3(-half_size.x, -half_size.y, -half_size.z);
			r_corners[1] = Vector3(half_size.x, -half_size.y, -half_size.z);
			r_corners[2] = Vector3(half_size.x, -half_size.y, half_size.z);
			r_corners[3] = Vector3(-half_size.x, -half_size.y, half_size.z);
			break;
		case CSGBox3D::SURFACE_POSITIVE_Z:
			r_corners[0] = Vector3(-half_size.x, -half_size.y, half_size.z);
			r_corners[1] = Vector3(half_size.x, -half_size.y, half_size.z);
			r_corners[2] = Vector3(half_size.x, half_size.y, half_size.z);
			r_corners[3] = Vector3(-half_size.x, half_size.y, half_size.z);
			break;
		case CSGBox3D::SURFACE_NEGATIVE_Z:
			r_corners[0] = Vector3(-half_size.x, half_size.y, -half_size.z);
			r_corners[1] = Vector3(half_size.x, half_size.y, -half_size.z);
			r_corners[2] = Vector3(half_size.x, -half_size.y, -half_size.z);
			r_corners[3] = Vector3(-half_size.x, -half_size.y, -half_size.z);
			break;
		default:
			for (int i = 0; i < 4; i++) {
				r_corners[i] = Vector3();
			}
			break;
	}
}

static Transform3D _local_to_world_transform(Node3D *p_node, const Transform3D &p_local_transform) {
	Node3D *parent_3d = Object::cast_to<Node3D>(p_node->get_parent());
	return parent_3d ? parent_3d->get_global_transform() * p_local_transform : p_local_transform;
}

void CSGSurfaceSession::_draw_ghost(Node3DEditorViewport *p_viewport) const {
	if (!has_selection || !p_viewport) {
		return;
	}
	CSGBox3D *box = _get_active_box();
	if (!box) {
		return;
	}
	Camera3D *camera = p_viewport->get_previewing_camera();
	if (!camera) {
		camera = p_viewport->get_camera_3d();
	}
	Control *surface_control = p_viewport->get_surface();
	if (!camera || !surface_control) {
		return;
	}

	const Vector3 target_size = has_ghost ? ghost_result.size : box->get_size();
	const Transform3D target_global = has_ghost ? _local_to_world_transform(box, ghost_result.transform) : box->get_global_transform();
	Vector3 face_corners[4];
	_get_box_face_corners(target_size, selected_hit.surface.semantic_surface, face_corners);
	Vector<Point2> face_polygon;
	face_polygon.resize(4);
	Vector3 face_center;
	for (int i = 0; i < 4; i++) {
		const Vector3 world_corner = target_global.xform(face_corners[i]);
		if (camera->is_position_behind(world_corner)) {
			return;
		}
		face_center += world_corner;
		face_polygon.write[i] = camera->unproject_position(world_corner);
	}
	face_center /= 4.0;
	surface_control->draw_colored_polygon(face_polygon, Color(1.0, 0.65, 0.15, has_ghost ? 0.22 : 0.12));
	face_polygon.push_back(face_polygon[0]);
	surface_control->draw_polyline(face_polygon, Color(1.0, 0.72, 0.2), 2.0 * EDSCALE, true);

	int axis = 0;
	real_t sign = 1.0;
	Vector3 outward;
	if (_get_box_surface_axis(selected_hit.surface.semantic_surface, axis, sign, outward)) {
		const Vector3 world_normal = target_global.basis.xform(outward).normalized();
		const real_t handle_length = MAX(target_global.basis.xform(outward * target_size[axis] * 0.25).length(), (real_t)0.25);
		const Vector3 handle_end = face_center + world_normal * handle_length;
		if (!camera->is_position_behind(handle_end)) {
			surface_control->draw_line(camera->unproject_position(face_center), camera->unproject_position(handle_end), Color(1.0, 0.78, 0.25), 2.0 * EDSCALE, true);
		}
	}

	if (has_ghost) {
		Vector3 corners[8];
		for (int corner_i = 0; corner_i < 8; corner_i++) {
			const Vector3 local_corner(
					(corner_i & 1) ? target_size.x * 0.5 : -target_size.x * 0.5,
					(corner_i & 2) ? target_size.y * 0.5 : -target_size.y * 0.5,
					(corner_i & 4) ? target_size.z * 0.5 : -target_size.z * 0.5);
			corners[corner_i] = target_global.xform(local_corner);
		}
		static constexpr int edge_indices[12][2] = {
			{ 0, 1 },
			{ 2, 3 },
			{ 4, 5 },
			{ 6, 7 },
			{ 0, 2 },
			{ 1, 3 },
			{ 4, 6 },
			{ 5, 7 },
			{ 0, 4 },
			{ 1, 5 },
			{ 2, 6 },
			{ 3, 7 },
		};
		for (const int *edge : edge_indices) {
			if (camera->is_position_behind(corners[edge[0]]) || camera->is_position_behind(corners[edge[1]])) {
				continue;
			}
			surface_control->draw_line(camera->unproject_position(corners[edge[0]]), camera->unproject_position(corners[edge[1]]), Color(0.4, 0.9, 1.0, 0.95), 1.5 * EDSCALE, true);
		}

		const Point2 label_position = camera->unproject_position(face_center) + Point2(10, -10) * EDSCALE;
		surface_control->draw_string(
				surface_control->get_theme_default_font(),
				label_position,
				vformat(TTR("Plane %s m"), String::num(target_plane_coordinate, 4)),
				HORIZONTAL_ALIGNMENT_LEFT,
				-1,
				surface_control->get_theme_default_font_size(),
				Color(0.9, 0.98, 1.0));
	}
}

void CSGSurfaceSession::_draw_hover(Node3DEditorViewport *p_viewport) const {
	if (!has_hover || !p_viewport) {
		return;
	}
	CSGShape3D *root = _get_active_root();
	if (!root) {
		return;
	}

	Camera3D *camera = p_viewport->get_previewing_camera();
	if (!camera) {
		camera = p_viewport->get_camera_3d();
	}
	Control *surface_control = p_viewport->get_surface();
	if (!camera || !surface_control) {
		return;
	}

	Vector<Vector3> world_corners;
	if (CSGBox3D *box = ObjectDB::get_instance<CSGBox3D>(hover_hit.surface.source_shape)) {
		Vector3 local_corners[4];
		_get_box_face_corners(box->get_size(), hover_hit.surface.semantic_surface, local_corners);
		world_corners.resize(4);
		for (int i = 0; i < 4; i++) {
			world_corners.write[i] = box->get_global_transform().xform(local_corners[i]);
		}
	} else {
		const uint32_t vertex_begin = hover_hit.triangle * 3;
		if (vertex_begin + 2 >= (uint32_t)pick_faces.size()) {
			return;
		}
		world_corners.resize(3);
		for (int i = 0; i < 3; i++) {
			world_corners.write[i] = root->get_global_transform().xform(pick_faces[vertex_begin + i]);
		}
	}

	Vector<Point2> polygon;
	polygon.resize(world_corners.size());
	for (int i = 0; i < world_corners.size(); i++) {
		if (camera->is_position_behind(world_corners[i])) {
			return;
		}
		polygon.write[i] = camera->unproject_position(world_corners[i]);
	}
	surface_control->draw_colored_polygon(polygon, Color(0.2, 0.7, 1.0, 0.24));
	polygon.push_back(polygon[0]);
	surface_control->draw_polyline(polygon, Color(0.35, 0.85, 1.0), 2.0 * EDSCALE, true);
}

void CSGSurfaceSession::enter(const EditorEditDomainContext &p_context) {
	entered = true;
	active_viewport_id = p_context.active_viewport ? p_context.active_viewport->get_instance_id() : ObjectID();
	_resolve_active_root(p_context);
	if (p_context.active_viewport) {
		p_context.active_viewport->update_surface();
	}
}

void CSGSurfaceSession::exit() {
	_cancel_gesture();
	entered = false;
	active_root_id = ObjectID();
	active_viewport_id = ObjectID();
	_clear_pick_state();
	_clear_selection();
}

void CSGSurfaceSession::retarget(const EditorEditDomainContext &p_context) {
	_cancel_gesture();
	_clear_pick_state();
	_clear_selection();
	active_viewport_id = p_context.active_viewport ? p_context.active_viewport->get_instance_id() : ObjectID();
	_resolve_active_root(p_context);
	if (p_context.active_viewport) {
		p_context.active_viewport->update_surface();
	}
}

EditorEditDomainInput CSGSurfaceSession::handle_input(const EditorEditDomainContext &p_context, Camera3D *p_camera, const Ref<InputEvent> &p_event) {
	if (!entered || !p_context.active_viewport) {
		return EditorEditDomainInput::PASS_TO_VIEWPORT;
	}
	CSGShape3D *selected_root = _find_csg_root(_get_single_selected_csg_shape(&p_context));
	const ObjectID selected_root_id = selected_root ? selected_root->get_instance_id() : ObjectID();
	if (selected_root_id != active_root_id) {
		retarget(p_context);
	}
	if (!_get_active_root()) {
		return EditorEditDomainInput::PASS_TO_VIEWPORT;
	}
	active_viewport_id = p_context.active_viewport->get_instance_id();
	if (!surface_tool_active) {
		return EditorEditDomainInput::PASS_TO_VIEWPORT;
	}

	Ref<InputEventKey> key_event = p_event;
	if (key_event.is_valid() && key_event->is_pressed() && !key_event->is_echo() && (key_event->get_keycode() == Key::ENTER || key_event->get_keycode() == Key::KP_ENTER)) {
		if (gesture_state == GestureState::DRAGGING) {
			_commit_gesture();
			return EditorEditDomainInput::CONSUMED;
		}
		if (gesture_state == GestureState::PRESSED) {
			_finish_without_commit();
			return EditorEditDomainInput::CONSUMED;
		}
	}

	Ref<View3DController> controller = p_context.active_viewport->get_controller();
	if (controller.is_valid() && (controller->is_navigating() || controller->cursor.region_select)) {
		return EditorEditDomainInput::PASS_TO_VIEWPORT;
	}

	Ref<InputEventMouseButton> mouse_button = p_event;
	if (mouse_button.is_valid()) {
		const MouseButton button = mouse_button->get_button_index();
		if (button == MouseButton::MIDDLE || button == MouseButton::RIGHT || button == MouseButton::WHEEL_UP || button == MouseButton::WHEEL_DOWN || button == MouseButton::WHEEL_LEFT || button == MouseButton::WHEEL_RIGHT) {
			return EditorEditDomainInput::PASS_TO_VIEWPORT;
		}
		if (button == MouseButton::LEFT) {
			if (mouse_button->is_pressed()) {
				return _begin_gesture(p_context.active_viewport, mouse_button) ? EditorEditDomainInput::CONSUMED : EditorEditDomainInput::PASS_TO_VIEWPORT;
			}
			if (gesture_state == GestureState::PRESSED) {
				_finish_without_commit();
				return EditorEditDomainInput::CONSUMED;
			}
			if (gesture_state == GestureState::DRAGGING) {
				_commit_gesture();
				return EditorEditDomainInput::CONSUMED;
			}
		}
	}

	Ref<InputEventMouseMotion> mouse_motion = p_event;
	if (mouse_motion.is_valid() && mouse_motion->get_button_mask().has_flag(MouseButtonMask::LEFT) && (gesture_state == GestureState::PRESSED || gesture_state == GestureState::DRAGGING)) {
		_update_drag(p_context.active_viewport, mouse_motion->get_position());
		return EditorEditDomainInput::CONSUMED;
	}
	if (mouse_motion.is_valid() && !mouse_motion->get_button_mask().has_flag(MouseButtonMask::LEFT) && !mouse_motion->get_button_mask().has_flag(MouseButtonMask::MIDDLE) && !mouse_motion->get_button_mask().has_flag(MouseButtonMask::RIGHT)) {
		const bool had_hover = has_hover;
		const CSGSurfaceHit previous_hit = hover_hit;
		const bool picked = _pick(p_context.active_viewport, mouse_motion->get_position());
		if (had_hover != has_hover || (has_hover && (previous_hit.triangle != hover_hit.triangle || previous_hit.result_generation != hover_hit.result_generation))) {
			_queue_redraw(p_context.active_viewport);
		}
		gesture_state = has_hover ? GestureState::HOVER : GestureState::IDLE;
		return picked ? EditorEditDomainInput::BLOCK_NATIVE_EDIT : EditorEditDomainInput::PASS_TO_VIEWPORT;
	}
	return EditorEditDomainInput::PASS_TO_VIEWPORT;
}

bool CSGSurfaceSession::handle_escape() {
	if (gesture_state != GestureState::PRESSED && gesture_state != GestureState::DRAGGING) {
		return false;
	}
	_cancel_gesture();
	return true;
}

bool CSGSurfaceSession::handle_tool_toggle() {
	if (gesture_state == GestureState::PRESSED || gesture_state == GestureState::DRAGGING) {
		_cancel_gesture();
	}
	surface_tool_active = !surface_tool_active;
	if (!surface_tool_active) {
		has_hover = false;
	}
	_queue_redraw(_get_active_viewport());
	return true;
}

void CSGSurfaceSession::draw_overlay(Node3DEditorViewport *p_viewport) {
	if (entered && surface_tool_active) {
		_draw_hover(p_viewport);
		_draw_ghost(p_viewport);
	}
}

Control *CSGSurfaceSession::build_tool_rail() {
	VBoxContainer *rail = memnew(VBoxContainer);
	rail->set_name("CSGSurfaceToolRail");
	Button *surface_button = memnew(Button);
	surface_button->set_text(TTR("Surface"));
	surface_button->set_toggle_mode(true);
	surface_button->set_pressed(true);
	rail->add_child(surface_button);
	for (const String &tool_name : { TTR("Draw"), TTR("Paint"), TTR("Operand") }) {
		Button *button = memnew(Button);
		button->set_text(tool_name);
		button->set_disabled(true);
		rail->add_child(button);
	}
	return rail;
}

Control *CSGSurfaceSession::build_contextual_panel() {
	PanelContainer *panel = memnew(PanelContainer);
	panel->set_name("CSGSurfaceContextPanel");
	VBoxContainer *contents = memnew(VBoxContainer);
	panel->add_child(contents);
	Label *distance_label = memnew(Label);
	distance_label->set_text(TTR("Select a box face"));
	distance_label_id = distance_label->get_instance_id();
	contents->add_child(distance_label);
	HBoxContainer *coordinate_row = memnew(HBoxContainer);
	contents->add_child(coordinate_row);
	Label *coordinate_label = memnew(Label);
	coordinate_label->set_text(TTR("Plane"));
	coordinate_row->add_child(coordinate_label);
	LineEdit *coordinate_edit = memnew(LineEdit);
	coordinate_edit->set_placeholder(TTR("Coordinate"));
	coordinate_edit->set_custom_minimum_size(Size2(96, 0) * EDSCALE);
	coordinate_edit->connect(SceneStringName(text_submitted), callable_mp(this, &CSGSurfaceSession::_numeric_coordinate_submitted));
	coordinate_edit_id = coordinate_edit->get_instance_id();
	coordinate_row->add_child(coordinate_edit);
	_update_context_panel();
	return panel;
}

StringName CSGEditDomainProvider::get_domain_id() const {
	return SNAME("csg_surface");
}

bool CSGEditDomainProvider::is_available(const EditorEditDomainContext &p_context) const {
	return _get_single_selected_csg_shape(&p_context) != nullptr;
}

bool CSGEditDomainProvider::can_activate_from_double_click(const EditorEditDomainContext &p_context, ObjectID p_hit) const {
	return ObjectDB::get_instance<CSGShape3D>(p_hit) != nullptr;
}

EditorEditDomainSession *CSGEditDomainProvider::create_session(const EditorEditDomainContext &p_context) const {
	return memnew(CSGSurfaceSession);
}
