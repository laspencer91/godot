/**************************************************************************/
/*  block_tool.cpp                                                        */
/**************************************************************************/

#include "block_tool.h"

#include "core/templates/hash_set.h"
#include "editor/editor_data.h"
#include "editor/editor_document.h"
#include "editor/editor_undo_redo_manager.h"
#include "editor/level/level_editor.h"
#include "editor/level/level_editor_view.h"
#include "editor/level/level_snap_service.h"
#include "editor/scene/3d/node_3d_editor_plugin.h"
#include "scene/3d/camera_3d.h"
#include "scene/3d/node_3d.h"

#include "modules/level_kernel/level_block.h"
#include "modules/level_kernel/level_mesh.h"
#include "modules/level_kernel/level_mesh_diff.h"

Vector3 BlockTool::_nearest_world_axis(const Vector3 &p_normal) {
	const Vector3 absolute = p_normal.abs();
	// Exact ties prefer X, then Y, then Z. Besides making 45-degree slopes
	// deterministic, this matches the dominant-axis convention used elsewhere in
	// the level editor.
	if (absolute.x >= absolute.y && absolute.x >= absolute.z) {
		return Vector3(p_normal.x < 0.0 ? -1.0 : 1.0, 0.0, 0.0);
	}
	if (absolute.y >= absolute.z) {
		return Vector3(0.0, p_normal.y < 0.0 ? -1.0 : 1.0, 0.0);
	}
	return Vector3(0.0, 0.0, p_normal.z < 0.0 ? -1.0 : 1.0);
}

Basis BlockTool::_axis_tangent_basis(const Vector3 &p_axis_normal) {
	// Local Y is the extrusion normal; local X/Z are stable plane tangents drawn
	// from the level kernel's grid UV frame, so surface snapping and textures share
	// one tangent convention. p_axis_normal is a signed unit axis, so the resulting
	// basis is orthonormal with determinant +1.
	const Vector3 tangent = LevelMesh::grid_uv_tangent_for_normal(p_axis_normal);
	return Basis(tangent, p_axis_normal, tangent.cross(p_axis_normal));
}

bool BlockTool::_query_surface_hit(Camera3D *p_camera, const Vector2 &p_screen_position, Vector3 &r_position, Vector3 &r_normal) const {
	LevelEditorView *level_view = get_view();
	LevelDocument *document = level_view ? level_view->get_level_document() : nullptr;
	Node3DEditor *node_3d_editor = Node3DEditor::get_singleton();
	if (!p_camera || !document || !document->get_root() || !node_3d_editor) {
		return false;
	}

	// This is deliberately the Select tool's exact cached picking path: the
	// pane/world broad-phase gizmo BVH, then each LevelBlock's element BVH.
	const Vector3 ray_origin = p_camera->project_ray_origin(p_screen_position);
	const Vector3 ray_direction = p_camera->project_ray_normal(p_screen_position).normalized();
	const Vector<Node3D *> broad_candidates = node_3d_editor->gizmo_bvh_ray_query(
			ray_origin, ray_origin + ray_direction * p_camera->get_far(), document->get_world_3d());
	HashSet<ObjectID> visited;
	bool found = false;
	real_t best_t = Math::INF;
	uint64_t best_block_id = 0;
	int best_face_id = -1;
	int best_triangle_id = -1;
	for (Node3D *candidate : broad_candidates) {
		LevelBlock *block = Object::cast_to<LevelBlock>(candidate);
		if (!block || visited.has(block->get_instance_id()) || !document->get_root()->is_ancestor_of(block) || block->get_data().is_null()) {
			continue;
		}
		visited.insert(block->get_instance_id());

		const Transform3D block_transform = block->get_global_transform();
		if (!block_transform.is_finite() || Math::abs(block_transform.basis.determinant()) <= CMP_EPSILON) {
			continue;
		}
		const Transform3D inverse = block_transform.affine_inverse();
		const Ref<LevelMesh> mesh = block->get_level_mesh();
		if (mesh.is_null()) {
			continue;
		}
		const Dictionary result = mesh->ray_closest(inverse.xform(ray_origin), inverse.basis.xform(ray_direction));
		if (!(bool)result.get("hit", false)) {
			continue;
		}

		const real_t t = result.get("t", Math::INF);
		const int face_id = result.get("face_id", -1);
		const int triangle_id = result.get("tri_id", -1);
		if (t < 0.0 || t > p_camera->get_far() || face_id < 0) {
			continue;
		}
		const Vector3 local_normal = mesh->get_face_normal(face_id);
		const Vector3 world_normal = block_transform.basis.inverse().transposed().xform(local_normal).normalized();
		const Vector3 world_position = ray_origin + ray_direction * t;
		if (!world_normal.is_finite() || world_normal.length_squared() <= CMP_EPSILON2 || !world_position.is_finite()) {
			continue;
		}

		const uint64_t block_id = (uint64_t)block->get_instance_id();
		bool closer = !found;
		if (found) {
			if (!Math::is_equal_approx(t, best_t)) {
				closer = t < best_t;
			} else if (block_id != best_block_id) {
				closer = block_id < best_block_id;
			} else if (face_id != best_face_id) {
				closer = face_id < best_face_id;
			} else {
				closer = triangle_id < best_triangle_id;
			}
		}
		if (!closer) {
			continue;
		}

		found = true;
		best_t = t;
		best_block_id = block_id;
		best_face_id = face_id;
		best_triangle_id = triangle_id;
		r_position = world_position;
		r_normal = world_normal;
	}
	return found;
}

