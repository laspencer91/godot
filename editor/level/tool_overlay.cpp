/**************************************************************************/
/*  tool_overlay.cpp                                                      */
/**************************************************************************/
/*  G-Level S1: create-detached -> reconcile -> free-in-dtor preview RIDs.*/
/**************************************************************************/

#include "tool_overlay.h"

#include "editor/editor_node.h"
#include "editor/editor_string_names.h"
#include "scene/resources/theme.h"
#include "servers/rendering/rendering_server.h"

void ToolOverlay::_sync_visibility() {
	if (instance.is_valid() && RenderingServer::get_singleton()) {
		RenderingServer::get_singleton()->instance_set_visible(instance, has_geometry && view_visible);
	}
}

void ToolOverlay::set_render_layer(int p_layer) {
	ERR_FAIL_INDEX(p_layer, 32);
	render_layer = p_layer;
	if (instance.is_valid() && RenderingServer::get_singleton()) {
		RenderingServer::get_singleton()->instance_set_layer_mask(instance, 1u << render_layer);
	}
}

void ToolOverlay::set_scenario(const RID &p_scenario) {
	scenario = p_scenario;
	if (!instance.is_valid() || !RenderingServer::get_singleton()) {
		return;
	}
	RenderingServer::get_singleton()->instance_set_scenario(instance, scenario);
	_sync_visibility();
}

void ToolOverlay::set_view_visible(bool p_visible) {
	view_visible = p_visible;
	_sync_visibility();
}

void ToolOverlay::update_box(const Transform3D &p_frame, const Vector3 &p_size) {
	ERR_FAIL_COND(!p_frame.is_finite() || !p_size.is_finite());
	ERR_FAIL_COND(p_size.x <= 0.0 || p_size.y <= 0.0 || p_size.z <= 0.0);

	const Vector3 half_size = p_size * (real_t)0.5;
	const Vector3 corners[8] = {
		Vector3(-half_size.x, -half_size.y, -half_size.z),
		Vector3(half_size.x, -half_size.y, -half_size.z),
		Vector3(half_size.x, half_size.y, -half_size.z),
		Vector3(-half_size.x, half_size.y, -half_size.z),
		Vector3(-half_size.x, -half_size.y, half_size.z),
		Vector3(half_size.x, -half_size.y, half_size.z),
		Vector3(half_size.x, half_size.y, half_size.z),
		Vector3(-half_size.x, half_size.y, half_size.z),
	};

	static constexpr int BOX_FACES[6][4] = {
		{ 0, 3, 2, 1 },
		{ 4, 5, 6, 7 },
		{ 0, 4, 7, 3 },
		{ 1, 2, 6, 5 },
		{ 0, 1, 5, 4 },
		{ 3, 7, 6, 2 },
	};
	static constexpr int BOX_EDGES[12][2] = {
		{ 0, 1 },
		{ 1, 2 },
		{ 2, 3 },
		{ 3, 0 },
		{ 4, 5 },
		{ 5, 6 },
		{ 6, 7 },
		{ 7, 4 },
		{ 0, 4 },
		{ 1, 5 },
		{ 2, 6 },
		{ 3, 7 },
	};

	mesh->clear_surfaces();
	mesh->surface_begin(Mesh::PRIMITIVE_TRIANGLES, fill_material);
	for (const auto &face : BOX_FACES) {
		mesh->surface_add_vertex(corners[face[0]]);
		mesh->surface_add_vertex(corners[face[1]]);
		mesh->surface_add_vertex(corners[face[2]]);
		mesh->surface_add_vertex(corners[face[0]]);
		mesh->surface_add_vertex(corners[face[2]]);
		mesh->surface_add_vertex(corners[face[3]]);
	}
	mesh->surface_end();
	mesh->surface_begin(Mesh::PRIMITIVE_POINTS, wire_material);
	for (const Vector3 &corner : corners) {
		mesh->surface_add_vertex(corner);
	}
	mesh->surface_end();

	mesh->surface_begin(Mesh::PRIMITIVE_LINES, wire_material);
	for (const auto &edge : BOX_EDGES) {
		mesh->surface_add_vertex(corners[edge[0]]);
		mesh->surface_add_vertex(corners[edge[1]]);
	}
	mesh->surface_end();

	has_geometry = true;
	if (instance.is_valid() && RenderingServer::get_singleton()) {
		RenderingServer::get_singleton()->instance_set_transform(instance, p_frame);
	}
	_sync_visibility();
}

