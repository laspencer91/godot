/**************************************************************************/
/*  csg_edit_domain.h                                                     */
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

#pragma once

#include "../csg_shape.h"

#include "core/math/triangle_mesh.h"
#include "core/object/undo_redo.h"
#include "editor/gui/editor_edit_domain.h"

struct CSGPushPullResult {
	Vector3 size;
	Transform3D transform;
};

CSGPushPullResult csg_push_pull_apply(const Vector3 &p_start_size, const Transform3D &p_start_transform, uint32_t p_semantic_surface, real_t p_displacement, bool p_symmetric);
Vector2 csg_texture_lock_compensate_offset(const CSGPrimitive3D *p_primitive, uint32_t p_semantic_surface, const CSGSurfaceSetting &p_setting, const Transform3D &p_operand_to_root, const Vector3 &p_center_shift_root);

// CSG-5: Deterministic box operand produced by an outward face extrusion.
struct CSGExtrusionResult {
	Vector3 size;
	Transform3D local_transform;
};

CSGExtrusionResult csg_extrude_box_face(const Vector3 &p_source_size, uint32_t p_semantic_surface, real_t p_depth);
void csg_configure_extrusion_surface_settings(CSGBox3D *p_source, uint32_t p_source_surface, const Transform3D &p_source_to_root, CSGBox3D *r_extrusion);
bool csg_paint_surfaces_with_undo(UndoRedo *p_undo_redo, CSGShape3D *p_root, Node *p_edited_root, const Vector<CSGSurfaceKey> &p_surfaces, const CSGSurfaceSetting &p_setting, UndoRedo::MergeMode p_merge_mode = UndoRedo::MERGE_DISABLE, const String &p_action_name = "CSG Paint Surfaces");

class CSGSurfaceSession : public EditorEditDomainSession {
public:
	enum class ToolMode {
		SURFACE,
		PAINT,
		OPERAND,
	};

private:
	enum class GestureState {
		IDLE,
		HOVER,
		PRESSED,
		DRAGGING,
		COMMIT,
		CANCEL,
	};
	enum class SnapSpace {
		LOCAL,
		ROOT,
		WORLD,
	};

	ObjectID active_root_id;
	ObjectID active_box_id;
	ObjectID active_viewport_id;
	ObjectID edited_scene_root_id;
	bool entered = false;
	ToolMode tool_mode = ToolMode::SURFACE;
	SnapSpace snap_space = SnapSpace::LOCAL;
	Ref<TriangleMesh> pick_mesh;
	Vector<Vector3> pick_faces;
	uint64_t pick_mesh_generation = UINT64_MAX;
	CSGSurfaceHit hover_hit;
	bool has_hover = false;
	CSGSurfaceHit selected_hit;
	bool has_selection = false;
	GestureState gesture_state = GestureState::IDLE;

	Vector2 press_position;
	bool symmetric_drag = false;
	bool extrude_gesture = false; // CSG-5: Captured from Shift at press.
	Vector3 start_size;
	Transform3D start_transform;
	Transform3D start_global_transform;
	int drag_axis = 0;
	real_t drag_axis_sign = 1.0;
	real_t start_plane_coordinate = 0.0;
	real_t target_plane_coordinate = 0.0;
	Vector3 drag_line_origin_world;
	Vector3 drag_line_direction_world;
	real_t drag_axis_world_scale = 1.0;
	real_t drag_start_parameter = 0.0;
	real_t drag_displacement = 0.0;
	CSGPushPullResult ghost_result;
	CSGExtrusionResult extrude_ghost; // CSG-5: View-only child prism during drag.
	bool has_ghost = false;
	CSGSurfaceSetting paint_well;
	Vector<CSGSurfaceKey> paint_selection;
	bool paint_eyedropper_active = false;
	bool updating_paint_controls = false;

	ObjectID distance_label_id;
	ObjectID coordinate_edit_id;
	ObjectID surface_context_id;
	ObjectID paint_context_id;
	ObjectID surface_tool_button_id;
	ObjectID paint_tool_button_id;
	ObjectID operand_tool_button_id;
	ObjectID paint_material_picker_id;
	ObjectID paint_uv_mode_id;
	ObjectID paint_uv_space_id;
	ObjectID paint_meters_u_id;
	ObjectID paint_meters_v_id;
	ObjectID paint_offset_u_id;
	ObjectID paint_offset_v_id;
	ObjectID paint_rotation_id;
	ObjectID paint_texture_lock_id;
	ObjectID paint_selection_label_id;
	ObjectID paint_eyedropper_button_id;