bool BlockTool::_resolve_start(Camera3D *p_camera, const Vector2 &p_screen_position, Plane &r_plane,
		Vector3 &r_plane_origin, Basis &r_basis, Vector3 &r_point, bool &r_surface_hit) const {
	r_surface_hit = false;
	Vector3 surface_position;
	Vector3 surface_normal;
	if (_query_surface_hit(p_camera, p_screen_position, surface_position, surface_normal)) {
		const Vector3 axis_normal = _nearest_world_axis(surface_normal);
		// Keep the hit face's normal-axis coordinate, but anchor both tangents at
		// world zero. Snapping is then continuous with the floor/world grid instead
		// of starting a new arbitrary grid at every click point on existing geometry.
		r_plane_origin = axis_normal * axis_normal.dot(surface_position);
		r_plane = Plane(axis_normal, r_plane_origin);
		r_basis = _axis_tangent_basis(axis_normal);
		r_point = surface_position;
		r_surface_hit = true;
		return true;
	}

	r_plane_origin = Vector3();
	r_plane = Plane(Vector3::UP, r_plane_origin);
	r_basis = Basis();
	const Vector3 ray_origin = p_camera->project_ray_origin(p_screen_position);
	const Vector3 ray_direction = p_camera->project_ray_normal(p_screen_position);
	return r_plane.intersects_ray(ray_origin, ray_direction, &r_point);
}

bool BlockTool::_begin_pending(Camera3D *p_camera, const Ref<InputEventMouse> &p_event) {
	ERR_FAIL_NULL_V(p_camera, false);
	ERR_FAIL_COND_V(p_event.is_null(), false);

	Plane candidate_plane;
	Vector3 candidate_origin;
	Basis candidate_basis;
	Vector3 candidate_point;
	bool candidate_surface_hit = false;
	if (!_resolve_start(p_camera, p_event->get_position(), candidate_plane, candidate_origin, candidate_basis, candidate_point, candidate_surface_hit)) {
		return false;
	}

	// The surface/ground decision and its signed-axis tangent frame are frozen for
	// the complete Base/Height gesture.
	drag_plane = candidate_plane;
	drag_plane_origin = candidate_origin;
	drag_basis = candidate_basis;
	gesture_snap_step = LevelEditor::snap_step_or_default();
	LevelEditor *level_editor = LevelEditor::get_singleton();
	gesture_snap_enabled = candidate_surface_hit || (level_editor ? level_editor->is_snap_enabled() : true);
	default_block_height = level_editor ? level_editor->get_default_block_height() : LevelEditor::DEFAULT_BLOCK_HEIGHT;
	press_position = p_event->get_position();
	height_accept_position = Vector2();
	height_accept_armed = false;
	base_drag_start = candidate_point;
	base_drag_point = candidate_point;
	p0 = LevelSnapService::snap_point_to_plane_grid(candidate_point, Transform3D(drag_basis, drag_plane_origin), gesture_snap_step, gesture_snap_enabled);
	p1 = p0;
	p2 = p0 + drag_basis.xform(_derive_base_local_delta()) + drag_plane.normal * default_block_height;
	state = STATE_PENDING;
	_clear_preview();
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
	base_drag_point = intersection;
	p1 = LevelSnapService::snap_point_to_plane_grid(intersection, Transform3D(drag_basis, drag_plane_origin), gesture_snap_step, gesture_snap_enabled);
	p2 = p0 + drag_basis.xform(_derive_base_local_delta()) + drag_plane.normal * default_block_height;
	_update_preview();
	return true;
}

