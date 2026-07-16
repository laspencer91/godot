/**************************************************************************/
/*  transform_gizmo.cpp                                                   */
/**************************************************************************/
/*  G-Level LE1: persistent meshes with detached/reconciled render RIDs.  */
/**************************************************************************/

#include "transform_gizmo.h"

#include "core/math/geometry_3d.h"
#include "editor/level/tool_overlay.h"
#include "editor/themes/editor_scale.h"
#include "scene/3d/camera_3d.h"
#include "scene/main/viewport.h"
#include "scene/resources/surface_tool.h"
#include "servers/rendering/rendering_server.h"

namespace {

constexpr real_t GIZMO_SIZE_PIXELS = 80.0;
constexpr real_t GIZMO_ARROW_OFFSET = 1.4;
constexpr real_t GIZMO_ARROW_SIZE = 0.35;
constexpr real_t GIZMO_ARROW_SHAFT_RADIUS = 0.012;
constexpr real_t GIZMO_ARROW_HEAD_RADIUS = 0.07;
constexpr real_t GIZMO_ARROW_HIT_START = 0.55;
constexpr real_t GIZMO_PLANE_DST = 0.3;
constexpr real_t GIZMO_PLANE_SIZE = 0.2;
constexpr real_t GIZMO_RING_RADIUS = 1.1;
constexpr real_t GIZMO_RING_TUBE_RADIUS = 0.026;
constexpr real_t GIZMO_HIT_TOLERANCE_PIXELS = 6.0;
constexpr real_t GIZMO_CENTER_DEAD_ZONE_PIXELS = 18.0;

Vector3 axis_vector(int p_axis) {
	Vector3 axis;
	axis[p_axis] = 1.0;
	return axis;
}

void perpendicular_vectors(int p_axis, Vector3 &r_u, Vector3 &r_v) {
	r_u = axis_vector((p_axis + 1) % 3);
	r_v = axis_vector((p_axis + 2) % 3);
}

void add_triangle(const Ref<SurfaceTool> &p_surface, const Vector3 &p_a, const Vector3 &p_b, const Vector3 &p_c) {
	p_surface->add_vertex(p_a);
	p_surface->add_vertex(p_b);
	p_surface->add_vertex(p_c);
}

Ref<ArrayMesh> build_arrow_mesh(int p_axis) {
	Ref<SurfaceTool> surface;
	surface.instantiate();
	surface->begin(Mesh::PRIMITIVE_TRIANGLES);

	const Vector3 direction = axis_vector(p_axis);
	Vector3 radial_u;
	Vector3 radial_v;
	perpendicular_vectors(p_axis, radial_u, radial_v);
	constexpr int SEGMENTS = 16;
	for (int segment = 0; segment < SEGMENTS; segment++) {
		const real_t angle_a = Math::TAU * segment / SEGMENTS;
		const real_t angle_b = Math::TAU * (segment + 1) / SEGMENTS;
		const Vector3 radius_a = radial_u * Math::cos(angle_a) + radial_v * Math::sin(angle_a);
		const Vector3 radius_b = radial_u * Math::cos(angle_b) + radial_v * Math::sin(angle_b);
		const Vector3 shaft_a0 = radius_a * GIZMO_ARROW_SHAFT_RADIUS;
		const Vector3 shaft_b0 = radius_b * GIZMO_ARROW_SHAFT_RADIUS;
		const Vector3 shaft_a1 = direction * GIZMO_ARROW_OFFSET + shaft_a0;
		const Vector3 shaft_b1 = direction * GIZMO_ARROW_OFFSET + shaft_b0;
		add_triangle(surface, shaft_a0, shaft_a1, shaft_b1);
		add_triangle(surface, shaft_a0, shaft_b1, shaft_b0);

		const Vector3 cone_a = direction * GIZMO_ARROW_OFFSET + radius_a * GIZMO_ARROW_HEAD_RADIUS;
		const Vector3 cone_b = direction * GIZMO_ARROW_OFFSET + radius_b * GIZMO_ARROW_HEAD_RADIUS;
		const Vector3 tip = direction * (GIZMO_ARROW_OFFSET + GIZMO_ARROW_SIZE);
		add_triangle(surface, cone_a, tip, cone_b);
	}
	return surface->commit();
}

Ref<ArrayMesh> build_plane_mesh(int p_normal_axis) {
	Ref<SurfaceTool> surface;
	surface.instantiate();
	surface->begin(Mesh::PRIMITIVE_TRIANGLES);
	Vector3 u;
	Vector3 v;
	perpendicular_vectors(p_normal_axis, u, v);
	const Vector3 p0 = (u + v) * GIZMO_PLANE_DST;
	const Vector3 p1 = u * (GIZMO_PLANE_DST + GIZMO_PLANE_SIZE) + v * GIZMO_PLANE_DST;
	const Vector3 p2 = (u + v) * (GIZMO_PLANE_DST + GIZMO_PLANE_SIZE);
	const Vector3 p3 = u * GIZMO_PLANE_DST + v * (GIZMO_PLANE_DST + GIZMO_PLANE_SIZE);
	add_triangle(surface, p0, p1, p2);
	add_triangle(surface, p0, p2, p3);
	return surface->commit();
}

Ref<ArrayMesh> build_ring_mesh(int p_axis) {
	Ref<SurfaceTool> surface;
	surface.instantiate();
	surface->begin(Mesh::PRIMITIVE_TRIANGLES);
	Vector3 u;
	Vector3 v;
	perpendicular_vectors(p_axis, u, v);
	const Vector3 normal = axis_vector(p_axis);
	constexpr int RING_SEGMENTS = 96;
	constexpr int TUBE_SEGMENTS = 6;
	for (int ring_segment = 0; ring_segment < RING_SEGMENTS; ring_segment++) {
		const real_t ring_a = Math::TAU * ring_segment / RING_SEGMENTS;
		const real_t ring_b = Math::TAU * (ring_segment + 1) / RING_SEGMENTS;
		const Vector3 radial_a = u * Math::cos(ring_a) + v * Math::sin(ring_a);
		const Vector3 radial_b = u * Math::cos(ring_b) + v * Math::sin(ring_b);
		const Vector3 center_a = radial_a * GIZMO_RING_RADIUS;
		const Vector3 center_b = radial_b * GIZMO_RING_RADIUS;
		for (int tube_segment = 0; tube_segment < TUBE_SEGMENTS; tube_segment++) {
			const real_t tube_a = Math::TAU * tube_segment / TUBE_SEGMENTS;
			const real_t tube_b = Math::TAU * (tube_segment + 1) / TUBE_SEGMENTS;
			const Vector3 a0 = center_a + (radial_a * Math::cos(tube_a) + normal * Math::sin(tube_a)) * GIZMO_RING_TUBE_RADIUS;
			const Vector3 a1 = center_a + (radial_a * Math::cos(tube_b) + normal * Math::sin(tube_b)) * GIZMO_RING_TUBE_RADIUS;
			const Vector3 b0 = center_b + (radial_b * Math::cos(tube_a) + normal * Math::sin(tube_a)) * GIZMO_RING_TUBE_RADIUS;
			const Vector3 b1 = center_b + (radial_b * Math::cos(tube_b) + normal * Math::sin(tube_b)) * GIZMO_RING_TUBE_RADIUS;
			add_triangle(surface, a0, b0, b1);
			add_triangle(surface, a0, b1, a1);
		}
	}
	return surface->commit();
}

Ref<StandardMaterial3D> make_handle_material(const Color &p_color) {
	Ref<StandardMaterial3D> material;
	material.instantiate();
	material->set_shading_mode(BaseMaterial3D::SHADING_MODE_UNSHADED);
	material->set_transparency(BaseMaterial3D::TRANSPARENCY_ALPHA);
	material->set_cull_mode(BaseMaterial3D::CULL_DISABLED);
	material->set_flag(BaseMaterial3D::FLAG_DISABLE_DEPTH_TEST, true);
	material->set_flag(BaseMaterial3D::FLAG_DISABLE_FOG, true);
	material->set_render_priority(Material::RENDER_PRIORITY_MAX);
	material->set_albedo(p_color);
	return material;
}

} // namespace