void ToolOverlay::update_footprint(const Transform3D &p_frame, const Vector2 &p_size) {
	ERR_FAIL_COND(!p_frame.is_finite() || !p_size.is_finite());
	ERR_FAIL_COND(p_size.x <= 0.0 || p_size.y <= 0.0);

	const Vector2 half_size = p_size * (real_t)0.5;
	const Vector3 corners[4] = {
		Vector3(-half_size.x, 0.0, -half_size.y),
		Vector3(half_size.x, 0.0, -half_size.y),
		Vector3(half_size.x, 0.0, half_size.y),
		Vector3(-half_size.x, 0.0, half_size.y),
	};

	mesh->clear_surfaces();
	mesh->surface_begin(Mesh::PRIMITIVE_POINTS, wire_material);
	for (const Vector3 &corner : corners) {
		mesh->surface_add_vertex(corner);
	}
	mesh->surface_end();
	mesh->surface_begin(Mesh::PRIMITIVE_LINES, wire_material);
	for (int edge = 0; edge < 4; edge++) {
		mesh->surface_add_vertex(corners[edge]);
		mesh->surface_add_vertex(corners[(edge + 1) % 4]);
	}
	mesh->surface_end();

	has_geometry = true;
	if (instance.is_valid() && RenderingServer::get_singleton()) {
		RenderingServer::get_singleton()->instance_set_transform(instance, p_frame);
	}
	_sync_visibility();
}

void ToolOverlay::_ensure_axis_materials() {
	if (axis_materials[0].is_valid()) {
		return;
	}
	Color axis_colors[3];
	resolve_axis_colors(axis_colors);
	for (int axis = 0; axis < 3; axis++) {
		axis_materials[axis].instantiate();
		axis_materials[axis]->set_shading_mode(BaseMaterial3D::SHADING_MODE_UNSHADED);
		axis_materials[axis]->set_transparency(BaseMaterial3D::TRANSPARENCY_ALPHA);
		axis_materials[axis]->set_cull_mode(BaseMaterial3D::CULL_DISABLED);
		axis_materials[axis]->set_flag(BaseMaterial3D::FLAG_DISABLE_DEPTH_TEST, true);
		axis_materials[axis]->set_albedo(axis_colors[axis]);
	}
}

void ToolOverlay::resolve_axis_colors(Color r_colors[3]) {
	ERR_FAIL_NULL(r_colors);
	r_colors[0] = Color(0.96, 0.20, 0.32);
	r_colors[1] = Color(0.53, 0.84, 0.01);
	r_colors[2] = Color(0.16, 0.55, 0.96);
	if (EditorNode *editor_node = EditorNode::get_singleton()) {
		const Ref<Theme> editor_theme = editor_node->get_editor_theme();
		if (editor_theme.is_valid()) {
			r_colors[0] = editor_theme->get_color(SNAME("axis_x_color"), EditorStringName(Editor));
			r_colors[1] = editor_theme->get_color(SNAME("axis_y_color"), EditorStringName(Editor));
			r_colors[2] = editor_theme->get_color(SNAME("axis_z_color"), EditorStringName(Editor));
		}
	}
}

void ToolOverlay::update_constraint_guides(const Vector3 &p_pivot, int p_axis, bool p_plane) {
	ERR_FAIL_COND(!p_pivot.is_finite());
	ERR_FAIL_INDEX(p_axis, 3);

	_ensure_axis_materials();
	static constexpr real_t GUIDE_EXTENT = 500.0;
	mesh->clear_surfaces();
	for (int axis = 0; axis < 3; axis++) {
		if ((axis == p_axis) == p_plane) {
			continue;
		}
		Vector3 direction;
		direction[axis] = GUIDE_EXTENT;
		mesh->surface_begin(Mesh::PRIMITIVE_LINES, axis_materials[axis]);
		mesh->surface_add_vertex(-direction);
		mesh->surface_add_vertex(direction);
		mesh->surface_end();
	}

	has_geometry = true;
	if (instance.is_valid() && RenderingServer::get_singleton()) {
		RenderingServer::get_singleton()->instance_set_transform(instance, Transform3D(Basis(), p_pivot));
	}
	_sync_visibility();
}

