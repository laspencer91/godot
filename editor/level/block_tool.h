/**************************************************************************/
/*  block_tool.h                                                          */
/**************************************************************************/
/*  G-Level LE0: frozen-plane, two-stage drag-create block tool.          */
/**************************************************************************/

#pragma once

#include "core/math/plane.h"
#include "editor/level/level_editor.h"
#include "editor/level/level_editor_tool.h"

class BlockTool : public LevelEditorTool {
	GDCLASS(BlockTool, LevelEditorTool);

public:
	enum GestureState {
		STATE_IDLE,
		STATE_PENDING,
		STATE_BASE_DRAG,
		STATE_HEIGHT_DRAG,
		STATE_COMMIT,
	};

private:
	static constexpr real_t DRAG_ANGLE_LIMIT = Math::TAU / 36.0; // 10 degrees.

	GestureState state = STATE_IDLE;
	Plane drag_plane;
	Vector3 drag_plane_origin;
	Basis drag_basis;
	Vector3 p0;
	Vector3 p1;
	Vector3 p2;
	Vector3 base_drag_start;
	Vector3 base_drag_point;
	Vector2 press_position;
	Vector2 height_accept_position;
	real_t gesture_snap_step = LevelEditor::DEFAULT_SNAP_STEP;
	real_t default_block_height = LevelEditor::DEFAULT_BLOCK_HEIGHT;
	bool gesture_snap_enabled = true;
	bool height_accept_armed = false;

	static Vector3 _nearest_world_axis(const Vector3 &p_normal);
	static Basis _axis_tangent_basis(const Vector3 &p_axis_normal);
	bool _query_surface_hit(Camera3D *p_camera, const Vector2 &p_screen_position, Vector3 &r_position, Vector3 &r_normal) const;
	bool _resolve_start(Camera3D *p_camera, const Vector2 &p_screen_position, Plane &r_plane,
			Vector3 &r_plane_origin, Basis &r_basis, Vector3 &r_point, bool &r_surface_hit) const;
	bool _begin_pending(Camera3D *p_camera, const Ref<InputEventMouse> &p_event);
	bool _intersect_drag_plane(Camera3D *p_camera, const Vector2 &p_screen_position, Vector3 &r_point) const;
	bool _update_base(Camera3D *p_camera, const Vector2 &p_screen_position);
	void _update_height(Camera3D *p_camera, const Vector2 &p_screen_position);
	void _update_hover(Camera3D *p_camera, const Vector2 &p_screen_position);
	Vector3 _derive_base_local_delta() const;
	bool _has_base_area() const;
	bool _get_box_spec(Transform3D &r_frame, Vector3 &r_size) const;
	void _update_preview();
	void _clear_preview();
	void _sync_overlay_probe();

protected:
	static void _bind_methods() {}
	virtual bool _handle_input(Camera3D *p_camera, const Ref<InputEvent> &p_event) override;
	virtual bool _commit_gesture() override;
	virtual bool _has_active_gesture() const override { return state != STATE_IDLE; }
	virtual void _reset_gesture() override;

public:
	GestureState get_state() const { return state; }
};

VARIANT_ENUM_CAST(BlockTool::GestureState);
