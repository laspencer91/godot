/**************************************************************************/
/*  block_tool.cpp                                                        */
/**************************************************************************/

#include "block_tool.h"

#include "editor/editor_document.h"
#include "editor/editor_undo_redo_manager.h"
#include "editor/level/level_editor.h"
#include "editor/level/level_editor_view.h"
#include "editor/level/level_snap_service.h"
#include "scene/3d/camera_3d.h"

#include "modules/level_kernel/level_block.h"
#include "modules/level_kernel/level_mesh.h"
#include "modules/level_kernel/level_mesh_diff.h"

bool BlockTool::_begin_pending(Camera3D *p_camera, const Ref<InputEventMouse> &p_event) {
	ERR_FAIL_NULL_V(p_camera, false);
	ERR_FAIL_COND_V(p_event.is_null(), false);

	// LE0 deliberately freezes the authored ground plane and its canonical X/Z
	// tangent frame at gesture start. Surface snap and camera-facing Ctrl flip are
	// scoped follow-ups; neither is allowed to perturb an in-flight drag.
	drag_plane_origin = Vector3();
	drag_plane = Plane(Vector3::UP, drag_plane_origin);
	drag_basis = Basis();
	gesture_snap_step = LevelEditor::snap_step_or_default();
	default_block_height = LevelEditor::get_singleton() ? LevelEditor::get_singleton()->get_default_block_height() : LevelEditor::DEFAULT_BLOCK_HEIGHT;

	Vector3 intersection;
	if (!_intersect_drag_plane(p_camera, p_event->get_position(), intersection)) {
		return false;
	}

	press_position = p_event->get_position();
	p0 = LevelSnapService::snap_point_to_plane_grid(intersection, Transform3D(drag_basis, drag_plane_origin), gesture_snap_step);
	p1 = p0;
	p2 = p0 + drag_plane.normal * default_block_height;
	state = STATE_PENDING;
	return true;
}

bool BlockTool::_intersect_drag_plane(Camera3D *p_camera, const Vector2 &p_screen_position, Vector3 &r_point) const {
	const Vector3 ray_origin = p_camera->project_ray_origin(p_screen_position);
	const Vector3 ray_direction = p_camera->project_ray_normal(p_screen_position);
	return drag_plane.intersects_ray(ray_origin, ray_direction, &r_point);
}

bool BlockTool::_update_base(Camera3D *p_camera, const Vector2 &p_screen_position) {
	Vector3 intersection;
	if (!_intersect_drag_plane(p_camera, p_screen_position, intersection)) {
		return false;
	}
	p1 = LevelSnapService::snap_point_to_plane_grid(intersection, Transform3D(drag_basis, drag_plane_origin), gesture_snap_step);
	p2 = p1 + drag_plane.normal * default_block_height;
	_update_preview();
	return true;
}

void BlockTool::_update_height(Camera3D *p_camera, const Vector2 &p_screen_position) {
	const Vector3 ray_origin = p_camera->project_ray_origin(p_screen_position);
	const Vector3 ray_direction = p_camera->project_ray_normal(p_screen_position).normalized();
	const Vector3 height_axis = drag_plane.normal.normalized();
	const real_t parallel = Math::abs(ray_direction.dot(height_axis));

	if (parallel >= Math::cos(DRAG_ANGLE_LIMIT)) {
		p2 = p1 + height_axis * default_block_height;
		_update_preview();
		return;
	}

	// Closest point between the mouse ray's infinite line and the frozen normal
	// through p1. The angle guard above keeps the denominator well-conditioned.
	const Vector3 w0 = ray_origin - p1;
	const real_t ray_dot_axis = ray_direction.dot(height_axis);
	const real_t ray_dot_w0 = ray_direction.dot(w0);
	const real_t axis_dot_w0 = height_axis.dot(w0);
	const real_t denominator = 1.0 - ray_dot_axis * ray_dot_axis;
	if (denominator <= CMP_EPSILON) {
		p2 = p1 + height_axis * default_block_height;
	} else {
		const real_t height = LevelSnapService::snap_delta((axis_dot_w0 - ray_dot_axis * ray_dot_w0) / denominator, gesture_snap_step);
		p2 = p1 + height_axis * height;
	}
	_update_preview();
}

bool BlockTool::_has_base_area() const {
	const Vector3 local_delta = drag_basis.transposed().xform(p1 - p0);
	return Math::abs(local_delta.x) > CMP_EPSILON && Math::abs(local_delta.z) > CMP_EPSILON;
}

bool BlockTool::_get_box_spec(Transform3D &r_frame, Vector3 &r_size) const {
	const Vector3 local_base = drag_basis.transposed().xform(p1 - p0);
	const real_t height = (p2 - p1).dot(drag_plane.normal);
	r_size = Vector3(Math::abs(local_base.x), Math::abs(height), Math::abs(local_base.z));

	// AABB::has_volume is the tool-layer guard required by the S1 contract. It
	// operates in the frozen orthonormal frame, so a sloped plane cannot turn a
	// zero local extent into a non-zero world-axis AABB by rotation.
	const AABB local_bounds(-r_size * (real_t)0.5, r_size);
	if (!local_bounds.has_volume()) {
		return false;
	}

	r_frame.basis = drag_basis;
	r_frame.origin = p0 + drag_basis.get_column(0) * (local_base.x * (real_t)0.5) +
			drag_basis.get_column(2) * (local_base.z * (real_t)0.5) +
			drag_plane.normal * (height * (real_t)0.5);
	return r_frame.is_finite() && r_size.is_finite();
}

