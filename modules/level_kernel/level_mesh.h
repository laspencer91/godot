/**************************************************************************/
/*  level_mesh.h                                                          */
/**************************************************************************/

#pragma once

#include "core/math/transform_3d.h"
#include "core/object/ref_counted.h"

class LevelMeshData;
class LevelMeshDiff;
class LevelMeshAdjacency;
class LevelMeshElementBVH;

class LevelMesh : public RefCounted {
	GDCLASS(LevelMesh, RefCounted);

	Ref<LevelMeshData> data;
	Ref<LevelMeshData> transaction_before;
	Ref<LevelMeshAdjacency> adjacency;
	Ref<LevelMeshElementBVH> element_bvh;
	bool transaction_active = false;
	bool transaction_changed = false;
	bool transform_preview_active = false;
	bool geometry_change_notification = false;
	PackedInt32Array transform_preview_vertex_ids;
	PackedInt32Array transform_preview_face_ids;

	static Vector3 _dominant_axis_tangent(const Vector3 &p_normal);
	static void _append_uv_transform(LevelMeshData &r_data, const Transform2D &p_transform);
	static bool _compute_face_basis(const LevelMeshData &p_data, int p_face_id, bool p_canonical_tangent,
			Vector3 &r_origin, Vector3 &r_tangent, Vector3 &r_bitangent, Vector3 &r_normal);

	int _next_polygroup_id() const;
	int _append_vertex(const Vector3 &p_position);
	int _append_edge(int p_vertex_a, int p_vertex_b);
	int _append_quad_face(const int p_vertex_ids[4], int p_material_index, int p_polygroup_id, int p_face_flags);
	bool _get_face_directed_edge(int p_face_id, int p_edge_id, int &r_vertex_a, int &r_vertex_b) const;
	bool _initialize_face_projection(int p_face_id);
	bool _solve_texture_lock(int p_face_id, const LevelMeshData &p_before);
	bool _restore_transform_preview_baseline();
	bool _collect_push_pull(const PackedInt32Array &p_face_ids, real_t p_distance,
			PackedInt32Array &r_vertex_ids, PackedVector3Array &r_positions) const;
	bool _reconcile_face_uv(int p_face_id);
	bool _restore_diff_state(const Ref<LevelMeshDiff> &p_diff, bool p_reverted);
	void _invalidate_topology();
	void _invalidate_geometry();
	void _on_data_changed();

protected:
	static void _bind_methods();

public:
	void set_data(const Ref<LevelMeshData> &p_data);
	Ref<LevelMeshData> get_data() const;

	void begin_transaction();
	Ref<LevelMeshDiff> commit();
	void rollback();
	bool is_transaction_active() const;

	bool create_box(const Transform3D &p_frame, const Vector3 &p_size, int p_material_index);
	bool reconcile_face_uv(int p_face_id);
	bool set_face_texture_lock(int p_face_id, bool p_enabled);
	bool is_face_texture_locked(int p_face_id) const;

	bool begin_transform_preview(const PackedInt32Array &p_vertex_ids);
	bool preview_transform_vertices(const PackedVector3Array &p_new_positions);
	Ref<LevelMeshDiff> commit_transform_preview();
	void cancel_transform_preview();
	bool is_transform_preview_active() const;

	Ref<LevelMeshDiff> extrude_faces(const PackedInt32Array &p_face_ids);
	Dictionary calculate_push_pull(const PackedInt32Array &p_face_ids, real_t p_distance) const;
	Ref<LevelMeshDiff> push_pull_faces(const PackedInt32Array &p_face_ids, real_t p_distance);
	Ref<LevelMeshDiff> extrude_boundary_edges(const PackedInt32Array &p_edge_ids);

	bool apply_diff(const Ref<LevelMeshDiff> &p_diff);
	bool revert_diff(const Ref<LevelMeshDiff> &p_diff);

	Ref<LevelMeshAdjacency> get_adjacency() const;
	Ref<LevelMeshElementBVH> get_element_bvh() const;
	Dictionary ray_closest(const Vector3 &p_local_origin, const Vector3 &p_local_direction) const;
	PackedInt32Array get_face_corner_vertex_ids(int p_face_id) const;
	PackedVector3Array get_face_corner_positions(int p_face_id) const;
	int get_face_triangle_count(int p_face_id) const;
	PackedInt32Array get_face_triangle_vertex_ids(int p_face_id, int p_local_tri) const;
	Vector3 get_face_normal(int p_face_id) const;
	PackedInt32Array get_face_boundary_edge_ids(int p_face_id, bool p_polygroup_tier = false) const;
	PackedVector3Array get_face_boundary_edge_positions(int p_face_id, bool p_polygroup_tier = false) const;

	int64_t make_vertex_handle(int p_vertex_id) const;
	int64_t make_edge_handle(int p_edge_id) const;
	int64_t make_face_handle(int p_face_id) const;
	int resolve_vertex(int64_t p_handle) const;
	int resolve_edge(int64_t p_handle) const;
	int resolve_face(int64_t p_handle) const;

	LevelMesh();
	~LevelMesh();
};