bool TransformGizmo::is_move_axis_handle(Handle p_handle) {
	return p_handle >= HANDLE_MOVE_X && p_handle <= HANDLE_MOVE_Z;
}

bool TransformGizmo::is_move_plane_handle(Handle p_handle) {
	return p_handle >= HANDLE_PLANE_YZ && p_handle <= HANDLE_PLANE_XY;
}

bool TransformGizmo::is_rotate_handle(Handle p_handle) {
	return p_handle >= HANDLE_ROTATE_X && p_handle <= HANDLE_ROTATE_Z;
}

int TransformGizmo::get_handle_axis(Handle p_handle) {
	if (is_move_axis_handle(p_handle)) {
		return int(p_handle) - int(HANDLE_MOVE_X);
	}
	if (is_move_plane_handle(p_handle)) {
		return int(p_handle) - int(HANDLE_PLANE_YZ);
	}
	if (is_rotate_handle(p_handle)) {
		return int(p_handle) - int(HANDLE_ROTATE_X);
	}
	return -1;
}

real_t TransformGizmo::get_arrow_grab_offset() {
	return GIZMO_ARROW_OFFSET + GIZMO_ARROW_SIZE * (real_t)0.5;
}

real_t TransformGizmo::get_plane_center_offset() {
	return GIZMO_PLANE_DST + GIZMO_PLANE_SIZE * (real_t)0.5;
}

