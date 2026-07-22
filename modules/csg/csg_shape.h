/**************************************************************************/
/*  csg_shape.h                                                           */
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

#include "csg.h"

#include "core/templates/hash_map.h"
#include "scene/3d/path_3d.h"
#include "scene/3d/visual_instance_3d.h"

#ifndef PHYSICS_3D_DISABLED
#include "scene/resources/3d/concave_polygon_shape_3d.h"
#endif // PHYSICS_3D_DISABLED

class Mesh;
class NavigationMesh;
class NavigationMeshSourceGeometryData3D;
struct CSGEvaluationInputs;
struct CSGEvaluationSnapshot;
struct CSGRenderSurface;

using CSGOriginToken = uint32_t;

struct CSGSurfaceKey {
	// ObjectID is intentional: this is deliberately non-serializable,
	// process-local authoring identity rather than scene data.
	ObjectID source_shape;
	uint32_t semantic_surface = 0;
	uint32_t schema_generation = 0;

	bool operator==(const CSGSurfaceKey &p_other) const {
		return source_shape == p_other.source_shape && semantic_surface == p_other.semantic_surface && schema_generation == p_other.schema_generation;
	}
};

struct CSGSurfaceHit {
	CSGSurfaceKey surface;
	uint64_t result_generation = 0;
	uint32_t face_id = 0;
	uint32_t triangle = 0;
	uint32_t connected_fragment = UINT32_MAX;
};

class CSGShape3D : public GeometryInstance3D {
	GDCLASS(CSGShape3D, GeometryInstance3D);

public:
	enum Operation {
		OPERATION_UNION,
		OPERATION_INTERSECTION,
		OPERATION_SUBTRACTION,

	};

private:
	Operation operation = OPERATION_UNION;
	CSGShape3D *parent_shape = nullptr;

	struct ManifoldCache;
	ManifoldCache *manifold_cache = nullptr;
	uint32_t surface_schema_generation = 1;
	uint32_t cached_surface_schema_size = UINT32_MAX;
	uint64_t result_generation = 0;

	CSGBrush *brush = nullptr;

	AABB node_aabb;

	bool dirty = false;
	bool last_visible = false;
	float snap = 0.001;

	bool autosmooth = false;
	float smoothing_angle = 50.0;

#ifndef PHYSICS_3D_DISABLED
	bool use_collision = false;
	uint32_t collision_layer = 1;
	uint32_t collision_mask = 1;
	real_t collision_priority = 1.0;
	Ref<ConcavePolygonShape3D> root_collision_shape;
	RID root_collision_body;
	RID root_collision_debug_instance;
	Transform3D debug_shape_old_transform;
#endif // PHYSICS_3D_DISABLED

	bool calculate_tangents = true;

	Ref<ArrayMesh> root_mesh;

#ifndef PHYSICS_3D_DISABLED
	bool _is_debug_collision_shape_visible();
	void _update_debug_collision_shape();
	void _clear_debug_collision_shape();
	void _on_transform_changed();
	Vector<Vector3> _get_brush_collision_faces();
#endif // PHYSICS_3D_DISABLED

	void _queue_root_update(bool p_force = false);
	void _invalidate_subtree_and_ancestors();
	void _invalidate_materialization_and_ancestors();
	void _make_transform_dirty();
	void _make_operation_dirty();
	void _ensure_local_manifold();
	void _ensure_subtree_manifold();
	void _ensure_transformed_manifold();
	Ref<Material> _resolve_manifold_material(const Ref<Material> &p_source_material) const;
	void _gather_manifold_surface_records(HashMap<CSGOriginToken, Ref<Material>> &r_mesh_materials, HashMap<CSGOriginToken, CSGSurfaceKey> &r_surface_keys);
	CSGEvaluationInputs _gather_evaluation_inputs(bool p_want_render, bool p_want_collision);
	void _publish_snapshot(CSGEvaluationSnapshot &p_snapshot);
	void _update_cached_aabb_from_manifold();
	void _update_child_manifold_aabbs();

protected:
	void _notification(int p_what);
	virtual CSGBrush *_build_brush() = 0;
	virtual uint32_t _get_surface_schema_size() const { return 0; }
	void _synchronize_surface_schema();
	void _make_dirty();
	void _make_material_dirty();
	void _make_output_dirty();
	PackedStringArray get_configuration_warnings() const override;