void BlockTool::_update_height(Camera3D *p_camera, const Vector2 &p_screen_position) {
	const Vector3 ray_origin = p_camera->project_ray_origin(p_screen_position);
	const Vector3 ray_direction = p_camera->project_ray_normal(p_screen_position).normalized();
	const Vector3 height_axis = drag_plane.normal.normalized();
	const Vector3 base_endpoint = p0 + drag_basis.xform(_derive_base_local_delta());
	const real_t parallel = Math::abs(ray_direction.dot(height_axis));

	if (parallel >= Math::cos(DRAG_ANGLE_LIMIT)) {
		p2 = base_endpoint + height_axis * default_block_height;
		_update_preview();
		return;
	}

	// Closest point between the mouse ray's infinite line and the frozen normal
	// through the effective base endpoint. The angle guard above keeps the
	// denominator well-conditioned.
	const Vector3 w0 = ray_origin - base_endpoint;
	const real_t ray_dot_axis = ray_direction.dot(height_axis);
	const real_t ray_dot_w0 = ray_direction.dot(w0);
	const real_t axis_dot_w0 = height_axis.dot(w0);
	const real_t denominator = 1.0 - ray_dot_axis * ray_dot_axis;
	if (denominator <= CMP_EPSILON) {
		p2 = base_endpoint + height_axis * default_block_height;
	} else {
		const real_t height = LevelSnapService::snap_delta((axis_dot_w0 - ray_dot_axis * ray_dot_w0) / denominator, gesture_snap_step, gesture_snap_enabled);
		p2 = base_endpoint + height_axis * height;
	}
	_update_preview();
}

void BlockTool::_update_hover(Camera3D *p_camera, const Vector2 &p_screen_position) {
	Plane candidate_plane;
	Vector3 candidate_origin;
	Basis candidate_basis;
	Vector3 candidate_point;
	bool candidate_surface_hit = false;
	if (!_resolve_start(p_camera, p_screen_position, candidate_plane, candidate_origin, candidate_basis, candidate_point, candidate_surface_hit)) {
		_clear_preview();
		return;
	}

	const real_t snap_step = LevelEditor::snap_step_or_default();
	LevelEditor *level_editor = LevelEditor::get_singleton();
	const bool snap_enabled = candidate_surface_hit || (level_editor ? level_editor->is_snap_enabled() : true);
	const Vector3 start = LevelSnapService::snap_point_to_plane_grid(
			candidate_point, Transform3D(candidate_basis, candidate_origin), snap_step, snap_enabled);
	const Vector3 center = start + (candidate_basis.get_column(0) + candidate_basis.get_column(2)) * (snap_step * (real_t)0.5);
	get_overlay().update_footprint(Transform3D(candidate_basis, center), Vector2(snap_step, snap_step));
	_sync_overlay_probe();
}

Vector3 BlockTool::_derive_base_local_delta() const {
	Vector3 local_delta = drag_basis.transposed().xform(p1 - p0);
	local_delta.y = 0.0;
	if (!gesture_snap_enabled) {
		return local_delta;
	}

	// p0/p1 carry grid-snapped positions. Use the raw plane intersections only
	// to choose the side when snapping collapses an extent below one full cell.
	const Vector3 unsnapped_delta = drag_basis.transposed().xform(base_drag_point - base_drag_start);
	if (Math::abs(local_delta.x) < gesture_snap_step) {
		local_delta.x = unsnapped_delta.x < 0.0 ? -gesture_snap_step : gesture_snap_step;
	}
	if (Math::abs(local_delta.z) < gesture_snap_step) {
		local_delta.z = unsnapped_delta.z < 0.0 ? -gesture_snap_step : gesture_snap_step;
	}
	return local_delta;
}

bool BlockTool::_has_base_area() const {
	const Vector3 local_delta = _derive_base_local_delta();
	return Math::abs(local_delta.x) > CMP_EPSILON && Math::abs(local_delta.z) > CMP_EPSILON;
}

bool BlockTool::_get_box_spec(Transform3D &r_frame, Vector3 &r_size) const {
	const Vector3 local_base = _derive_base_local_delta();
	const Vector3 base_endpoint = p0 + drag_basis.xform(local_base);
	const real_t height = (p2 - base_endpoint).dot(drag_plane.normal);
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
	// A transient invalid solve retains the last valid in-gesture geometry.
	_sync_overlay_probe();
}

void BlockTool::_clear_preview() {
	get_overlay().clear();
	_sync_overlay_probe();
}