void BlockTool::_update_preview() {
	Transform3D frame;
	Vector3 size;
	if (_get_box_spec(frame, size)) {
		get_overlay().update_box(frame, size);
	}
}

bool BlockTool::_handle_input(Camera3D *p_camera, const Ref<InputEvent> &p_event) {
	Ref<InputEventMouseButton> button = p_event;
	if (button.is_valid() && button->get_button_index() == MouseButton::LEFT && !button->is_alt_pressed()) {
		if (button->is_pressed()) {
			if (state == STATE_IDLE || state == STATE_PENDING) {
				return _begin_pending(p_camera, button);
			}
			// Height selection follows the cursor without a held button; the next
			// click is accepted on release, matching every other modal tool.
			return state == STATE_HEIGHT_DRAG;
		}

		switch (state) {
			case STATE_PENDING: {
				exit_gesture(); // A bare click never creates or enters undo history.
				return true;
			}
			case STATE_BASE_DRAG: {
				_update_base(p_camera, button->get_position());
				if (!_has_base_area()) {
					// Snap collapse re-arms from the release point. Cleanup still goes
					// through the one exit path before a fresh PENDING gesture is armed.
					exit_gesture();
					_begin_pending(p_camera, button);
					return true;
				}

				const Vector3 release_ray = p_camera->project_ray_normal(button->get_position()).normalized();
				if (Math::abs(release_ray.dot(drag_plane.normal)) >= Math::cos(DRAG_ANGLE_LIMIT)) {
					p2 = p1 + drag_plane.normal * default_block_height;
					commit_gesture();
				} else {
					state = STATE_HEIGHT_DRAG;
					p2 = p1 + drag_plane.normal * default_block_height;
					_update_preview();
				}
				return true;
			}
			case STATE_HEIGHT_DRAG: {
				_update_height(p_camera, button->get_position());
				commit_gesture();
				return true;
			}
			default:
				return false;
		}
	}

	Ref<InputEventMouseMotion> motion = p_event;
	if (motion.is_null()) {
		return false;
	}

	if (state == STATE_PENDING && motion->get_button_mask().has_flag(MouseButtonMask::LEFT)) {
		if (!drag_started(press_position, motion->get_position())) {
			return true;
		}
		state = STATE_BASE_DRAG;
		_update_base(p_camera, motion->get_position());
		return true;
	}
	if (state == STATE_BASE_DRAG && motion->get_button_mask().has_flag(MouseButtonMask::LEFT)) {
		_update_base(p_camera, motion->get_position());
		return true;
	}
	if (state == STATE_HEIGHT_DRAG) {
		_update_height(p_camera, motion->get_position());
		return true;
	}
	return false;
}

bool BlockTool::_commit_gesture() {
	if (state == STATE_BASE_DRAG) {
		p2 = p1 + drag_plane.normal * default_block_height;
	}
	if (state != STATE_BASE_DRAG && state != STATE_HEIGHT_DRAG) {
		return false;
	}

	Transform3D frame;
	Vector3 size;
	if (!_get_box_spec(frame, size)) {
		return false;
	}

	LevelEditorView *level_view = get_view();
	LevelDocument *document = level_view ? level_view->get_level_document() : nullptr;
	Node *scene_root = document ? document->get_root() : nullptr;
	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	if (!document || !scene_root || !undo_redo) {
		return false;
	}

	Ref<LevelMesh> level_mesh;
	level_mesh.instantiate();
	level_mesh->begin_transaction();
	if (!level_mesh->create_box(frame, size, 0)) {
		level_mesh->rollback();
		return false;
	}
	Ref<LevelMeshDiff> diff = level_mesh->commit();
	if (diff.is_null()) {
		return false;
	}

	LevelBlock *block = memnew(LevelBlock);
	block->set_name("Block");
	block->set_data(level_mesh->get_data());

	state = STATE_COMMIT;
	undo_redo->create_action_for_history(TTR("Add Block"), document->get_history_id());
	undo_redo->add_do_method(scene_root, SNAME("add_child"), block, true);
	undo_redo->add_do_method(block, SNAME("set_owner"), scene_root);
	undo_redo->add_do_reference(block);
	undo_redo->add_undo_method(scene_root, SNAME("remove_child"), block);
	undo_redo->commit_action();
	return true;
}

void BlockTool::_reset_gesture() {
	state = STATE_IDLE;
	drag_plane = Plane();
	drag_plane_origin = Vector3();
	drag_basis = Basis();
	p0 = Vector3();
	p1 = Vector3();
	p2 = Vector3();
	press_position = Vector2();
	gesture_snap_step = LevelEditor::DEFAULT_SNAP_STEP;
	default_block_height = LevelEditor::DEFAULT_BLOCK_HEIGHT;
}