real_t TransformGizmo::get_ring_radius() {
	return GIZMO_RING_RADIUS;
}

void TransformGizmo::_build_materials() {
	Color axis_colors[3];
	ToolOverlay::resolve_axis_colors(axis_colors);
	for (int handle = 0; handle < HANDLE_COUNT; handle++) {
		const int axis = get_handle_axis(Handle(handle));
		Color normal = axis_colors[axis];
		Color highlighted = normal.lerp(Color(1.0, 1.0, 1.0, normal.a), 0.58);
		if (is_move_plane_handle(Handle(handle))) {
			normal.a = 0.32;
			highlighted.a = 0.72;
		} else {
			normal.a = 0.96;
			highlighted.a = 1.0;
		}
		handle_materials[handle] = make_handle_material(normal);
		hover_materials[handle] = make_handle_material(highlighted);
	}
}

void TransformGizmo::_build_meshes() {
	for (int axis = 0; axis < 3; axis++) {
		handle_meshes[HANDLE_MOVE_X + axis] = build_arrow_mesh(axis);
		handle_meshes[HANDLE_PLANE_YZ + axis] = build_plane_mesh(axis);
		handle_meshes[HANDLE_ROTATE_X + axis] = build_ring_mesh(axis);
	}
}

void TransformGizmo::_create_instances() {
	RenderingServer *rs = RenderingServer::get_singleton();
	ERR_FAIL_NULL(rs);
	for (int handle = 0; handle < HANDLE_COUNT; handle++) {
		ERR_CONTINUE(handle_meshes[handle].is_null());
		instances[handle] = rs->instance_create();
		rs->instance_set_base(instances[handle], handle_meshes[handle]->get_rid());
		rs->instance_set_layer_mask(instances[handle], 1u << render_layer);
		rs->instance_geometry_set_cast_shadows_setting(instances[handle], RSE::SHADOW_CASTING_SETTING_OFF);
		rs->instance_geometry_set_flag(instances[handle], RSE::INSTANCE_FLAG_IGNORE_OCCLUSION_CULLING, true);
		rs->instance_geometry_set_flag(instances[handle], RSE::INSTANCE_FLAG_USE_BAKED_LIGHT, false);
		rs->instance_geometry_set_material_override(instances[handle], handle_materials[handle]->get_rid());
		rs->instance_set_visible(instances[handle], false);
	}
}

void TransformGizmo::_sync_visibility() {
	RenderingServer *rs = RenderingServer::get_singleton();
	if (!rs) {
		return;
	}
	const bool visible = is_visible();
	for (const RID &instance : instances) {
		if (instance.is_valid()) {
			rs->instance_set_visible(instance, visible);
		}
	}
}

void TransformGizmo::_sync_materials() {
	RenderingServer *rs = RenderingServer::get_singleton();
	if (!rs) {
		return;
	}
	for (int handle = 0; handle < HANDLE_COUNT; handle++) {
		if (!instances[handle].is_valid()) {
			continue;
		}
		const Ref<StandardMaterial3D> &material = handle == hovered_handle ? hover_materials[handle] : handle_materials[handle];
		rs->instance_geometry_set_material_override(instances[handle], material->get_rid());
	}
}

