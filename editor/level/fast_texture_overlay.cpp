/**************************************************************************/
/*  fast_texture_overlay.cpp                                              */
/**************************************************************************/

#include "fast_texture_overlay.h"

#include "core/input/input_event.h"
#include "core/math/geometry_2d.h"
#include "core/object/callable_mp.h"
#include "editor/editor_document.h"
#include "editor/editor_undo_redo_manager.h"
#include "editor/level/level_editor.h"
#include "editor/level/level_editor_view.h"
#include "editor/level/selection_model.h"
#include "editor/themes/editor_scale.h"
#include "scene/resources/font.h"

#include "modules/level_kernel/level_block.h"
#include "modules/level_kernel/level_mesh.h"
#include "modules/level_kernel/level_mesh_baker.h"
#include "modules/level_kernel/level_mesh_data.h"
#include "modules/level_kernel/level_mesh_diff.h"

namespace {

constexpr real_t MIN_ZOOM = 4.0;
constexpr real_t MAX_ZOOM = 8192.0;
constexpr real_t HANDLE_RADIUS = 7.0;

Key event_keycode(const Ref<InputEventKey> &p_key) {
	return p_key->get_keycode() != Key::NONE ? p_key->get_keycode() : p_key->get_physical_keycode();
}

} // namespace

const char *FastTextureOverlay::_mode_name(int p_mode) {
	switch (FastTextureSession::Mode(p_mode)) {
		case FastTextureSession::MODE_USE_EXISTING:
			return "Use Existing";
		case FastTextureSession::MODE_CONFORMING:
			return "Conforming";
		case FastTextureSession::MODE_SQUARE:
			return "Square";
		case FastTextureSession::MODE_FOLLOW_QUADS:
			return "Follow Quads";
		case FastTextureSession::MODE_PLANAR:
			return "Planar";
	}
	return "Unknown";
}

const char *FastTextureOverlay::_spacing_name(int p_spacing) {
	switch (FastTextureSession::SpacingMode(p_spacing)) {
		case FastTextureSession::SPACING_LENGTH:
			return "Length";
		case FastTextureSession::SPACING_EVEN:
			return "Even";
		case FastTextureSession::SPACING_LENGTH_AVERAGE:
			return "Length-Average";
	}
	return "Unknown";
}

Vector<Vector2> FastTextureOverlay::_rect_corners(const Rect2 &p_rect) {
	Vector<Vector2> corners;
	const Vector2 end = p_rect.get_end();
	corners.push_back(p_rect.position);
	corners.push_back(Vector2(end.x, p_rect.position.y));
	corners.push_back(end);
	corners.push_back(Vector2(p_rect.position.x, end.y));
	return corners;
}

Transform2D FastTextureOverlay::_transform_about(const Transform2D &p_transform, const Vector2 &p_pivot) {
	Transform2D result = p_transform;
	result.set_origin(p_pivot - result.basis_xform(p_pivot));
	return result;
}

Vector2 FastTextureOverlay::_viewport_to_uv(const Vector2 &p_position) const {
	if (Math::is_zero_approx(projection.determinant())) {
		return Vector2();
	}
	return projection.affine_inverse().xform(p_position);
}

Vector2 FastTextureOverlay::_uv_to_viewport(const Vector2 &p_uv) const {
	return projection.xform(p_uv);
}

bool FastTextureOverlay::_compute_bounds(bool p_apply_nudge, Rect2 &r_bounds) const {
	bool initialized = false;
	for (const MeshSession &entry : sessions) {
		if (entry.session.is_null() || !entry.session->is_active()) {
			continue;
		}
		const Ref<LevelMeshData> mesh_data = entry.session->get_working_data();
		const PackedVector2Array uvs = p_apply_nudge ? entry.session->get_working_loop_uvs() : entry.session->get_mode_loop_uvs();
		if (mesh_data.is_null() || uvs.is_empty()) {
			continue;
		}
		const PackedInt32Array loop_starts = mesh_data->get_face_loop_starts();
		const PackedInt32Array loop_counts = mesh_data->get_face_loop_counts();
		for (const int face_id : entry.session->get_face_ids()) {
			const int loop_start = loop_starts[face_id];
			const int loop_count = loop_counts[face_id];
			for (int corner = 0; corner < loop_count; corner++) {
				const Vector2 uv = uvs[loop_start + corner];
				if (!initialized) {
					r_bounds = Rect2(uv, Vector2());
					initialized = true;
				} else {
					r_bounds.expand_to(uv);
				}
			}
		}
	}
	return initialized;
}

