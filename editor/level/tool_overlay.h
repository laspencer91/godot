/**************************************************************************/
/*  tool_overlay.h                                                        */
/**************************************************************************/
/*  G-Level S1: render-only transient geometry for modal tool previews.   */
/*  This helper deliberately has no dependency on modules/level_kernel.   */
/**************************************************************************/

#pragma once

#include "core/math/transform_3d.h"
#include "core/templates/rid.h"
#include "core/templates/vector.h"
#include "scene/resources/immediate_mesh.h"
#include "scene/resources/material.h"

class ToolOverlay {
	Ref<ImmediateMesh> mesh;
	Ref<StandardMaterial3D> fill_material;
	Ref<StandardMaterial3D> wire_material;
	Ref<StandardMaterial3D> vertex_color_material;
	Ref<StandardMaterial3D> axis_materials[3];
	RID instance;
	RID scenario;
	int render_layer = 20;
	bool has_geometry = false;
	bool view_visible = false;

	void _sync_visibility();
	void _ensure_axis_materials();

public:
	static void resolve_axis_colors(Color r_colors[3]);

	void set_render_layer(int p_layer);
	void set_scenario(const RID &p_scenario);
	void set_view_visible(bool p_visible);
	void update_box(const Transform3D &p_frame, const Vector3 &p_size);
	void update_footprint(const Transform3D &p_frame, const Vector2 &p_size);
	void update_constraint_guides(const Vector3 &p_pivot, int p_axis, bool p_plane);
	// WP21: world-space translucent triangles with one color per vertex. Used
	// by the pane-local hotspot preview/debug overlays without touching meshes.
	void update_colored_faces(const Vector<Vector3> &p_vertices, const Vector<Color> &p_colors);
	void clear();
	bool has_geometry_state() const { return has_geometry; }

	ToolOverlay();
	~ToolOverlay();
};