real_t TransformGizmo::_compute_screen_scale(Camera3D *p_camera, const Vector3 &p_pivot, real_t &r_pixels_per_unit) const {
	r_pixels_per_unit = 0.0;
	if (!p_camera || !p_camera->get_viewport() || p_camera->is_position_behind(p_pivot)) {
		return 0.0;
	}
	const real_t viewport_height = p_camera->get_viewport()->get_visible_rect().size.y;
	if (viewport_height <= 1.0) {
		return 0.0;
	}

	real_t world_per_pixel = 0.0;
	if (p_camera->get_projection() == Camera3D::PROJECTION_PERSPECTIVE) {
		const Vector3 camera_forward = -p_camera->get_global_transform().basis.get_column(2).normalized();
		const real_t depth = (p_pivot - p_camera->get_global_position()).dot(camera_forward);
		if (depth <= p_camera->get_near()) {
			return 0.0;
		}
		world_per_pixel = 2.0 * depth * Math::tan(Math::deg_to_rad(p_camera->get_fov()) * (real_t)0.5) / viewport_height;
	} else {
		world_per_pixel = p_camera->get_size() / viewport_height;
	}

	const real_t editor_scale = MAX((real_t)1.0, (real_t)EDSCALE);
	const real_t viewport_base_height = 400.0 * editor_scale;
	const real_t viewport_factor = MIN(viewport_base_height, viewport_height) / viewport_base_height;
	r_pixels_per_unit = GIZMO_SIZE_PIXELS * editor_scale * viewport_factor;
	return world_per_pixel * r_pixels_per_unit;
}

void TransformGizmo::set_render_layer(int p_layer) {
	ERR_FAIL_INDEX(p_layer, 32);
	render_layer = p_layer;
	if (RenderingServer *rs = RenderingServer::get_singleton()) {
		for (const RID &instance : instances) {
			if (instance.is_valid()) {
				rs->instance_set_layer_mask(instance, 1u << render_layer);
			}
		}
	}
}

void TransformGizmo::set_scenario(const RID &p_scenario) {
	scenario = p_scenario;
	if (RenderingServer *rs = RenderingServer::get_singleton()) {
		for (const RID &instance : instances) {
			if (instance.is_valid()) {
				rs->instance_set_scenario(instance, scenario);
			}
		}
	}
	_sync_visibility();
}

void TransformGizmo::set_view_visible(bool p_visible) {
	view_visible = p_visible;
	_sync_visibility();
}

void TransformGizmo::set_requested_visible(bool p_visible) {
	requested_visible = p_visible;
	if (!requested_visible) {
		set_hovered_handle(HANDLE_NONE);
	}
	_sync_visibility();
}

void TransformGizmo::update(Camera3D *p_camera, const Vector3 &p_pivot) {
	pivot = p_pivot;
	gizmo_scale = _compute_screen_scale(p_camera, pivot, screen_pixels_per_unit);
	placed = gizmo_scale > CMP_EPSILON && Math::is_finite(gizmo_scale) && pivot.is_finite();
	if (placed) {
		const Transform3D transform(Basis().scaled(Vector3(gizmo_scale, gizmo_scale, gizmo_scale)), pivot);
		if (RenderingServer *rs = RenderingServer::get_singleton()) {
			for (const RID &instance : instances) {
				if (instance.is_valid()) {
					rs->instance_set_transform(instance, transform);
				}
			}
		}
	}
	_sync_visibility();
}