void FastTextureOverlay::_frame_selection() {
	Rect2 bounds;
	if (!_compute_bounds(true, bounds) || get_size().x <= 1.0 || get_size().y <= 1.0) {
		return;
	}
	Vector2 extent = bounds.size;
	extent.x = MAX(extent.x, (real_t)0.25);
	extent.y = MAX(extent.y, (real_t)0.25);
	const Vector2 available = Vector2(MAX(get_size().x - 160.0 * EDSCALE, 64.0 * EDSCALE),
			MAX(get_size().y - 140.0 * EDSCALE, 64.0 * EDSCALE));
	const real_t scale = CLAMP(MIN(available.x / extent.x, available.y / extent.y), MIN_ZOOM, MAX_ZOOM);
	projection = Transform2D(Vector2(scale, 0.0), Vector2(0.0, -scale), Vector2());
	projection.set_origin(get_size() * 0.5 - projection.basis_xform(bounds.get_center()));
	initial_frame_pending = false;
	queue_redraw();
}

void FastTextureOverlay::_zoom_at(const Vector2 &p_position, real_t p_factor) {
	const Vector2 anchored_uv = _viewport_to_uv(p_position);
	const real_t old_scale = projection[0].length();
	const real_t new_scale = CLAMP(old_scale * p_factor, MIN_ZOOM, MAX_ZOOM);
	projection = Transform2D(Vector2(new_scale, 0.0), Vector2(0.0, -new_scale), Vector2());
	projection.set_origin(p_position - projection.basis_xform(anchored_uv));
	queue_redraw();
}

void FastTextureOverlay::_load_background_texture() {
	background_texture.unref();
	if (sessions.is_empty() || sessions[0].session.is_null()) {
		return;
	}
	const Ref<LevelMeshData> mesh_data = sessions[0].session->get_original_data();
	const PackedInt32Array ids = sessions[0].session->get_face_ids();
	LevelEditor *level_editor = LevelEditor::get_singleton();
	if (mesh_data.is_null() || ids.is_empty() || !document || !level_editor) {
		return;
	}
	const PackedInt32Array material_indices = mesh_data->get_face_material_indices();
	const int material_index = ids[0] < material_indices.size() ? material_indices[ids[0]] : -1;
	const String material_path = mesh_data->get_material_path(material_index);
	const Ref<Material> face_material = LevelMeshBaker::resolve_material_path(material_path);
	background_texture = level_editor->get_material_albedo_texture(face_material, material_path);
	if (background_texture.is_null()) {
		background_texture = level_editor->get_material_albedo_texture(level_editor->get_active_material(document),
				level_editor->get_active_material_path(document));
	}
}

bool FastTextureOverlay::open_from_selection() {
	if (!level_view || !sessions.is_empty()) {
		return false;
	}
	SelectionModel *selection = document ? document->get_selection_model() : nullptr;
	if (!selection || selection->get_mode() != SelectionModel::MODE_FACE ||
			selection->get_selected(SelectionModel::FEATURE_FACE).is_empty()) {
		hud_message = TTR("Fast Texture needs a face selection.");
		return false;
	}

	auto find_entry = [&](LevelBlock *p_block) -> MeshSession * {
		for (MeshSession &entry : sessions) {
			if (entry.block == p_block) {
				return &entry;
			}
		}
		MeshSession entry;
		entry.block = p_block;
		entry.session.instantiate();
		entry.session->set_mesh(p_block->get_level_mesh());
		sessions.push_back(entry);
		return &sessions.write[sessions.size() - 1];
	};

	for (const SelectionModel::Element &element : selection->get_selected(SelectionModel::FEATURE_FACE)) {
		LevelBlock *block = nullptr;
		Ref<LevelMesh> mesh;
		int face_id = -1;
		if (!selection->resolve(element, block, mesh, face_id) || !block || mesh.is_null()) {
			continue;
		}
		MeshSession *entry = find_entry(block);
		if (!entry->face_ids.has(face_id)) {
			entry->face_ids.push_back(face_id);
		}
	}

	for (int i = sessions.size() - 1; i >= 0; i--) {
		if (sessions[i].face_ids.is_empty() || !sessions[i].session->open(sessions[i].face_ids)) {
			for (MeshSession &opened : sessions) {
				opened.session->cancel();
			}
			hud_message = sessions[i].face_ids.is_empty() ? TTR("Fast Texture could not resolve the selected faces.") : sessions[i].session->get_last_error();
			sessions.clear();
			return false;
		}
	}

	for (int i = 0; i < sessions.size(); i++) {
		sessions.write[i].session->connect(SNAME("changed"), callable_mp(this, &FastTextureOverlay::_session_changed).bind(i));
	}
	const PackedInt32Array first_ids = sessions[0].session->get_face_ids();
	selected_session = 0;
	selected_face = first_ids.is_empty() ? -1 : first_ids[0];
	mode = FastTextureSession::MODE_USE_EXISTING;
	spacing = FastTextureSession::SPACING_LENGTH;
	nudge = Transform2D();
	_load_background_texture();
	initial_frame_pending = true;
	callable_mp(this, &FastTextureOverlay::_frame_selection).call_deferred();
	return true;
}