	static void _bind_methods();

	friend class CSGCombiner3D;
	CSGBrush *_get_brush();

	void _validate_property(PropertyInfo &p_property) const;

public:
	Array get_meshes() const;
	void update_shape();

	void set_operation(Operation p_operation);
	Operation get_operation() const;

	virtual Vector<Vector3> get_brush_faces();
	uint32_t get_surface_schema_size() const;
	uint32_t get_surface_schema_generation() const;
	bool get_surface_key(uint32_t p_semantic_surface, CSGSurfaceKey &r_surface) const;
	bool get_surface_origin_token(uint32_t p_semantic_surface, CSGOriginToken &r_token);
	static bool is_surface_key_valid(const CSGSurfaceKey &p_surface);

	uint64_t get_result_generation() const;
	uint32_t get_result_triangle_count() const;
	bool resolve_result_triangle(uint32_t p_triangle, uint64_t p_result_generation, CSGSurfaceKey &r_surface, uint32_t &r_face_id, CSGOriginToken *r_origin_token = nullptr) const;

	virtual AABB get_aabb() const override;

	void set_use_collision(bool p_enable);
	bool is_using_collision() const;

	void set_collision_layer(uint32_t p_layer);
	uint32_t get_collision_layer() const;

	void set_collision_mask(uint32_t p_mask);
	uint32_t get_collision_mask() const;

	void set_collision_layer_value(int p_layer_number, bool p_value);
	bool get_collision_layer_value(int p_layer_number) const;

	void set_collision_mask_value(int p_layer_number, bool p_value);
	bool get_collision_mask_value(int p_layer_number) const;

	RID _get_root_collision_instance() const;

	void set_collision_priority(real_t p_priority);
	real_t get_collision_priority() const;

	void set_autosmooth(bool p_smooth);
	bool is_autosmooth() const;

	void set_smoothing_angle(const float p_angle);
	float get_smoothing_angle() const;

#ifndef DISABLE_DEPRECATED
	void set_snap(float p_snap);
	float get_snap() const;
#endif // DISABLE_DEPRECATED

	void set_calculate_tangents(bool p_calculate_tangents);
	bool is_calculating_tangents() const;

	bool is_root_shape() const;

	Ref<ArrayMesh> bake_static_mesh();
#ifndef PHYSICS_3D_DISABLED
	Ref<ConcavePolygonShape3D> bake_collision_shape();
#endif // PHYSICS_3D_DISABLED

	virtual Ref<TriangleMesh> generate_triangle_mesh() const override;

#ifndef NAVIGATION_3D_DISABLED
private:
	static Callable _navmesh_source_geometry_parsing_callback;
	static RID _navmesh_source_geometry_parser;

public:
	static void navmesh_parse_init();
	static void navmesh_parse_source_geometry(const Ref<NavigationMesh> &p_navigation_mesh, Ref<NavigationMeshSourceGeometryData3D> p_source_geometry_data, Node *p_node);
#endif // NAVIGATION_3D_DISABLED

	CSGShape3D();
	~CSGShape3D();
};

VARIANT_ENUM_CAST(CSGShape3D::Operation)

class CSGCombiner3D : public CSGShape3D {
	GDCLASS(CSGCombiner3D, CSGShape3D);

private:
	virtual CSGBrush *_build_brush() override;
	virtual uint32_t _get_surface_schema_size() const override { return SURFACE_COUNT; }

public:
	enum Surface {
		SURFACE_COUNT = 0,
	};

	CSGCombiner3D();
};

class CSGPrimitive3D : public CSGShape3D {
	GDCLASS(CSGPrimitive3D, CSGShape3D);

protected:
	bool flip_faces;
	CSGBrush *_create_brush_from_arrays(const Vector<Vector3> &p_vertices, const Vector<Vector2> &p_uv, const Vector<bool> &p_smooth, const Vector<Ref<Material>> &p_materials);
	static void _bind_methods();

public:
	void set_flip_faces(bool p_invert);
	bool get_flip_faces();

	CSGPrimitive3D();
};

class CSGMesh3D : public CSGPrimitive3D {
	GDCLASS(CSGMesh3D, CSGPrimitive3D);

