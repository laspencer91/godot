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
#include "editor/gui/editor_edit_domain.h"

struct CSGPushPullResult {
	Vector3 size;
	Transform3D transform;
};

CSGPushPullResult csg_push_pull_apply(const Vector3 &p_start_size, const Transform3D &p_start_transform, uint32_t p_semantic_surface, real_t p_displacement, bool p_symmetric);

class CSGSurfaceSession : public EditorEditDomainSession {
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
	bool entered = false;
	bool surface_tool_active = true;
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
	bool has_ghost = false;

	ObjectID distance_label_id;
	ObjectID coordinate_edit_id;

	void _resolve_active_root(const EditorEditDomainContext &p_context);
	CSGShape3D *_get_active_root() const;
	CSGBox3D *_get_active_box() const;
	Node3DEditorViewport *_get_active_viewport() const;
	void _clear_pick_state();
	void _clear_selection();
	bool _pick(Node3DEditorViewport *p_viewport, const Vector2 &p_position);
	void _queue_redraw(Node3DEditorViewport *p_viewport) const;
	void _draw_hover(Node3DEditorViewport *p_viewport) const;
	void _draw_ghost(Node3DEditorViewport *p_viewport) const;
	bool _begin_gesture(Node3DEditorViewport *p_viewport, const Ref<InputEventMouseButton> &p_event);
	void _update_drag(Node3DEditorViewport *p_viewport, const Vector2 &p_position);
	void _cancel_gesture();
	void _finish_without_commit();
	void _commit_gesture();
	void _update_context_panel();
	void _numeric_coordinate_submitted(const String &p_text);

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
};

class CSGEditDomainProvider : public EditorEditDomainProvider {
public:
	virtual StringName get_domain_id() const override;
	virtual bool is_available(const EditorEditDomainContext &p_context) const override;
	virtual bool can_activate_from_double_click(const EditorEditDomainContext &p_context, ObjectID p_hit) const override;
	virtual EditorEditDomainSession *create_session(const EditorEditDomainContext &p_context) const override;
};