Ref<FastTextureSession> FastTextureOverlay::get_primary_session() const {
	return sessions.is_empty() ? Ref<FastTextureSession>() : sessions[0].session;
}

void FastTextureOverlay::_session_changed(int p_session_index) {
	if (p_session_index < 0 || p_session_index >= sessions.size() || sessions[p_session_index].session.is_null()) {
		return;
	}
	const Ref<FastTextureSession> session = sessions[p_session_index].session;
	if (!session->get_last_error().is_empty()) {
		hud_message = session->get_last_error();
	}
	if (p_session_index == 0 && session->is_active()) {
		mode = session->get_mode();
		spacing = session->get_spacing();
		nudge = session->get_nudge();
	}
	queue_redraw();
}

bool FastTextureOverlay::set_mode(int p_mode, int p_spacing) {
	if (sessions.is_empty()) {
		return false;
	}
	const int previous_mode = mode;
	const int previous_spacing = spacing;
	for (int i = 0; i < sessions.size(); i++) {
		if (!sessions[i].session->set_mode(p_mode, p_spacing)) {
			const String rejection = sessions[i].session->get_last_error();
			for (int restore = 0; restore < i; restore++) {
				sessions.write[restore].session->set_mode(previous_mode, previous_spacing);
			}
			hud_message = rejection;
			queue_redraw();
			return false;
		}
	}
	mode = p_mode;
	spacing = p_spacing;
	hud_message.clear();
	queue_redraw();
	return true;
}

bool FastTextureOverlay::set_nudge(const Transform2D &p_nudge) {
	if (sessions.is_empty() || !p_nudge.is_finite()) {
		return false;
	}
	const Transform2D previous = nudge;
	for (int i = 0; i < sessions.size(); i++) {
		if (!sessions[i].session->set_nudge(p_nudge)) {
			for (int restore = 0; restore < i; restore++) {
				sessions.write[restore].session->set_nudge(previous);
			}
			return false;
		}
	}
	nudge = p_nudge;
	hud_message.clear();
	queue_redraw();
	return true;
}

bool FastTextureOverlay::accept() {
	if (sessions.is_empty() || !level_view) {
		return false;
	}
	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	if (!undo_redo || !document) {
		hud_message = TTR("Fast Texture could not access this document's undo history.");
		queue_redraw();
		return false;
	}
	for (const MeshSession &entry : sessions) {
		if (!entry.session->can_accept()) {
			hud_message = TTR("The source mesh changed while Fast Texture was open.");
			queue_redraw();
			return false;
		}
	}
	Vector<int> accepted;
	for (int i = 0; i < sessions.size(); i++) {
		if (!sessions[i].session->accept()) {
			hud_message = sessions[i].session->get_last_error();
			for (int rollback = accepted.size() - 1; rollback >= 0; rollback--) {
				const Ref<FastTextureSession> session = sessions[accepted[rollback]].session;
				if (session->get_last_diff().is_valid()) {
					session->get_mesh()->revert_diff(session->get_last_diff());
				}
			}
			queue_redraw();
			return false;
		}
		accepted.push_back(i);
	}

	bool has_diff = false;
	for (const MeshSession &entry : sessions) {
		has_diff = has_diff || entry.session->get_last_diff().is_valid();
	}
	if (has_diff) {
		undo_redo->create_action_for_history(TTR("Fast Texture"), document->get_history_id());
		for (const MeshSession &entry : sessions) {
			if (entry.session->get_last_diff().is_valid()) {
				undo_redo->add_do_method(entry.session->get_mesh().ptr(), SNAME("apply_diff"), entry.session->get_last_diff());
			}
		}
		for (int i = sessions.size() - 1; i >= 0; i--) {
			if (sessions[i].session->get_last_diff().is_valid()) {
				undo_redo->add_undo_method(sessions[i].session->get_mesh().ptr(), SNAME("revert_diff"), sessions[i].session->get_last_diff());
			}
		}
		undo_redo->commit_action(false);
	}
	return true;
}

void FastTextureOverlay::cancel() {
	for (MeshSession &entry : sessions) {
		entry.session->cancel();
	}
}

