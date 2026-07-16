/**************************************************************************/
/*  transform_gizmo.h                                                     */
/**************************************************************************/
/*  G-Level LE1: persistent world-axis move and rotation handles.         */
/**************************************************************************/

#pragma once

#include "core/math/transform_3d.h"
#include "core/templates/rid.h"
#include "scene/resources/material.h"
#include "scene/resources/mesh.h"

class Camera3D;

class TransformGizmo {
public:
	enum Handle {
		HANDLE_NONE = -1,
		HANDLE_MOVE_X,
		HANDLE_MOVE_Y,
		HANDLE_MOVE_Z,
		HANDLE_PLANE_YZ,
		HANDLE_PLANE_XZ,
		HANDLE_PLANE_XY,
		HANDLE_ROTATE_X,
		HANDLE_ROTATE_Y,
		HANDLE_ROTATE_Z,
		HANDLE_COUNT,
	};

private:
	Ref<ArrayMesh> handle_meshes[HANDLE_COUNT];
	Ref<StandardMaterial3D> handle_materials[HANDLE_COUNT];
	Ref<StandardMaterial3D> hover_materials[HANDLE_COUNT];
	RID instances[HANDLE_COUNT];
	RID scenario;
	int render_layer = 20;
	Handle hovered_handle = HANDLE_NONE;
	Vector3 pivot;
	real_t gizmo_scale = 0.0;
	real_t screen_pixels_per_unit = 0.0;
	bool placed = false;
	bool requested_visible = false;
	bool view_visible = false;

	void _build_materials();
	void _build_meshes();
	void _create_instances();
	void _sync_visibility();
	void _sync_materials();
	real_t _compute_screen_scale(Camera3D *p_camera, const Vector3 &p_pivot, real_t &r_pixels_per_unit) const;

public:
	static bool is_move_axis_handle(Handle p_handle);
	static bool is_move_plane_handle(Handle p_handle);
	static bool is_rotate_handle(Handle p_handle);
	static int get_handle_axis(Handle p_handle);
	static real_t get_arrow_grab_offset();
	static real_t get_plane_center_offset();
	static real_t get_ring_radius();

	void set_render_layer(int p_layer);
	void set_scenario(const RID &p_scenario);
	void set_view_visible(bool p_visible);
	void set_requested_visible(bool p_visible);
	void update(Camera3D *p_camera, const Vector3 &p_pivot);
	Handle hit_test(Camera3D *p_camera, const Vector2 &p_screen_position) const;
	void set_hovered_handle(Handle p_handle);

	bool is_visible() const;
	const Vector3 &get_pivot() const { return pivot; }
	real_t get_scale() const { return gizmo_scale; }

	TransformGizmo();
	~TransformGizmo();
};