	void _resolve_active_root(const EditorEditDomainContext &p_context);
	void _capture_edited_scene_root(const EditorEditDomainContext &p_context);
	CSGShape3D *_get_active_root() const;
	CSGBox3D *_get_active_box() const;
	Node3DEditorViewport *_get_active_viewport() const;
	Node *_get_edited_scene_root() const;
	bool _is_source_editable(const CSGPrimitive3D *p_source) const;
	void _clear_pick_state();
	void _clear_selection();
	void _set_tool_mode(ToolMode p_mode);
	void _update_tool_buttons();
	bool _pick(Node3DEditorViewport *p_viewport, const Vector2 &p_position);
	void _queue_redraw(Node3DEditorViewport *p_viewport) const;
	void _draw_hover(Node3DEditorViewport *p_viewport) const;
	void _draw_paint_selection(Node3DEditorViewport *p_viewport) const;
	void _draw_ghost(Node3DEditorViewport *p_viewport) const;
	bool _begin_gesture(Node3DEditorViewport *p_viewport, const Ref<InputEventMouseButton> &p_event);
	void _update_drag(Node3DEditorViewport *p_viewport, const Vector2 &p_position);
	void _apply_displacement(); // CSG-4: Shared post-clamp push/pull recompute.
	void _cancel_gesture();
	void _finish_without_commit();
	void _commit_gesture();
	void _update_context_panel();
	void _numeric_coordinate_submitted(const String &p_text);
	void _prune_paint_selection();
	bool _paint_selection_has(const CSGSurfaceKey &p_surface) const;
	void _select_paint_surface(const CSGSurfaceKey &p_surface, bool p_add);
	bool _apply_paint_to_surfaces(const Vector<CSGSurfaceKey> &p_surfaces, UndoRedo::MergeMode p_merge_mode = UndoRedo::MERGE_DISABLE, const String &p_action_name = "CSG Paint Surfaces");
	bool _lift_paint_setting(const CSGSurfaceKey &p_surface);
	void _update_paint_controls();
	void _apply_well_to_selection(UndoRedo::MergeMode p_merge_mode, const String &p_action_name);
	void _surface_tool_pressed();
	void _paint_tool_pressed();
	void _operand_tool_pressed();
	void _paint_material_changed(Ref<Resource> p_resource);
	void _paint_uv_mode_selected(int p_index);
	void _paint_uv_space_selected(int p_index);
	void _paint_numeric_changed(double p_value);
	void _paint_texture_lock_toggled(bool p_pressed);
	void _paint_assign_pressed();
	void _paint_eyedropper_toggled(bool p_pressed);
	void _paint_align_face_pressed();
	void _paint_align_root_pressed();
	void _paint_fit_pressed();
	void _paint_reset_pressed();
	void _paint_apply_selected_pressed();

public:
	virtual void enter(const EditorEditDomainContext &p_context) override;
	virtual void exit() override;
	virtual void retarget(const EditorEditDomainContext &p_context) override;
	virtual EditorEditDomainInput handle_input(const EditorEditDomainContext &p_context, Camera3D *p_camera, const Ref<InputEvent> &p_event) override;
	virtual bool handle_escape() override;
	virtual bool handle_tool_toggle() override;
	virtual void draw_overlay(Node3DEditorViewport *p_viewport) override;
	virtual Control *build_tool_rail() override;
	virtual Control *build_contextual_panel() override;

	ObjectID get_active_root_id() const { return active_root_id; }
	ToolMode get_tool_mode() const { return tool_mode; }
	const CSGSurfaceSetting &get_paint_well() const { return paint_well; }
	bool lift_paint_setting(const CSGSurfaceKey &p_surface) { return _lift_paint_setting(p_surface); }
	void select_paint_surface(const CSGSurfaceKey &p_surface, bool p_add) { _select_paint_surface(p_surface, p_add); }
	const Vector<CSGSurfaceKey> &get_paint_selection() const { return paint_selection; }
};

class CSGEditDomainProvider : public EditorEditDomainProvider {
public:
	virtual StringName get_domain_id() const override;
	virtual bool is_available(const EditorEditDomainContext &p_context) const override;
	virtual bool can_activate_from_double_click(const EditorEditDomainContext &p_context, ObjectID p_hit) const override;
	virtual EditorEditDomainSession *create_session(const EditorEditDomainContext &p_context) const override;
};