bool FastTextureOverlay::_compute_gizmo_bounds(Rect2 &r_bounds) const {
	if (!_compute_bounds(false, r_bounds)) {
		return false;
	}
	if (r_bounds.size.x < CMP_EPSILON) {
		r_bounds = r_bounds.grow_side(SIDE_LEFT, 0.125);
		r_bounds = r_bounds.grow_side(SIDE_RIGHT, 0.125);
	}
	if (r_bounds.size.y < CMP_EPSILON) {
		r_bounds = r_bounds.grow_side(SIDE_TOP, 0.125);
		r_bounds = r_bounds.grow_side(SIDE_BOTTOM, 0.125);
	}
	return true;
}

Vector<Vector2> FastTextureOverlay::_face_viewport_polygon(const PackedVector2Array &p_uvs, const PackedInt32Array &p_loop_starts, const PackedInt32Array &p_loop_counts, int p_face_id) const {
	Vector<Vector2> polygon;
	for (int corner = 0; corner < p_loop_counts[p_face_id]; corner++) {
		polygon.push_back(_uv_to_viewport(p_uvs[p_loop_starts[p_face_id] + corner]));
	}
	return polygon;
}

bool FastTextureOverlay::_pick_face(const Vector2 &p_position) {
	for (int session_index = sessions.size() - 1; session_index >= 0; session_index--) {
		const Ref<FastTextureSession> session = sessions[session_index].session;
		const Ref<LevelMeshData> mesh_data = session->get_working_data();
		const PackedVector2Array uvs = session->get_working_loop_uvs();
		if (mesh_data.is_null()) {
			continue;
		}
		const PackedInt32Array loop_starts = mesh_data->get_face_loop_starts();
		const PackedInt32Array loop_counts = mesh_data->get_face_loop_counts();
		const PackedInt32Array ids = session->get_face_ids();
		for (int face_index = ids.size() - 1; face_index >= 0; face_index--) {
			const int face_id = ids[face_index];
			const Vector<Vector2> polygon = _face_viewport_polygon(uvs, loop_starts, loop_counts, face_id);
			if (Geometry2D::is_point_in_polygon(p_position, polygon)) {
				selected_session = session_index;
				selected_face = face_id;
				queue_redraw();
				return true;
			}
		}
	}
	return false;
}

int FastTextureOverlay::_pick_corner_handle(const Vector2 &p_position, const Rect2 &p_base_bounds) const {
	const Vector<Vector2> corners = _rect_corners(p_base_bounds);
	for (int i = 0; i < corners.size(); i++) {
		const Vector2 handle_position = _uv_to_viewport(nudge.xform(corners[i]));
		const real_t handle_radius = HANDLE_RADIUS * EDSCALE;
		if (handle_position.distance_squared_to(p_position) <= handle_radius * handle_radius) {
			return i;
		}
	}
	return -1;
}

bool FastTextureOverlay::_point_inside_gizmo(const Vector2 &p_position, const Rect2 &p_base_bounds) const {
	if (Math::is_zero_approx(nudge.determinant())) {
		return false;
	}
	return p_base_bounds.has_point(nudge.affine_inverse().xform(_viewport_to_uv(p_position)));
}

void FastTextureOverlay::_rotate_step(real_t p_angle) {
	Rect2 base_bounds;
	if (!_compute_bounds(false, base_bounds)) {
		return;
	}
	const Vector2 pivot = nudge.xform(base_bounds.get_center());
	set_nudge(_transform_about(Transform2D(p_angle, Vector2()), pivot) * nudge);
}

void FastTextureOverlay::_flip(const Vector2 &p_scale) {
	Rect2 base_bounds;
	if (!_compute_bounds(false, base_bounds)) {
		return;
	}
	const Vector2 pivot = nudge.xform(base_bounds.get_center());
	const Transform2D flip(0.0, p_scale, 0.0, Vector2());
	set_nudge(_transform_about(flip, pivot) * nudge);
}