	virtual uint32_t _get_surface_schema_size() const override;
	virtual CSGBrush *_build_brush() override;

	Ref<Mesh> mesh;
	Ref<Material> material;

	void _mesh_changed();

protected:
	static void _bind_methods();

public:
	static constexpr uint32_t SURFACE_SOURCE_MESH_BASE = 0;

	void set_mesh(const Ref<Mesh> &p_mesh);
	Ref<Mesh> get_mesh();

	void set_material(const Ref<Material> &p_material);
	Ref<Material> get_material() const;
};

class CSGSphere3D : public CSGPrimitive3D {
	GDCLASS(CSGSphere3D, CSGPrimitive3D);
	virtual uint32_t _get_surface_schema_size() const override { return SURFACE_COUNT; }
	virtual CSGBrush *_build_brush() override;

	Ref<Material> material;
	bool smooth_faces;
	float radius;
	int radial_segments;
	int rings;

protected:
	static void _bind_methods();

public:
	enum Surface {
		SURFACE_BODY = 0,
		SURFACE_COUNT,
	};

	void set_radius(const float p_radius);
	float get_radius() const;

	void set_radial_segments(const int p_radial_segments);
	int get_radial_segments() const;

	void set_rings(const int p_rings);
	int get_rings() const;

	void set_material(const Ref<Material> &p_material);
	Ref<Material> get_material() const;

	void set_smooth_faces(bool p_smooth_faces);
	bool get_smooth_faces() const;

	CSGSphere3D();
};

class CSGBox3D : public CSGPrimitive3D {
	GDCLASS(CSGBox3D, CSGPrimitive3D);
	virtual uint32_t _get_surface_schema_size() const override { return SURFACE_COUNT; }
	virtual CSGBrush *_build_brush() override;

	Ref<Material> material;
	Vector3 size = Vector3(1, 1, 1);

protected:
	static void _bind_methods();
#ifndef DISABLE_DEPRECATED
	// Kept for compatibility from 3.x to 4.0.
	bool _set(const StringName &p_name, const Variant &p_value);
#endif

public:
	enum Surface {
		SURFACE_POSITIVE_X = 0,
		SURFACE_NEGATIVE_X,
		SURFACE_POSITIVE_Y,
		SURFACE_NEGATIVE_Y,
		SURFACE_POSITIVE_Z,
		SURFACE_NEGATIVE_Z,
		SURFACE_COUNT,
	};

	void set_size(const Vector3 &p_size);
	Vector3 get_size() const;

	void set_material(const Ref<Material> &p_material);
	Ref<Material> get_material() const;

	CSGBox3D() {}
};

class CSGCylinder3D : public CSGPrimitive3D {
	GDCLASS(CSGCylinder3D, CSGPrimitive3D);
	virtual uint32_t _get_surface_schema_size() const override { return SURFACE_COUNT; }
	virtual CSGBrush *_build_brush() override;

	Ref<Material> material;
	float radius;
	float height;
	int sides;
	bool cone;
	bool smooth_faces;

protected:
	static void _bind_methods();

public:
	enum Surface {
		SURFACE_SIDE = 0,
		SURFACE_TOP,
		SURFACE_BOTTOM,
		SURFACE_COUNT,
	};

	void set_radius(const float p_radius);
	float get_radius() const;

	void set_height(const float p_height);
	float get_height() const;

	void set_sides(const int p_sides);
	int get_sides() const;

	void set_cone(const bool p_cone);
	bool is_cone() const;

	void set_smooth_faces(bool p_smooth_faces);
	bool get_smooth_faces() const;

	void set_material(const Ref<Material> &p_material);
	Ref<Material> get_material() const;

	CSGCylinder3D();
};

class CSGTorus3D : public CSGPrimitive3D {
	GDCLASS(CSGTorus3D, CSGPrimitive3D);
	virtual uint32_t _get_surface_schema_size() const override { return SURFACE_COUNT; }
	virtual CSGBrush *_build_brush() override;

	Ref<Material> material;
	float inner_radius;
	float outer_radius;
	int sides;
	int ring_sides;
	bool smooth_faces;

protected:
	static void _bind_methods();

public:
	enum Surface {
		SURFACE_BODY = 0,
		SURFACE_COUNT,
	};

