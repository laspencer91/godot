/**************************************************************************/
/*  level_mesh_adjacency.h                                                */
/**************************************************************************/

#pragma once

#include "core/object/ref_counted.h"
#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"
#include "core/templates/vector.h"

class LevelMesh;
class LevelMeshData;

class LevelMeshAdjacency : public RefCounted {
	GDCLASS(LevelMeshAdjacency, RefCounted);

	Ref<LevelMeshData> data;
	mutable bool dirty = true;
	mutable bool valid = false;
	mutable HashMap<uint64_t, int> edge_by_vertices;
	mutable Vector<Vector<int>> edge_faces;
	mutable Vector<Vector<int>> vertex_edges;
	mutable Vector<Vector<int>> vertex_faces;
	mutable Vector<Vector<int>> face_edges;

	static uint64_t _edge_key(int p_vertex_a, int p_vertex_b);
	static PackedInt32Array _to_packed(const Vector<int> &p_values);
	bool _edge_is_alive(int p_edge_id) const;
	bool _face_is_alive(int p_face_id) const;
	bool _face_is_quad(int p_face_id) const;
	bool _get_edge_vertices(int p_edge_id, int &r_vertex_a, int &r_vertex_b) const;
	bool _get_face_plane(int p_face_id, Vector3 &r_normal, real_t &r_distance) const;
	bool _face_matches_plane(int p_face_id, const Vector3 &p_normal, real_t p_distance, real_t p_normal_angle_epsilon, real_t p_plane_distance_epsilon, bool p_allow_flipped_normal) const;
	void _walk_loop_direction(int p_seed_edge_id, int p_start_vertex_id, HashSet<int> &r_visited, PackedInt32Array &r_edges) const;
	void _walk_ring_direction(int p_seed_edge_id, int p_start_face_id, HashSet<int> &r_visited, PackedInt32Array &r_edges) const;
	void _set_data(const Ref<LevelMeshData> &p_data);
	void _mark_dirty();
	void _ensure_built() const;
	void _rebuild() const;

	friend class LevelMesh;

protected:
	static void _bind_methods();

public:
	bool is_valid() const;
	int find_edge(int p_vertex_a, int p_vertex_b) const;
	PackedInt32Array get_edge_vertices(int p_edge_id) const;
	PackedInt32Array get_edge_faces(int p_edge_id) const;
	PackedInt32Array get_edge_faces_by_vertices(int p_vertex_a, int p_vertex_b) const;
	PackedInt32Array get_vertex_edges(int p_vertex_id) const;
	PackedInt32Array get_vertex_faces(int p_vertex_id) const;
	PackedInt32Array get_face_edges(int p_face_id) const;
	PackedInt32Array walk_edge_loop(int p_seed_edge_id) const;
	PackedInt32Array walk_edge_ring(int p_seed_edge_id) const;
	PackedInt32Array coplanar_flood_fill(int p_seed_face_id, real_t p_normal_angle_epsilon = (real_t)0.001, real_t p_plane_distance_epsilon = (real_t)0.0001) const;
	PackedInt32Array faces_on_plane(int p_seed_face_id, real_t p_normal_angle_epsilon = (real_t)0.001, real_t p_plane_distance_epsilon = (real_t)0.0001) const;
	Dictionary classify_region_rim(const PackedInt32Array &p_face_ids) const;
	PackedInt32Array get_polygroup_faces(int p_seed_face_id) const;
	PackedInt32Array get_polygroup_boundary_edges(int p_seed_face_id) const;
};