void FastTextureOverlay::_update_drag(const Ref<InputEventMouseMotion> &p_motion) {
	if (drag_mode == DRAG_PAN) {
		projection.set_origin(projection.get_origin() + p_motion->get_relative());
		queue_redraw();
		return;
	}
	if (drag_mode == DRAG_NONE) {
		return;
	}
	const Vector2 mouse_uv = _viewport_to_uv(p_motion->get_position());
	if (drag_mode == DRAG_TRANSLATE) {
		const Vector2 delta = mouse_uv - drag_start_uv;
		set_nudge(Transform2D(0.0, delta) * drag_start_nudge);
		return;
	}
	if (drag_mode != DRAG_SCALE || drag_corner < 0 || Math::is_zero_approx(drag_start_nudge.determinant())) {
		return;
	}
	const Vector<Vector2> corners = _rect_corners(drag_base_bounds);
	const Vector2 center = drag_base_bounds.get_center();
	const Vector2 start_vector = corners[drag_corner] - center;
	const Vector2 local_mouse = drag_start_nudge.affine_inverse().xform(mouse_uv);
	const Vector2 current_vector = local_mouse - center;
	Vector2 scale(
			Math::is_zero_approx(start_vector.x) ? 1.0 : current_vector.x / start_vector.x,
			Math::is_zero_approx(start_vector.y) ? 1.0 : current_vector.y / start_vector.y);
	if (p_motion->is_shift_pressed()) {
		const real_t uniform = Math::abs(scale.x - 1.0) >= Math::abs(scale.y - 1.0) ? scale.x : scale.y;
		scale = Vector2(uniform, uniform);
	}
	for (int axis = 0; axis < 2; axis++) {
		if (Math::abs(scale[axis]) < 0.01) {
			scale[axis] = scale[axis] < 0.0 ? -0.01 : 0.01;
		}
	}
	const Transform2D scale_transform(0.0, scale, 0.0, Vector2());
	set_nudge(drag_start_nudge * _transform_about(scale_transform, center));
}

bool FastTextureOverlay::handle_modal_input(const Ref<InputEvent> &p_event) {
	if (p_event.is_null()) {
		return true;
	}
	Ref<InputEventKey> key = p_event;
	if (key.is_valid()) {
		if (!key->is_pressed() || key->is_echo()) {
			return true;
		}
		const Key code = event_keycode(key);
		if (code == Key::ESCAPE) {
			level_view->_close_fast_texture(false);
			return true;
		}
		if (code == Key::ENTER || code == Key::KP_ENTER) {
			level_view->_close_fast_texture(true);
			return true;
		}
		if (key->is_ctrl_pressed() && !key->is_alt_pressed() &&
				(code == Key::KEY_1 || code == Key::KEY_2 || code == Key::KEY_3)) {
			const int target_spacing = code == Key::KEY_1 ? FastTextureSession::SPACING_LENGTH : (code == Key::KEY_2 ? FastTextureSession::SPACING_EVEN : FastTextureSession::SPACING_LENGTH_AVERAGE);
			set_mode(FastTextureSession::MODE_FOLLOW_QUADS, target_spacing);
			return true;
		}
		if (!key->is_ctrl_pressed() && !key->is_alt_pressed()) {
			switch (code) {
				case Key::KEY_1:
					set_mode(FastTextureSession::MODE_USE_EXISTING, spacing);
					return true;
				case Key::KEY_2:
					set_mode(FastTextureSession::MODE_CONFORMING, spacing);
					return true;
				case Key::KEY_3:
					set_mode(FastTextureSession::MODE_SQUARE, spacing);
					return true;
				case Key::KEY_4:
					set_mode(FastTextureSession::MODE_FOLLOW_QUADS, spacing);
					return true;
				case Key::KEY_5:
					set_mode(FastTextureSession::MODE_PLANAR, spacing);
					return true;
				case Key::Q:
					_rotate_step(-Math::deg_to_rad((real_t)15.0));
					return true;
				case Key::E:
					_rotate_step(Math::deg_to_rad((real_t)15.0));
					return true;
				case Key::F:
					_frame_selection();
					return true;
				case Key::B:
					repeat_background = !repeat_background;
					queue_redraw();
					return true;
				case Key::G:
					world_scale_units = !world_scale_units;
					queue_redraw();
					return true;
				case Key::C:
					show_cursor_coordinates = !show_cursor_coordinates;
					queue_redraw();
					return true;
				case Key::T:
					show_material = !show_material;
					queue_redraw();
					return true;
				case Key::BRACKETLEFT:
					subdivision_count = MAX(1, subdivision_count / 2);
					queue_redraw();
					return true;
				case Key::BRACKETRIGHT:
					subdivision_count = MIN(64, subdivision_count * 2);
					queue_redraw();
					return true;
				default:
					break;
			}
		}
		if (key->is_alt_pressed() && !key->is_ctrl_pressed() && code == Key::R) {
			_flip(Vector2(-1.0, 1.0));
			return true;
		}
		if (key->is_alt_pressed() && !key->is_ctrl_pressed() && code == Key::T) {
			_flip(Vector2(1.0, -1.0));
			return true;
		}
		return true;
	}

	Ref<InputEventMouseButton> button = p_event;
	if (button.is_valid()) {
		last_pointer_position = button->get_position();
		cursor_uv = _viewport_to_uv(last_pointer_position);
		if (button->get_button_index() == MouseButton::WHEEL_UP && button->is_pressed()) {
			_zoom_at(button->get_position(), 1.18);
			return true;
		}
		if (button->get_button_index() == MouseButton::WHEEL_DOWN && button->is_pressed()) {
			_zoom_at(button->get_position(), 1.0 / 1.18);
			return true;
		}
		if (button->get_button_index() == MouseButton::MIDDLE) {
			drag_mode = button->is_pressed() ? DRAG_PAN : DRAG_NONE;
			if (button->is_pressed()) {
				grab_focus();
			}
			return true;
		}
		if (button->get_button_index() == MouseButton::LEFT) {
			if (!button->is_pressed()) {
				drag_mode = DRAG_NONE;
				drag_corner = -1;
				return true;
			}
			grab_focus();
			_pick_face(button->get_position());
			Rect2 base_bounds;
			if (_compute_gizmo_bounds(base_bounds)) {
				drag_corner = _pick_corner_handle(button->get_position(), base_bounds);
				drag_mode = drag_corner >= 0 ? DRAG_SCALE : (_point_inside_gizmo(button->get_position(), base_bounds) ? DRAG_TRANSLATE : DRAG_NONE);
				drag_base_bounds = base_bounds;
				drag_start_nudge = nudge;
				drag_start_uv = _viewport_to_uv(button->get_position());
			}
			return true;
		}
		return true;
	}

	Ref<InputEventMouseMotion> motion = p_event;
	if (motion.is_valid()) {
		last_pointer_position = motion->get_position();
		cursor_uv = _viewport_to_uv(last_pointer_position);
		_update_drag(motion);
		if (show_cursor_coordinates) {
			queue_redraw();
		}
		return true;
	}
	return true;
}

