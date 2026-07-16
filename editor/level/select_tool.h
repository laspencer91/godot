/**************************************************************************/
/*  select_tool.h                                                         */
/**************************************************************************/
/*  G-Level LE1: geometric block/sub-object picking and selection input.  */
/**************************************************************************/

#pragma once

#include "editor/level/level_editor.h"
#include "editor/level/level_editor_tool.h"
#include "editor/level/selection_model.h"
#include "editor/level/transform_gizmo.h"

class Camera3D;
class LevelBlock;
class LevelMesh;
class LevelMeshData;
class LevelMeshDiff;

class SelectTool : public LevelEditorTool {
	GDCLASS(SelectTool, LevelEditorTool);

	struct SurfaceHit {
		LevelBlock *block = nullptr;
		real_t t = Math::INF;
		int face_id = -1;
		int triangle_id = -1;
		int local_triangle = -1;

		bool operator<(const SurfaceHit &p_other) const;
	};

	struct ScreenCandidate {
		SelectionModel::Element element;
		real_t pixel_distance = Math::INF;
		real_t depth = Math::INF;

		bool operator<(const ScreenCandidate &p_other) const;
	};

	static constexpr real_t VERTEX_TOLERANCE_PX = 10.0;

	enum TransformDragMode {
		TRANSFORM_DRAG_NONE,
		TRANSFORM_DRAG_MOVE,
		TRANSFORM_DRAG_ROTATE,
		TRANSFORM_DRAG_FACE_EXTRUDE,
		TRANSFORM_DRAG_PUSH_PULL,
		TRANSFORM_DRAG_EDGE_EXTRUDE,
		TRANSFORM_DRAG_OBJECT_MOVE,
	};

	enum TransformConstraintMode {
		TRANSFORM_CONSTRAINT_FREE,
		TRANSFORM_CONSTRAINT_AXIS,
		TRANSFORM_CONSTRAINT_PLANE,
	};

	struct MeshDragState {
		LevelBlock *block = nullptr;
		Ref<LevelMesh> mesh;
		PackedInt32Array face_ids;
		PackedInt32Array edge_ids;
		PackedInt32Array vertex_ids;
		PackedVector3Array original_positions;
		PackedVector3Array push_directions;
		Ref<LevelMeshDiff> topology_diff;
		Ref<LevelMeshDiff> geometry_diff;
		PackedInt64Array created_face_handles;

		void capture_original_positions();
	};

	struct ObjectDragState {
		LevelBlock *block = nullptr;
		Transform3D original_transform;
		Transform3D preview_transform;
	};

	bool pointer_down = false;
	bool marquee_active = false;
	bool press_was_double_click = false;
	bool xray_enabled[2] = {};
	// Deferred selection-polish seam: cursor-cell candidate cycling, paint/path
	// drag, grow/shrink, plane-wide fill, and marquee touch mode live here.
	Vector2 pointer_position;
	bool pointer_position_valid = false;
	Vector2 press_position;
	Vector2 current_position;
	SelectionModel::Operation press_operation = SelectionModel::OP_REPLACE;
	bool transform_candidate = false;
	bool transform_active = false;
	bool transform_committed = false;
	bool gesture_shift = false;
	bool gesture_ctrl = false;
	TransformDragMode transform_drag_mode = TRANSFORM_DRAG_NONE;
	TransformConstraintMode transform_constraint_mode = TRANSFORM_CONSTRAINT_FREE;
	int transform_constraint_axis = Vector3::AXIS_X;
	Vector<MeshDragState> mesh_drag_states;
	Vector<ObjectDragState> object_drag_states;
	Plane transform_drag_plane;
	Vector3 transform_pivot;
	Vector3 transform_axis;
	Vector3 transform_press_point;
	real_t transform_press_axis_parameter = 0.0;
	bool transform_axis_screen_fallback = false;
	Vector3 transform_view_axis;
	Vector2 transform_rotation_pivot_screen;
	Vector2 transform_rotation_press_vector;
	real_t transform_rotation_angle = 0.0;
	bool transform_rotation_reference_valid = false;
	bool transform_rotation_commit_on_release = false;
	real_t transform_snap_step = LevelEditor::DEFAULT_SNAP_STEP;
	TransformGizmo transform_gizmo;
	bool texture_flow_active = false;
	LevelBlock *texture_flow_block = nullptr;
	Ref<LevelMesh> texture_flow_mesh;
	PackedInt32Array texture_flow_faces;

	SelectionModel *get_selection_model() const;
	static SelectionModel::Operation _operation_from_modifiers(const Ref<InputEventWithModifiers> &p_event);
	static SelectionModel::Feature _feature_for_mode(SelectionModel::Mode p_mode);
	static real_t _point_segment_distance(const Vector2 &p_point, const Vector2 &p_a, const Vector2 &p_b, real_t *r_segment_t = nullptr);
	static bool _rect_contains_projected(Camera3D *p_camera, const Rect2 &p_rect, const Vector3 &p_world_position);
	static bool _get_face_loop(const Ref<LevelMeshData> &p_data, int p_face_id, int &r_start, int &r_count);
	static int _find_face_corner(const Ref<LevelMeshData> &p_data, int p_face_id, int p_vertex_id);