void BlockTool::_sync_overlay_probe() {
	if (LevelEditorView *level_view = get_view()) {
		level_view->set_meta(StringName("_level_block_overlay_has_geometry"), get_overlay().has_geometry_state());
		level_view->set_meta(StringName("_level_block_state"), int(state));
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
			if (state == STATE_HEIGHT_DRAG) {
				height_accept_position = button->get_position();
				height_accept_armed = true;
				return true;
			}
			return false;
		}

		switch (state) {
			case STATE_PENDING: {
				exit_gesture(); // A bare click never creates or enters undo history.
				return true;
			}
			case STATE_BASE_DRAG: {
				// A release ray miss keeps and accepts the last valid base instead of
				// flashing or silently discarding the gesture.
				_update_base(p_camera, button->get_position());
				if (!_has_base_area()) {
					// Only a snap-disabled, epsilon-degenerate free drag can reach this
					// legacy no-op/re-arm guard.
					exit_gesture();
					_begin_pending(p_camera, button);
					return true;
				}

				const Vector3 base_endpoint = p0 + drag_basis.xform(_derive_base_local_delta());
				p2 = base_endpoint + drag_plane.normal * default_block_height;
				const Vector3 release_ray = p_camera->project_ray_normal(button->get_position()).normalized();
				if (Math::abs(release_ray.dot(drag_plane.normal)) >= Math::cos(DRAG_ANGLE_LIMIT)) {
					commit_gesture();
				} else {
					state = STATE_HEIGHT_DRAG;
					height_accept_armed = false;
					_update_preview();
				}
				return true;
			}
			case STATE_HEIGHT_DRAG: {
				// A click at its press position accepts the already-visible height.
				// Re-solving there can project back to the base and erase the default.
				if (!height_accept_armed || !height_accept_position.is_equal_approx(button->get_position())) {
					_update_height(p_camera, button->get_position());
				}
				height_accept_armed = false;
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
		// Seed the positive one-cell fallback before solving the first drag ray.
		// If that solve misses, the gesture still starts with visible geometry.
		_update_preview();
		_update_base(p_camera, motion->get_position());
		_sync_overlay_probe();
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
	if (state == STATE_IDLE) {
		// Keep camera navigation available: idle hover updates the render-only
		// footprint but does not consume the motion event.
		_update_hover(p_camera, motion->get_position());
	}
	return false;
}

bool BlockTool::_commit_gesture() {
	if (state == STATE_BASE_DRAG) {
		p2 = p0 + drag_basis.xform(_derive_base_local_delta()) + drag_plane.normal * default_block_height;
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
	Node *parent = scene_root;
	if (EditorSelection *selection = document->get_selection()) {
		const TypedArray<Node> selected_nodes = selection->get_selected_nodes();
		if (selected_nodes.size() == 1) {
			Node *selected_node = Object::cast_to<Node>(selected_nodes[0]);
			if (selected_node && (selected_node == scene_root || scene_root->is_ancestor_of(selected_node))) {
				parent = selected_node;
			}
		}
	}

	Transform3D mesh_frame = frame;
	Transform3D block_transform;
	bool use_block_transform = false;
	if (parent != scene_root) {
		Node3D *parent_3d = Object::cast_to<Node3D>(parent);
		if (parent_3d) {
			const Transform3D parent_global = parent_3d->get_global_transform();
			if (parent_global.is_finite() && Math::abs(parent_global.basis.determinant()) > CMP_EPSILON) {
				// Keep LevelMesh's box frame orthonormal. Placement lives on the new child
				// node, converted into the selected parent's local space.
				mesh_frame = Transform3D();
				block_transform = parent_global.affine_inverse() * frame;
				use_block_transform = block_transform.is_finite();
				if (!use_block_transform) {
					mesh_frame = frame;
				}
			}
		}
	}

	Ref<LevelMesh> level_mesh;
	level_mesh.instantiate();
	level_mesh->begin_transaction();
	if (!level_mesh->create_box(mesh_frame, size, 0)) {
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
	if (use_block_transform) {
		block->set_transform(block_transform);
	}

	state = STATE_COMMIT;
	undo_redo->create_action_for_history(TTR("Add Block"), document->get_history_id());
	undo_redo->add_do_method(parent, SNAME("add_child"), block, true);
	undo_redo->add_do_method(block, SNAME("set_owner"), scene_root);
	undo_redo->add_do_reference(block);
	undo_redo->add_undo_method(parent, SNAME("remove_child"), block);
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
	base_drag_start = Vector3();
	base_drag_point = Vector3();
	press_position = Vector2();
	height_accept_position = Vector2();
	gesture_snap_step = LevelEditor::DEFAULT_SNAP_STEP;
	default_block_height = LevelEditor::DEFAULT_BLOCK_HEIGHT;
	gesture_snap_enabled = true;
	height_accept_armed = false;
	_sync_overlay_probe();
}