void FastTextureOverlay::_gui_input(const Ref<InputEvent> &p_event) {
	handle_modal_input(p_event);
	accept_event();
}

void FastTextureOverlay::_draw_background() {
	draw_rect(Rect2(Vector2(), get_size()), Color(0.025, 0.03, 0.04, 0.92), true);
	if (!show_material || background_texture.is_null()) {
		const real_t cell = 24.0 * EDSCALE;
		for (int y = 0; y * cell < get_size().y; y++) {
			for (int x = 0; x * cell < get_size().x; x++) {
				const Color color = ((x + y) & 1) == 0 ? Color(0.16, 0.17, 0.19, 0.72) : Color(0.09, 0.10, 0.12, 0.72);
				draw_rect(Rect2(Vector2(x, y) * cell, Vector2(cell, cell)), color, true);
			}
		}
		return;
	}

	Vector<Vector2> points;
	Vector<Vector2> uvs;
	if (repeat_background) {
		points.push_back(Vector2());
		points.push_back(Vector2(get_size().x, 0.0));
		points.push_back(get_size());
		points.push_back(Vector2(0.0, get_size().y));
		for (const Vector2 &point : points) {
			uvs.push_back(_viewport_to_uv(point));
		}
	} else {
		const Vector<Vector2> uv_corners = _rect_corners(Rect2(Vector2(), Vector2(1.0, 1.0)));
		for (const Vector2 &uv : uv_corners) {
			points.push_back(_uv_to_viewport(uv));
			uvs.push_back(uv);
		}
	}
	draw_colored_polygon(points, Color(1.0, 1.0, 1.0, 0.72), uvs, background_texture);
}