	void _set_mode(SelectionModel::Mode p_mode, SelectionModel::Tier p_tier);
	bool _is_xray_enabled() const;
	Vector<SurfaceHit> _query_surface_hits(Camera3D *p_camera, const Vector2 &p_position) const;
	Vector<SelectionModel::Element> _resolve_face(const SurfaceHit &p_hit, bool p_expand_polygroup) const;
	Vector<SelectionModel::Element> _resolve_edge(Camera3D *p_camera, const Vector2 &p_position, const SurfaceHit &p_hit) const;
	Vector<SelectionModel::Element> _resolve_vertex(Camera3D *p_camera, const Vector2 &p_position, const Vector<SurfaceHit> &p_hits) const;
	SelectionModel::Element _make_element(LevelBlock *p_block, SelectionModel::Feature p_feature,
			SelectionModel::HandleKind p_handle_kind, int64_t p_handle, int64_t p_sub_index = -1) const;
	void _apply_elements(SelectionModel::Feature p_feature, const Vector<SelectionModel::Element> &p_elements,
			SelectionModel::Operation p_operation, const StringName &p_action);
	void _apply_object(LevelBlock *p_block, SelectionModel::Operation p_operation);
	void _pick_click(Camera3D *p_camera, const Vector2 &p_position, SelectionModel::Operation p_operation, bool p_double_click);
	void _select_edge_walk(bool p_ring);
	void _select_all(bool p_invert);
	Vector<SelectionModel::Element> _collect_all(SelectionModel::Feature p_feature) const;
	Vector<Plane> _build_screen_frustum(Camera3D *p_camera, const Rect2 &p_rect) const;
	Vector<LevelBlock *> _query_marquee_blocks(Camera3D *p_camera, const Rect2 &p_rect) const;
	void _apply_marquee(Camera3D *p_camera, const Rect2 &p_rect);
	void _apply_object_marquee(Camera3D *p_camera, const Rect2 &p_rect, const Vector<LevelBlock *> &p_blocks);
	bool _press_hits_current_selection(Camera3D *p_camera, const Vector2 &p_position) const;
	bool _collect_transform_selection();
	bool _collect_transform_selection(Vector<MeshDragState> &r_mesh_states, Vector<ObjectDragState> &r_object_states) const;
	bool _compute_transform_pivot(Vector3 &r_pivot) const;
	bool _compute_transform_pivot(const Vector<MeshDragState> &p_mesh_states, const Vector<ObjectDragState> &p_object_states,
			Vector3 &r_pivot) const;
	bool _begin_transform_drag(Camera3D *p_camera, TransformDragMode p_requested_mode = TRANSFORM_DRAG_NONE,
			TransformConstraintMode p_initial_constraint = TRANSFORM_CONSTRAINT_FREE, int p_initial_axis = Vector3::AXIS_X,
			bool p_rotation_commit_on_release = false);
	bool _begin_gizmo_drag(Camera3D *p_camera, TransformGizmo::Handle p_handle, const Ref<InputEventMouseButton> &p_button);
	bool _update_transform_drag(Camera3D *p_camera, const Vector2 &p_position, bool p_ctrl_pressed);
	void _cancel_transform_drag();
	bool _commit_transform_drag();
	bool _is_move_drag() const;
	bool _is_rotation_drag() const;
	bool _is_object_drag() const;
	bool _closest_axis_parameter(Camera3D *p_camera, const Vector2 &p_position, const Vector3 &p_axis, real_t &r_parameter) const;
	bool _rederive_transform_press_reference(Camera3D *p_camera, TransformConstraintMode p_mode, int p_axis);
	bool _cycle_transform_constraint(Camera3D *p_camera, int p_axis);
	void _reset_transform_constraint();
	void _end_transform_drag(bool p_committed);
	bool _resolve_drag_delta(Camera3D *p_camera, const Vector2 &p_position, Vector3 &r_delta) const;
	bool _open_mesh_previews();
	bool _apply_mesh_preview_delta(const Vector3 &p_world_delta);
	bool _apply_rotation_preview(real_t p_angle);
	void _revert_topology_diffs();
	void _register_mesh_undo(const String &p_action_name, const Vector<MeshDragState> &p_states);
	bool _nudge(Camera3D *p_camera, Key p_key);
	bool _vertices_to_grid();
	bool _duplicate_objects();
	bool _collect_face_selection(Vector<MeshDragState> &r_states, LevelBlock *p_only_block = nullptr) const;
	bool _apply_hotspot_fit(bool p_individual);
	bool _handle_texture_key(const Ref<InputEventKey> &p_key);
	bool _pick_texture_face(Camera3D *p_camera, const Vector2 &p_position, LevelBlock *&r_block,
			Ref<LevelMesh> &r_mesh, int &r_face_id) const;
	bool _lift_face_texture(Camera3D *p_camera, const Vector2 &p_position);
	bool _wrap_face_texture(Camera3D *p_camera, const Vector2 &p_position);
	bool _wrap_texture_to_selection(Camera3D *p_camera, const Vector2 &p_position);
	bool _begin_texture_flow(Camera3D *p_camera, const Vector2 &p_position);
	bool _append_texture_flow_hover(Camera3D *p_camera, const Vector2 &p_position);
	bool _commit_texture_flow();
	bool _align_selected_texture(bool p_to_grid);
	void _show_texture_status(const String &p_message, bool p_warning = false) const;
	static Vector3 _nearest_world_axis(const Vector3 &p_direction);

protected:
	static void _bind_methods() {}
	virtual void _activate() override;
	virtual void _deactivate() override;
	virtual bool _handle_input(Camera3D *p_camera, const Ref<InputEvent> &p_event) override;
	virtual bool _commit_gesture() override;
	virtual bool _has_active_gesture() const override { return pointer_down || transform_active || texture_flow_active; }
	virtual void _reset_gesture() override;
	virtual bool _handles_idle_escape() const override { return true; }
	virtual void _escape_pressed() override;

public:
	bool apply_active_material();
	bool modify_selected_texture(int p_operation, const Vector2 &p_value = Vector2(1, 1));
	void update_transform_gizmo(Camera3D *p_camera);
	void set_transform_gizmo_view_visible(bool p_visible);
};