TransformGizmo::Handle TransformGizmo::hit_test(Camera3D *p_camera, const Vector2 &p_screen_position) const {
	if (!is_visible() || !p_camera || screen_pixels_per_unit <= CMP_EPSILON) {
		return HANDLE_NONE;
	}
	const real_t editor_scale = MAX((real_t)1.0, (real_t)EDSCALE);
	if (p_camera->unproject_position(pivot).distance_to(p_screen_position) <= GIZMO_CENTER_DEAD_ZONE_PIXELS * editor_scale) {
		// Several world axes can collapse onto the pivot when viewed end-on. Keep
		// that ambiguous screen-space junction available to normal selection.
		return HANDLE_NONE;
	}
	const Vector3 ray_origin = p_camera->project_ray_origin(p_screen_position);
	const Vector3 ray_direction = p_camera->project_ray_normal(p_screen_position);
	const real_t ray_length = MAX(p_camera->get_far(), ray_origin.distance_to(pivot) + gizmo_scale * 4.0);
	const Vector3 ray_end = ray_origin + ray_direction * ray_length;
	const real_t tolerance = GIZMO_HIT_TOLERANCE_PIXELS * editor_scale / screen_pixels_per_unit;

	Handle closest = HANDLE_NONE;
	real_t closest_depth = Math::INF;
	for (int axis = 0; axis < 3; axis++) {
		const Vector3 direction = axis_vector(axis);
		const Vector3 segment_start = pivot + direction * (gizmo_scale * GIZMO_ARROW_HIT_START);
		const Vector3 segment_end = pivot + direction * (gizmo_scale * (GIZMO_ARROW_OFFSET + GIZMO_ARROW_SIZE));
		Vector3 closest_ray;
		Vector3 closest_handle;
		Geometry3D::get_closest_points_between_segments(ray_origin, ray_end, segment_start, segment_end, closest_ray, closest_handle);
		const real_t radius = gizmo_scale * (GIZMO_ARROW_HEAD_RADIUS + tolerance);
		const real_t depth = ray_origin.distance_to(closest_ray);
		if (closest_ray.distance_squared_to(closest_handle) <= radius * radius && depth < closest_depth) {
			closest = Handle(HANDLE_MOVE_X + axis);
			closest_depth = depth;
		}
	}
	if (closest != HANDLE_NONE) {
		return closest;
	}

	for (int normal_axis = 0; normal_axis < 3; normal_axis++) {
		const Vector3 normal = axis_vector(normal_axis);
		const Plane plane(normal, pivot);
		Vector3 intersection;
		if (!plane.intersects_ray(ray_origin, ray_direction, &intersection)) {
			continue;
		}
		Vector3 u;
		Vector3 v;
		perpendicular_vectors(normal_axis, u, v);
		const Vector3 local = (intersection - pivot) / gizmo_scale;
		const real_t u_distance = local.dot(u);
		const real_t v_distance = local.dot(v);
		if (u_distance < GIZMO_PLANE_DST - tolerance || u_distance > GIZMO_PLANE_DST + GIZMO_PLANE_SIZE + tolerance ||
				v_distance < GIZMO_PLANE_DST - tolerance || v_distance > GIZMO_PLANE_DST + GIZMO_PLANE_SIZE + tolerance) {
			continue;
		}
		const real_t depth = ray_origin.distance_to(intersection);
		if (depth < closest_depth) {
			closest = Handle(HANDLE_PLANE_YZ + normal_axis);
			closest_depth = depth;
		}
	}
	if (closest != HANDLE_NONE) {
		return closest;
	}

	for (int axis = 0; axis < 3; axis++) {
		const Plane ring_plane(axis_vector(axis), pivot);
		Vector3 intersection;
		if (!ring_plane.intersects_ray(ray_origin, ray_direction, &intersection)) {
			continue;
		}
		const real_t radial_distance = intersection.distance_to(pivot) / gizmo_scale;
		if (Math::abs(radial_distance - GIZMO_RING_RADIUS) > GIZMO_RING_TUBE_RADIUS + tolerance) {
			continue;
		}
		const real_t depth = ray_origin.distance_to(intersection);
		if (depth < closest_depth) {
			closest = Handle(HANDLE_ROTATE_X + axis);
			closest_depth = depth;
		}
	}
	return closest;
}

void TransformGizmo::set_hovered_handle(Handle p_handle) {
	if (p_handle < HANDLE_NONE || p_handle >= HANDLE_COUNT || hovered_handle == p_handle) {
		return;
	}
	hovered_handle = p_handle;
	_sync_materials();
}

bool TransformGizmo::is_visible() const {
	return requested_visible && view_visible && placed && scenario.is_valid();
}

TransformGizmo::TransformGizmo() {
	_build_materials();
	_build_meshes();
	_create_instances();
}

TransformGizmo::~TransformGizmo() {
	if (RenderingServer *rs = RenderingServer::get_singleton()) {
		for (RID &instance : instances) {
			if (instance.is_valid()) {
				rs->instance_set_scenario(instance, RID());
				rs->free_rid(instance);
				instance = RID();
			}
		}
	}
	scenario = RID();
}