	void set_inner_radius(const float p_inner_radius);
	float get_inner_radius() const;

	void set_outer_radius(const float p_outer_radius);
	float get_outer_radius() const;

	void set_sides(const int p_sides);
	int get_sides() const;

	void set_ring_sides(const int p_ring_sides);
	int get_ring_sides() const;

	void set_smooth_faces(bool p_smooth_faces);
	bool get_smooth_faces() const;

	void set_material(const Ref<Material> &p_material);
	Ref<Material> get_material() const;

	CSGTorus3D();
};

class CSGPolygon3D : public CSGPrimitive3D {
	GDCLASS(CSGPolygon3D, CSGPrimitive3D);

public:
	enum Mode {
		MODE_DEPTH,
		MODE_SPIN,
		MODE_PATH
	};

	enum PathIntervalType {
		PATH_INTERVAL_DISTANCE,
		PATH_INTERVAL_SUBDIVIDE
	};

	enum PathRotation {
		PATH_ROTATION_POLYGON,
		PATH_ROTATION_PATH,
		PATH_ROTATION_PATH_FOLLOW,
	};

private:
	virtual uint32_t _get_surface_schema_size() const override { return SURFACE_COUNT; }
	virtual CSGBrush *_build_brush() override;

	Vector<Vector2> polygon;
	Ref<Material> material;

	Mode mode;

	float depth;

	float spin_degrees;
	int spin_sides;

	NodePath path_node;
	PathIntervalType path_interval_type;
	float path_interval;
	float path_simplify_angle;
	PathRotation path_rotation;
	bool path_rotation_accurate;
	bool path_local;

	Path3D *path = nullptr;

	bool smooth_faces;
	bool path_continuous_u;
	real_t path_u_distance;
	bool path_joined;

	bool _is_editable_3d_polygon() const;
	bool _has_editable_3d_polygon_no_depth() const;

	void _path_changed();
	void _path_exited();

protected:
	static void _bind_methods();
	void _validate_property(PropertyInfo &p_property) const;
	void _notification(int p_what);

public:
	// The schema keeps all three slots even when the extrusion has no end caps
	// (e.g. an open path); the size stays constant so toggling caps does not bump
	// the surface schema generation and invalidate surface keys.
	enum Surface {
		SURFACE_FRONT = 0,
		SURFACE_BACK,
		SURFACE_SIDE,
		SURFACE_COUNT,
	};

	void set_polygon(const Vector<Vector2> &p_polygon);
	Vector<Vector2> get_polygon() const;

	void set_mode(Mode p_mode);
	Mode get_mode() const;

	void set_depth(float p_depth);
	float get_depth() const;

	void set_spin_degrees(float p_spin_degrees);
	float get_spin_degrees() const;

	void set_spin_sides(int p_spin_sides);
	int get_spin_sides() const;

	void set_path_node(const NodePath &p_path);
	NodePath get_path_node() const;

	void set_path_interval_type(PathIntervalType p_interval_type);
	PathIntervalType get_path_interval_type() const;

	void set_path_interval(float p_interval);
	float get_path_interval() const;

	void set_path_simplify_angle(float p_angle);
	float get_path_simplify_angle() const;

	void set_path_rotation(PathRotation p_rotation);
	PathRotation get_path_rotation() const;

	void set_path_rotation_accurate(bool p_enable);
	bool get_path_rotation_accurate() const;

	void set_path_local(bool p_enable);
	bool is_path_local() const;

	void set_path_continuous_u(bool p_enable);
	bool is_path_continuous_u() const;

	void set_path_u_distance(real_t p_path_u_distance);
	real_t get_path_u_distance() const;

	void set_path_joined(bool p_enable);
	bool is_path_joined() const;

	void set_smooth_faces(bool p_smooth_faces);
	bool get_smooth_faces() const;

	void set_material(const Ref<Material> &p_material);
	Ref<Material> get_material() const;

	CSGPolygon3D();
};

VARIANT_ENUM_CAST(CSGPolygon3D::Mode)
VARIANT_ENUM_CAST(CSGPolygon3D::PathRotation)
VARIANT_ENUM_CAST(CSGPolygon3D::PathIntervalType)