void FastTextureOverlay::_draw_grid() {
	if (Math::is_zero_approx(projection.determinant())) {
		return;
	}
	const Vector2 corners[4] = {
		_viewport_to_uv(Vector2()),
		_viewport_to_uv(Vector2(get_size().x, 0.0)),
		_viewport_to_uv(get_size()),
		_viewport_to_uv(Vector2(0.0, get_size().y)),
	};
	real_t min_x = corners[0].x;
	real_t max_x = corners[0].x;
	real_t min_y = corners[0].y;
	real_t max_y = corners[0].y;
	for (int i = 1; i < 4; i++) {
		min_x = MIN(min_x, corners[i].x);
		max_x = MAX(max_x, corners[i].x);
		min_y = MIN(min_y, corners[i].y);
		max_y = MAX(max_y, corners[i].y);
	}
	const real_t zoom = projection[0].length();
	const real_t subdivision_step = 1.0 / subdivision_count;
	if (subdivision_step * zoom >= 5.0 * EDSCALE) {
		const int first_x = Math::floor(min_x / subdivision_step);
		const int last_x = Math::ceil(max_x / subdivision_step);
		for (int i = first_x; i <= last_x && i - first_x < 2048; i++) {
			if (i % subdivision_count == 0) {
				continue;
			}
			const real_t x = i * subdivision_step;
			draw_dashed_line(_uv_to_viewport(Vector2(x, min_y)), _uv_to_viewport(Vector2(x, max_y)),
					Color(0.58, 0.62, 0.70, 0.16), 1.0, 4.0 * EDSCALE);
		}
		const int first_y = Math::floor(min_y / subdivision_step);
		const int last_y = Math::ceil(max_y / subdivision_step);
		for (int i = first_y; i <= last_y && i - first_y < 2048; i++) {
			if (i % subdivision_count == 0) {
				continue;
			}
			const real_t y = i * subdivision_step;
			draw_dashed_line(_uv_to_viewport(Vector2(min_x, y)), _uv_to_viewport(Vector2(max_x, y)),
					Color(0.58, 0.62, 0.70, 0.16), 1.0, 4.0 * EDSCALE);
		}
	}

	const int skip = MAX(1, (int)Math::ceil((20.0 * EDSCALE) / zoom));
	const int first_x = (int)Math::floor(min_x / skip) * skip;
	const int last_x = (int)Math::ceil(max_x / skip) * skip;
	for (int x = first_x; x <= last_x && (x - first_x) / skip < 2048; x += skip) {
		draw_line(_uv_to_viewport(Vector2(x, min_y)), _uv_to_viewport(Vector2(x, max_y)),
				x == 0 ? Color(0.82, 0.88, 1.0, 0.72) : Color(0.70, 0.74, 0.82, 0.34),
				x == 0 ? 1.5 * EDSCALE : 1.0);
	}
	const int first_y = (int)Math::floor(min_y / skip) * skip;
	const int last_y = (int)Math::ceil(max_y / skip) * skip;
	for (int y = first_y; y <= last_y && (y - first_y) / skip < 2048; y += skip) {
		draw_line(_uv_to_viewport(Vector2(min_x, y)), _uv_to_viewport(Vector2(max_x, y)),
				y == 0 ? Color(0.82, 0.88, 1.0, 0.72) : Color(0.70, 0.74, 0.82, 0.34),
				y == 0 ? 1.5 * EDSCALE : 1.0);
	}
}

void FastTextureOverlay::_draw_faces() {
	for (int session_index = 0; session_index < sessions.size(); session_index++) {
		const Ref<FastTextureSession> session = sessions[session_index].session;
		const Ref<LevelMeshData> mesh_data = session->get_working_data();
		const PackedVector2Array uvs = session->get_working_loop_uvs();
		if (mesh_data.is_null()) {
			continue;
		}
		const PackedInt32Array loop_starts = mesh_data->get_face_loop_starts();
		const PackedInt32Array loop_counts = mesh_data->get_face_loop_counts();
		for (const int face_id : session->get_face_ids()) {
			Vector<Vector2> polygon = _face_viewport_polygon(uvs, loop_starts, loop_counts, face_id);
			const bool selected = session_index == selected_session && face_id == selected_face;
			draw_colored_polygon(polygon, selected ? Color(1.0, 0.56, 0.16, 0.36) : Color(0.28, 0.68, 1.0, 0.20));
			polygon.push_back(polygon[0]);
			draw_polyline(polygon, selected ? Color(1.0, 0.68, 0.25, 1.0) : Color(0.44, 0.78, 1.0, 0.92),
					selected ? 2.5 * EDSCALE : 1.5 * EDSCALE, true);
		}
	}
}

void FastTextureOverlay::_draw_box_gizmo() {
	Rect2 base_bounds;
	if (!_compute_gizmo_bounds(base_bounds)) {
		return;
	}
	Vector<Vector2> corners = _rect_corners(base_bounds);
	for (Vector2 &corner : corners) {
		corner = _uv_to_viewport(nudge.xform(corner));
	}
	corners.push_back(corners[0]);
	draw_polyline(corners, Color(1.0, 0.72, 0.20, 0.95), 2.0 * EDSCALE, true);
	for (int i = 0; i < 4; i++) {
		const Vector2 handle = corners[i];
		const Rect2 handle_rect(handle - Vector2(HANDLE_RADIUS, HANDLE_RADIUS) * EDSCALE,
				Vector2(HANDLE_RADIUS * 2.0, HANDLE_RADIUS * 2.0) * EDSCALE);
		draw_rect(handle_rect, Color(0.08, 0.09, 0.11, 0.96), true);
		draw_rect(handle_rect, Color(1.0, 0.76, 0.28, 1.0), false, 1.5 * EDSCALE);
	}
	const Vector2 center = _uv_to_viewport(nudge.xform(base_bounds.get_center()));
	draw_line(center - Vector2(5, 0) * EDSCALE, center + Vector2(5, 0) * EDSCALE, Color(1.0, 0.76, 0.28), 1.5 * EDSCALE);
	draw_line(center - Vector2(0, 5) * EDSCALE, center + Vector2(0, 5) * EDSCALE, Color(1.0, 0.76, 0.28), 1.5 * EDSCALE);
}