void ToolOverlay::update_colored_faces(const Vector<Vector3> &p_vertices, const Vector<Color> &p_colors) {
	ERR_FAIL_COND(p_vertices.size() != p_colors.size());
	ERR_FAIL_COND((p_vertices.size() % 3) != 0);
	mesh->clear_surfaces();
	if (p_vertices.is_empty()) {
		has_geometry = false;
		_sync_visibility();
		return;
	}
	for (const Vector3 &vertex : p_vertices) {
		ERR_FAIL_COND(!vertex.is_finite());
	}
	mesh->surface_begin(Mesh::PRIMITIVE_TRIANGLES, vertex_color_material);
	for (int i = 0; i < p_vertices.size(); i++) {
		mesh->surface_set_color(p_colors[i]);
		mesh->surface_add_vertex(p_vertices[i]);
	}
	mesh->surface_end();
	has_geometry = true;
	if (instance.is_valid() && RenderingServer::get_singleton()) {
		RenderingServer::get_singleton()->instance_set_transform(instance, Transform3D());
	}
	_sync_visibility();
}

void ToolOverlay::clear() {
	if (mesh.is_valid()) {
		mesh->clear_surfaces();
	}
	has_geometry = false;
	_sync_visibility();
}

ToolOverlay::ToolOverlay() {
	fill_material.instantiate();
	fill_material->set_shading_mode(BaseMaterial3D::SHADING_MODE_UNSHADED);
	fill_material->set_transparency(BaseMaterial3D::TRANSPARENCY_ALPHA);
	fill_material->set_cull_mode(BaseMaterial3D::CULL_DISABLED);
	fill_material->set_albedo(Color(0.25, 0.62, 1.0, 0.20));

	wire_material.instantiate();
	wire_material->set_shading_mode(BaseMaterial3D::SHADING_MODE_UNSHADED);
	wire_material->set_transparency(BaseMaterial3D::TRANSPARENCY_ALPHA);
	wire_material->set_albedo(Color(0.48, 0.78, 1.0, 0.95));
	wire_material->set_flag(BaseMaterial3D::FLAG_DISABLE_DEPTH_TEST, true);
	wire_material->set_flag(BaseMaterial3D::FLAG_USE_POINT_SIZE, true);
	wire_material->set_point_size(7.0f);

	vertex_color_material.instantiate();
	vertex_color_material->set_shading_mode(BaseMaterial3D::SHADING_MODE_UNSHADED);
	vertex_color_material->set_transparency(BaseMaterial3D::TRANSPARENCY_ALPHA);
	vertex_color_material->set_cull_mode(BaseMaterial3D::CULL_DISABLED);
	vertex_color_material->set_flag(BaseMaterial3D::FLAG_DISABLE_DEPTH_TEST, true);
	vertex_color_material->set_flag(BaseMaterial3D::FLAG_ALBEDO_FROM_VERTEX_COLOR, true);

	mesh.instantiate();

	RenderingServer *rs = RenderingServer::get_singleton();
	ERR_FAIL_NULL(rs);
	// The instance intentionally remains detached until set_scenario() reconciles it.
	instance = rs->instance_create();
	rs->instance_set_base(instance, mesh->get_rid());
	rs->instance_set_layer_mask(instance, 1u << render_layer);
	rs->instance_geometry_set_cast_shadows_setting(instance, RSE::SHADOW_CASTING_SETTING_OFF);
	rs->instance_geometry_set_flag(instance, RSE::INSTANCE_FLAG_IGNORE_OCCLUSION_CULLING, true);
	rs->instance_geometry_set_flag(instance, RSE::INSTANCE_FLAG_USE_BAKED_LIGHT, false);
	rs->instance_set_visible(instance, false);
}

ToolOverlay::~ToolOverlay() {
	if (instance.is_valid()) {
		if (RenderingServer *rs = RenderingServer::get_singleton()) {
			rs->instance_set_scenario(instance, RID());
			rs->free_rid(instance);
		}
		instance = RID();
	}
	scenario = RID();
}
