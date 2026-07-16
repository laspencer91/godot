/**************************************************************************/
/*  fast_texture_overlay.h                                                */
/**************************************************************************/
/*  G-Level WP17: per-pane modal 2D Fast Texture view state.              */
/**************************************************************************/

#pragma once

#include "scene/gui/control.h"

#include "modules/level_kernel/fast_texture_session.h"

class LevelBlock;
class LevelDocument;
class LevelEditorView;
class Texture2D;

class FastTextureOverlay : public Control {
	struct MeshSession {
		LevelBlock *block = nullptr;
		Ref<FastTextureSession> session;
		PackedInt32Array face_ids;
	};

	enum DragMode {
		DRAG_NONE,
		DRAG_PAN,
		DRAG_TRANSLATE,
		DRAG_SCALE,
	};

	LevelEditorView *level_view = nullptr;
	// The overlay is modal to one LevelEditorView. Retaining its document seam
	// keeps material fallback/selection/undo routing independent of focus moves.
	LevelDocument *document = nullptr;
	Vector<MeshSession> sessions;
	Ref<Texture2D> background_texture;
	Transform2D projection;
	Transform2D nudge;
	Transform2D drag_start_nudge;
	Rect2 drag_base_bounds;
	Vector2 drag_start_uv;
	Vector2 cursor_uv;
	Vector2 last_pointer_position;
	String hud_message;
	int mode = FastTextureSession::MODE_USE_EXISTING;
	int spacing = FastTextureSession::SPACING_LENGTH;
	int subdivision_count = 8;
	int selected_session = 0;
	int selected_face = -1;
	int drag_corner = -1;
	DragMode drag_mode = DRAG_NONE;
	bool repeat_background = true;
	bool show_material = true;
	bool world_scale_units = false;
	bool show_cursor_coordinates = false;
	bool initial_frame_pending = false;

	void _gui_input(const Ref<InputEvent> &p_event);
	void _session_changed(int p_session_index);
	void _draw_overlay();
	void _draw_background();
	void _draw_grid();
	void _draw_faces();
	void _draw_box_gizmo();
	void _draw_hud();
	void _frame_selection();
	void _load_background_texture();
	bool _compute_bounds(bool p_apply_nudge, Rect2 &r_bounds) const;
	// Unnudged selection bounds padded so a degenerate (zero-width or -height)
	// selection still yields a grabbable box gizmo. Returns false when there is
	// nothing selected to bound.
	bool _compute_gizmo_bounds(Rect2 &r_bounds) const;
	Vector<Vector2> _face_viewport_polygon(const PackedVector2Array &p_uvs, const PackedInt32Array &p_loop_starts, const PackedInt32Array &p_loop_counts, int p_face_id) const;
	bool _pick_face(const Vector2 &p_position);
	int _pick_corner_handle(const Vector2 &p_position, const Rect2 &p_base_bounds) const;
	bool _point_inside_gizmo(const Vector2 &p_position, const Rect2 &p_base_bounds) const;
	Vector2 _viewport_to_uv(const Vector2 &p_position) const;
	Vector2 _uv_to_viewport(const Vector2 &p_uv) const;
	void _zoom_at(const Vector2 &p_position, real_t p_factor);
	void _rotate_step(real_t p_angle);
	void _flip(const Vector2 &p_scale);
	void _update_drag(const Ref<InputEventMouseMotion> &p_motion);
	static Transform2D _transform_about(const Transform2D &p_transform, const Vector2 &p_pivot);
	static Vector<Vector2> _rect_corners(const Rect2 &p_rect);
	static const char *_mode_name(int p_mode);
	static const char *_spacing_name(int p_spacing);

protected:
	void _notification(int p_what);

public:
	bool open_from_selection();
	bool handle_modal_input(const Ref<InputEvent> &p_event);
	bool set_mode(int p_mode, int p_spacing);
	bool set_nudge(const Transform2D &p_nudge);
	bool accept();
	void cancel();
	Ref<FastTextureSession> get_primary_session() const;
	int get_session_count() const { return sessions.size(); }

	FastTextureOverlay(LevelEditorView *p_view);
	~FastTextureOverlay();
};