void FastTextureOverlay::_draw_hud() {
	const Ref<Font> font = get_theme_font(SceneStringName(font), SNAME("Label"));
	const int font_size = get_theme_font_size(SceneStringName(font_size), SNAME("Label"));
	if (font.is_null()) {
		return;
	}
	String mode_line = vformat("Fast Texture  |  %s", _mode_name(mode));
	if (mode == FastTextureSession::MODE_FOLLOW_QUADS) {
		mode_line += vformat(" (%s)", _spacing_name(spacing));
	}
	mode_line += vformat("  |  Grid 1/%d  |  %s", subdivision_count, world_scale_units ? "World units" : "UV units");
	const String help_line = "1 Existing  2 Conforming  3 Square  4 Follow  5 Planar  |  Enter Accept  Esc Cancel";
	const real_t line_height = font->get_height(font_size) + 5.0 * EDSCALE;
	const real_t panel_width = MIN(get_size().x - 24.0 * EDSCALE, 760.0 * EDSCALE);
	const int line_count = hud_message.is_empty() ? 2 : 3;
	draw_rect(Rect2(Vector2(12, 12) * EDSCALE, Vector2(panel_width, line_height * line_count + 10.0 * EDSCALE)),
			Color(0.015, 0.018, 0.024, 0.88), true);
	Vector2 text_position(20.0 * EDSCALE, 20.0 * EDSCALE + font->get_ascent(font_size));
	draw_string(font, text_position, mode_line, HORIZONTAL_ALIGNMENT_LEFT, panel_width - 16.0 * EDSCALE, font_size, Color(0.93, 0.95, 1.0));
	text_position.y += line_height;
	draw_string(font, text_position, help_line, HORIZONTAL_ALIGNMENT_LEFT, panel_width - 16.0 * EDSCALE, font_size, Color(0.68, 0.72, 0.80));
	if (!hud_message.is_empty()) {
		text_position.y += line_height;
		draw_string(font, text_position, hud_message, HORIZONTAL_ALIGNMENT_LEFT, panel_width - 16.0 * EDSCALE, font_size, Color(1.0, 0.55, 0.32));
	}
	if (show_cursor_coordinates) {
		const String coordinate = world_scale_units ? vformat("%.3f m, %.3f m", cursor_uv.x, cursor_uv.y) : vformat("U %.3f  V %.3f", cursor_uv.x, cursor_uv.y);
		const Vector2 coordinate_size = font->get_string_size(coordinate, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size);
		const Rect2 coordinate_panel(last_pointer_position + Vector2(14, 14) * EDSCALE,
				coordinate_size + Vector2(12, 8) * EDSCALE);
		draw_rect(coordinate_panel, Color(0.015, 0.018, 0.024, 0.88), true);
		draw_string(font, coordinate_panel.position + Vector2(6, 4) * EDSCALE + Vector2(0, font->get_ascent(font_size)),
				coordinate, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size, Color(0.92, 0.95, 1.0));
	}
}

void FastTextureOverlay::_draw_overlay() {
	_draw_background();
	_draw_grid();
	_draw_faces();
	_draw_box_gizmo();
	_draw_hud();
}

void FastTextureOverlay::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_DRAW:
			_draw_overlay();
			break;
		case NOTIFICATION_RESIZED:
			if (initial_frame_pending) {
				callable_mp(this, &FastTextureOverlay::_frame_selection).call_deferred();
			}
			break;
	}
}

FastTextureOverlay::FastTextureOverlay(LevelEditorView *p_view) :
		level_view(p_view),
		document(p_view ? p_view->get_level_document() : nullptr) {
	ERR_FAIL_NULL(level_view);
	ERR_FAIL_NULL(document);
	set_name("FastTextureOverlay");
	set_anchors_and_offsets_preset(PRESET_FULL_RECT);
	set_focus_mode(FOCUS_ALL);
	set_mouse_filter(MOUSE_FILTER_STOP);
	set_texture_repeat(TEXTURE_REPEAT_ENABLED);
	projection = Transform2D(Vector2(100.0, 0.0), Vector2(0.0, -100.0), Vector2(400.0, 300.0));
	connect(SceneStringName(gui_input), callable_mp(this, &FastTextureOverlay::_gui_input));
}

FastTextureOverlay::~FastTextureOverlay() {
	cancel();
}
