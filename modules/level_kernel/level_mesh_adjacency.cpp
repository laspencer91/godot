/**************************************************************************/
/*  level_mesh_adjacency.cpp                                              */
/**************************************************************************/

#include "level_mesh_adjacency.h"

#include "level_mesh_data.h"

#include "core/math/math_funcs.h"
#include "core/object/class_db.h"

uint64_t LevelMeshAdjacency::_edge_key(int p_vertex_a, int p_vertex_b) {
	const uint32_t a = (uint32_t)MIN(p_vertex_a, p_vertex_b);
	const uint32_t b = (uint32_t)MAX(p_vertex_a, p_vertex_b);
	return ((uint64_t)a << 32) | b;
}

PackedInt32Array LevelMeshAdjacency::_to_packed(const Vector<int> &p_values) {
	return p_values;
}

bool LevelMeshAdjacency::_edge_is_alive(int p_edge_id) const {
	return data.is_valid() && p_edge_id >= 0 && p_edge_id < data->edge_alive.size() && data->edge_alive[p_edge_id] != 0;
}

bool LevelMeshAdjacency::_face_is_alive(int p_face_id) const {
	return data.is_valid() && p_face_id >= 0 && p_face_id < data->face_alive.size() && data->face_alive[p_face_id] != 0;
}

bool LevelMeshAdjacency::_face_is_quad(int p_face_id) const {
	return _face_is_alive(p_face_id) && p_face_id < data->face_loop_counts.size() && data->face_loop_counts[p_face_id] == 4 &&
			p_face_id < face_edges.size() && face_edges[p_face_id].size() == 4;
}

bool LevelMeshAdjacency::_get_edge_vertices(int p_edge_id, int &r_vertex_a, int &r_vertex_b) const {
	if (!_edge_is_alive(p_edge_id) || p_edge_id > (data->edge_vertices.size() - 2) / 2) {
		return false;
	}
	const int offset = p_edge_id * 2;
	r_vertex_a = data->edge_vertices[offset];
	r_vertex_b = data->edge_vertices[offset + 1];
	return r_vertex_a >= 0 && r_vertex_b >= 0 && r_vertex_a < data->vertex_alive.size() && r_vertex_b < data->vertex_alive.size() &&
			data->vertex_alive[r_vertex_a] != 0 && data->vertex_alive[r_vertex_b] != 0;
}

bool LevelMeshAdjacency::_get_face_plane(int p_face_id, Vector3 &r_normal, real_t &r_distance) const {
	if (!_face_is_alive(p_face_id) || p_face_id >= data->face_loop_starts.size() || p_face_id >= data->face_loop_counts.size()) {
		return false;
	}
	const int loop_start = data->face_loop_starts[p_face_id];
	const int loop_count = data->face_loop_counts[p_face_id];
	if (loop_count < 3 || loop_start < 0 || loop_start > data->loop_vertex_indices.size() - loop_count ||
			loop_start > data->loop_alive.size() - loop_count) {
		return false;
	}
	const int first_vertex = data->loop_vertex_indices[loop_start];
	if (first_vertex < 0 || first_vertex >= data->vertex_positions.size() || first_vertex >= data->vertex_alive.size() || data->vertex_alive[first_vertex] == 0) {
		return false;
	}
	const Vector3 origin = data->vertex_positions[first_vertex];
	for (int corner = 1; corner < loop_count - 1; corner++) {
		const int vertex_b = data->loop_vertex_indices[loop_start + corner];
		const int vertex_c = data->loop_vertex_indices[loop_start + corner + 1];
		if (vertex_b < 0 || vertex_c < 0 || vertex_b >= data->vertex_positions.size() || vertex_c >= data->vertex_positions.size() ||
				vertex_b >= data->vertex_alive.size() || vertex_c >= data->vertex_alive.size() ||
				data->vertex_alive[vertex_b] == 0 || data->vertex_alive[vertex_c] == 0) {
			return false;
		}
		Vector3 normal = (data->vertex_positions[vertex_b] - origin).cross(data->vertex_positions[vertex_c] - origin);
		if (normal.length_squared() > CMP_EPSILON2) {
			r_normal = normal.normalized();
			r_distance = r_normal.dot(origin);
			return true;
		}
	}
	return false;
}

bool LevelMeshAdjacency::_face_matches_plane(int p_face_id, const Vector3 &p_normal, real_t p_distance, real_t p_normal_angle_epsilon, real_t p_plane_distance_epsilon, bool p_allow_flipped_normal) const {
	Vector3 normal;
	real_t distance = 0;
	if (!_get_face_plane(p_face_id, normal, distance)) {
		return false;
	}
	const real_t normal_alignment = normal.dot(p_normal);
	if ((p_allow_flipped_normal ? Math::abs(normal_alignment) : normal_alignment) < Math::cos(p_normal_angle_epsilon)) {
		return false;
	}
	const int loop_start = data->face_loop_starts[p_face_id];
	const int loop_count = data->face_loop_counts[p_face_id];
	for (int corner = 0; corner < loop_count; corner++) {
		const int loop_id = loop_start + corner;
		if (loop_id < 0 || loop_id >= data->loop_alive.size() || data->loop_alive[loop_id] == 0 || loop_id >= data->loop_vertex_indices.size()) {
			return false;
		}
		const int vertex_id = data->loop_vertex_indices[loop_id];
		if (vertex_id < 0 || vertex_id >= data->vertex_positions.size() || Math::abs(p_normal.dot(data->vertex_positions[vertex_id]) - p_distance) > p_plane_distance_epsilon) {
			return false;
		}
	}
	return true;
}

void LevelMeshAdjacency::_set_data(const Ref<LevelMeshData> &p_data) {
	data = p_data;
	_mark_dirty();
}

void LevelMeshAdjacency::_mark_dirty() {
	dirty = true;
}

void LevelMeshAdjacency::_ensure_built() const {
	if (dirty) {
		_rebuild();
	}
}

void LevelMeshAdjacency::_rebuild() const {
	edge_by_vertices.clear();
	edge_faces.clear();
	vertex_edges.clear();
	vertex_faces.clear();
	face_edges.clear();
	valid = data.is_valid();
	dirty = false;
	if (data.is_null()) {
		return;
	}

	edge_faces.resize(data->edge_alive.size());
	vertex_edges.resize(data->vertex_alive.size());
	vertex_faces.resize(data->vertex_alive.size());
	face_edges.resize(data->face_alive.size());

	for (int edge_id = 0; edge_id < data->edge_alive.size(); edge_id++) {
		if (data->edge_alive[edge_id] == 0) {
			continue;
		}
		int vertex_a = -1;
		int vertex_b = -1;
		if (!_get_edge_vertices(edge_id, vertex_a, vertex_b) || vertex_a == vertex_b) {
			valid = false;
			continue;
		}
		const uint64_t key = _edge_key(vertex_a, vertex_b);
		if (edge_by_vertices.has(key)) {
			valid = false;
			continue;
		}
		edge_by_vertices.insert(key, edge_id);
		vertex_edges.write[vertex_a].push_back(edge_id);
		vertex_edges.write[vertex_b].push_back(edge_id);
	}

	for (int face_id = 0; face_id < data->face_alive.size(); face_id++) {
		if (data->face_alive[face_id] == 0) {
			continue;
		}
		if (face_id >= data->face_loop_starts.size() || face_id >= data->face_loop_counts.size()) {
			valid = false;
			continue;
		}
		const int loop_start = data->face_loop_starts[face_id];
		const int loop_count = data->face_loop_counts[face_id];
		if (loop_count < 3 || loop_start < 0 || loop_start > data->loop_vertex_indices.size() - loop_count ||
				loop_start > data->loop_alive.size() - loop_count) {
			valid = false;
			continue;
		}

		HashSet<int> face_vertices;
		for (int corner = 0; corner < loop_count; corner++) {
			const int loop_id = loop_start + corner;
			const int next_loop_id = loop_start + ((corner + 1) % loop_count);
			const int vertex_a = data->loop_vertex_indices[loop_id];
			const int vertex_b = data->loop_vertex_indices[next_loop_id];
			if (data->loop_alive[loop_id] == 0 || vertex_a < 0 || vertex_b < 0 ||
					vertex_a >= data->vertex_alive.size() || vertex_b >= data->vertex_alive.size() ||
					data->vertex_alive[vertex_a] == 0 || data->vertex_alive[vertex_b] == 0) {
				face_edges.write[face_id].push_back(-1);
				valid = false;
				continue;
			}
			if (!face_vertices.has(vertex_a)) {
				face_vertices.insert(vertex_a);
				vertex_faces.write[vertex_a].push_back(face_id);
			}
			const int *edge_id = edge_by_vertices.getptr(_edge_key(vertex_a, vertex_b));
			if (edge_id == nullptr) {
				face_edges.write[face_id].push_back(-1);
				valid = false;
				continue;
			}
			face_edges.write[face_id].push_back(*edge_id);
			Vector<int> &adjacent_faces = edge_faces.write[*edge_id];
			if (adjacent_faces.find(face_id) == -1) {
				adjacent_faces.push_back(face_id);
			}
		}
	}
}

bool LevelMeshAdjacency::is_valid() const {
	_ensure_built();
	return valid;
}

int LevelMeshAdjacency::find_edge(int p_vertex_a, int p_vertex_b) const {
	_ensure_built();
	if (p_vertex_a < 0 || p_vertex_b < 0) {
		return -1;
	}
	const int *edge_id = edge_by_vertices.getptr(_edge_key(p_vertex_a, p_vertex_b));
	return edge_id != nullptr ? *edge_id : -1;
}

PackedInt32Array LevelMeshAdjacency::get_edge_vertices(int p_edge_id) const {
	_ensure_built();
	PackedInt32Array result;
	int vertex_a = -1;
	int vertex_b = -1;
	if (_get_edge_vertices(p_edge_id, vertex_a, vertex_b)) {
		result.push_back(vertex_a);
		result.push_back(vertex_b);
	}
	return result;
}

PackedInt32Array LevelMeshAdjacency::get_edge_faces(int p_edge_id) const {
	_ensure_built();
	return p_edge_id >= 0 && p_edge_id < edge_faces.size() ? _to_packed(edge_faces[p_edge_id]) : PackedInt32Array();
}

PackedInt32Array LevelMeshAdjacency::get_edge_faces_by_vertices(int p_vertex_a, int p_vertex_b) const {
	return get_edge_faces(find_edge(p_vertex_a, p_vertex_b));
}

PackedInt32Array LevelMeshAdjacency::get_vertex_edges(int p_vertex_id) const {
	_ensure_built();
	return p_vertex_id >= 0 && p_vertex_id < vertex_edges.size() ? _to_packed(vertex_edges[p_vertex_id]) : PackedInt32Array();
}

PackedInt32Array LevelMeshAdjacency::get_vertex_faces(int p_vertex_id) const {
	_ensure_built();
	return p_vertex_id >= 0 && p_vertex_id < vertex_faces.size() ? _to_packed(vertex_faces[p_vertex_id]) : PackedInt32Array();
}

PackedInt32Array LevelMeshAdjacency::get_face_edges(int p_face_id) const {
	_ensure_built();
	return p_face_id >= 0 && p_face_id < face_edges.size() ? _to_packed(face_edges[p_face_id]) : PackedInt32Array();
}

void LevelMeshAdjacency::_walk_loop_direction(int p_seed_edge_id, int p_start_vertex_id, HashSet<int> &r_visited, PackedInt32Array &r_edges) const {
	int current_edge_id = p_seed_edge_id;
	int current_vertex_id = p_start_vertex_id;
	while (true) {
		if (!_edge_is_alive(current_edge_id) || current_edge_id >= edge_faces.size() || edge_faces[current_edge_id].size() != 2 ||
				current_vertex_id < 0 || current_vertex_id >= vertex_edges.size() || vertex_edges[current_vertex_id].size() != 4) {
			return;
		}
		const Vector<int> &current_faces = edge_faces[current_edge_id];
		if (!_face_is_quad(current_faces[0]) || !_face_is_quad(current_faces[1])) {
			return;
		}

		int next_edge_id = -1;
		for (const int candidate_edge_id : vertex_edges[current_vertex_id]) {
			if (candidate_edge_id == current_edge_id || !_edge_is_alive(candidate_edge_id) || candidate_edge_id >= edge_faces.size()) {
				continue;
			}
			bool shares_current_face = false;
			for (const int candidate_face_id : edge_faces[candidate_edge_id]) {
				if (candidate_face_id == current_faces[0] || candidate_face_id == current_faces[1]) {
					shares_current_face = true;
					break;
				}
			}
			if (!shares_current_face) {
				if (next_edge_id != -1) {
					return;
				}
				next_edge_id = candidate_edge_id;
			}
		}
		if (next_edge_id == -1 || r_visited.has(next_edge_id)) {
			return;
		}

		int vertex_a = -1;
		int vertex_b = -1;
		if (!_get_edge_vertices(next_edge_id, vertex_a, vertex_b)) {
			return;
		}
		const int next_vertex_id = vertex_a == current_vertex_id ? vertex_b : (vertex_b == current_vertex_id ? vertex_a : -1);
		if (next_vertex_id == -1) {
			return;
		}
		r_visited.insert(next_edge_id);
		r_edges.push_back(next_edge_id);
		current_edge_id = next_edge_id;
		current_vertex_id = next_vertex_id;
	}
}

PackedInt32Array LevelMeshAdjacency::walk_edge_loop(int p_seed_edge_id) const {
	_ensure_built();
	PackedInt32Array result;
	int vertex_a = -1;
	int vertex_b = -1;
	if (!_get_edge_vertices(p_seed_edge_id, vertex_a, vertex_b)) {
		return result;
	}
	HashSet<int> visited;
	visited.insert(p_seed_edge_id);
	result.push_back(p_seed_edge_id);
	_walk_loop_direction(p_seed_edge_id, vertex_a, visited, result);
	_walk_loop_direction(p_seed_edge_id, vertex_b, visited, result);
	return result;
}

void LevelMeshAdjacency::_walk_ring_direction(int p_seed_edge_id, int p_start_face_id, HashSet<int> &r_visited, PackedInt32Array &r_edges) const {
	int current_edge_id = p_seed_edge_id;
	int current_face_id = p_start_face_id;
	while (current_face_id != -1) {
		if (!_face_is_quad(current_face_id) || current_face_id >= face_edges.size()) {
			return;
		}
		const Vector<int> &quad_edges = face_edges[current_face_id];
		const int edge_corner = quad_edges.find(current_edge_id);
		if (edge_corner == -1) {
			return;
		}
		const int opposite_edge_id = quad_edges[(edge_corner + 2) % 4];
		if (!_edge_is_alive(opposite_edge_id) || r_visited.has(opposite_edge_id)) {
			return;
		}
		r_visited.insert(opposite_edge_id);
		r_edges.push_back(opposite_edge_id);

		if (opposite_edge_id >= edge_faces.size() || edge_faces[opposite_edge_id].size() > 2) {
			return;
		}
		int other_face_id = -1;
		for (const int adjacent_face_id : edge_faces[opposite_edge_id]) {
			if (adjacent_face_id != current_face_id) {
				other_face_id = adjacent_face_id;
				break;
			}
		}
		current_edge_id = opposite_edge_id;
		current_face_id = other_face_id;
	}
}

PackedInt32Array LevelMeshAdjacency::walk_edge_ring(int p_seed_edge_id) const {
	_ensure_built();
	PackedInt32Array result;
	if (!_edge_is_alive(p_seed_edge_id) || p_seed_edge_id >= edge_faces.size() || edge_faces[p_seed_edge_id].size() > 2) {
		return result;
	}
	HashSet<int> visited;
	visited.insert(p_seed_edge_id);
	result.push_back(p_seed_edge_id);
	for (const int face_id : edge_faces[p_seed_edge_id]) {
		_walk_ring_direction(p_seed_edge_id, face_id, visited, result);
	}
	return result;
}

PackedInt32Array LevelMeshAdjacency::coplanar_flood_fill(int p_seed_face_id, real_t p_normal_angle_epsilon, real_t p_plane_distance_epsilon) const {
	_ensure_built();
	PackedInt32Array result;
	if (!_face_is_alive(p_seed_face_id) || !Math::is_finite(p_normal_angle_epsilon) || !Math::is_finite(p_plane_distance_epsilon) ||
			p_normal_angle_epsilon < 0 || p_normal_angle_epsilon > Math::PI || p_plane_distance_epsilon < 0) {
		return result;
	}
	Vector3 seed_normal;
	real_t seed_distance = 0;
	if (!_get_face_plane(p_seed_face_id, seed_normal, seed_distance)) {
		return result;
	}

	HashSet<int> visited;
	Vector<int> queue;
	visited.insert(p_seed_face_id);
	queue.push_back(p_seed_face_id);
	for (int cursor = 0; cursor < queue.size(); cursor++) {
		const int face_id = queue[cursor];
		result.push_back(face_id);
		if (face_id < 0 || face_id >= face_edges.size()) {
			continue;
		}
		for (const int edge_id : face_edges[face_id]) {
			if (edge_id < 0 || edge_id >= edge_faces.size()) {
				continue;
			}
			for (const int adjacent_face_id : edge_faces[edge_id]) {
				if (!visited.has(adjacent_face_id) && _face_matches_plane(adjacent_face_id, seed_normal, seed_distance, p_normal_angle_epsilon, p_plane_distance_epsilon, false)) {
					visited.insert(adjacent_face_id);
					queue.push_back(adjacent_face_id);
				}
			}
		}
	}
	return result;
}

PackedInt32Array LevelMeshAdjacency::faces_on_plane(int p_seed_face_id, real_t p_normal_angle_epsilon, real_t p_plane_distance_epsilon) const {
	_ensure_built();
	PackedInt32Array result;
	if (!_face_is_alive(p_seed_face_id) || !Math::is_finite(p_normal_angle_epsilon) || !Math::is_finite(p_plane_distance_epsilon) ||
			p_normal_angle_epsilon < 0 || p_normal_angle_epsilon > Math::PI || p_plane_distance_epsilon < 0) {
		return result;
	}
	Vector3 seed_normal;
	real_t seed_distance = 0;
	if (!_get_face_plane(p_seed_face_id, seed_normal, seed_distance)) {
		return result;
	}
	for (int face_id = 0; face_id < data->face_alive.size(); face_id++) {
		if (_face_matches_plane(face_id, seed_normal, seed_distance, p_normal_angle_epsilon, p_plane_distance_epsilon, true)) {
			result.push_back(face_id);
		}
	}
	return result;
}

Dictionary LevelMeshAdjacency::classify_region_rim(const PackedInt32Array &p_face_ids) const {
	_ensure_built();
	Dictionary result;
	result["valid"] = false;
	result["ambiguous_non_manifold"] = false;
	result["reason"] = String();
	result["interior_edges"] = PackedInt32Array();
	result["rim_edges"] = PackedInt32Array();
	result["rim_owner_faces"] = PackedInt32Array();
	if (!valid) {
		result["reason"] = "invalid_adjacency";
		return result;
	}

	HashSet<int> selected_faces;
	HashSet<int> touched_edge_set;
	Vector<int> touched_edges;
	for (int i = 0; i < p_face_ids.size(); i++) {
		const int face_id = p_face_ids[i];
		if (!_face_is_alive(face_id) || face_id >= face_edges.size()) {
			result["reason"] = "invalid_face";
			return result;
		}
		selected_faces.insert(face_id);
		for (const int edge_id : face_edges[face_id]) {
			if (!_edge_is_alive(edge_id)) {
				result["reason"] = "invalid_edge";
				return result;
			}
			if (!touched_edge_set.has(edge_id)) {
				touched_edge_set.insert(edge_id);
				touched_edges.push_back(edge_id);
			}
		}
	}
	touched_edges.sort();

	PackedInt32Array interior_edges;
	PackedInt32Array rim_edges;
	PackedInt32Array rim_owner_faces;
	for (const int edge_id : touched_edges) {
		if (edge_faces[edge_id].size() > 2) {
			result["ambiguous_non_manifold"] = true;
			result["reason"] = "non_manifold_edge";
			return result;
		}
		int selected_count = 0;
		int owner_face_id = -1;
		for (const int face_id : edge_faces[edge_id]) {
			if (selected_faces.has(face_id)) {
				selected_count++;
				owner_face_id = face_id;
			}
		}
		if (selected_count == 2) {
			interior_edges.push_back(edge_id);
		} else if (selected_count == 1) {
			rim_edges.push_back(edge_id);
			rim_owner_faces.push_back(owner_face_id);
		} else {
			result["reason"] = "inconsistent_adjacency";
			return result;
		}
	}

	result["valid"] = true;
	result["interior_edges"] = interior_edges;
	result["rim_edges"] = rim_edges;
	result["rim_owner_faces"] = rim_owner_faces;
	return result;
}

PackedInt32Array LevelMeshAdjacency::get_polygroup_faces(int p_seed_face_id) const {
	_ensure_built();
	PackedInt32Array faces;
	if (!_face_is_alive(p_seed_face_id) || p_seed_face_id >= data->face_polygroup_ids.size()) {
		return faces;
	}
	const int polygroup_id = data->face_polygroup_ids[p_seed_face_id];
	for (int face_id = 0; face_id < data->face_alive.size(); face_id++) {
		if (_face_is_alive(face_id) && face_id < data->face_polygroup_ids.size() && data->face_polygroup_ids[face_id] == polygroup_id) {
			faces.push_back(face_id);
		}
	}
	return faces;
}

PackedInt32Array LevelMeshAdjacency::get_polygroup_boundary_edges(int p_seed_face_id) const {
	const PackedInt32Array faces = get_polygroup_faces(p_seed_face_id);
	if (faces.is_empty()) {
		return PackedInt32Array();
	}
	const Dictionary classification = classify_region_rim(faces);
	if (!(bool)classification["valid"]) {
		return PackedInt32Array();
	}
	return classification["rim_edges"];
}

void LevelMeshAdjacency::_bind_methods() {
	ClassDB::bind_method(D_METHOD("is_valid"), &LevelMeshAdjacency::is_valid);
	ClassDB::bind_method(D_METHOD("find_edge", "vertex_a", "vertex_b"), &LevelMeshAdjacency::find_edge);
	ClassDB::bind_method(D_METHOD("get_edge_vertices", "edge_id"), &LevelMeshAdjacency::get_edge_vertices);
	ClassDB::bind_method(D_METHOD("get_edge_faces", "edge_id"), &LevelMeshAdjacency::get_edge_faces);
	ClassDB::bind_method(D_METHOD("get_edge_faces_by_vertices", "vertex_a", "vertex_b"), &LevelMeshAdjacency::get_edge_faces_by_vertices);
	ClassDB::bind_method(D_METHOD("get_vertex_edges", "vertex_id"), &LevelMeshAdjacency::get_vertex_edges);
	ClassDB::bind_method(D_METHOD("get_vertex_faces", "vertex_id"), &LevelMeshAdjacency::get_vertex_faces);
	ClassDB::bind_method(D_METHOD("get_face_edges", "face_id"), &LevelMeshAdjacency::get_face_edges);
	ClassDB::bind_method(D_METHOD("walk_edge_loop", "seed_edge_id"), &LevelMeshAdjacency::walk_edge_loop);
	ClassDB::bind_method(D_METHOD("walk_edge_ring", "seed_edge_id"), &LevelMeshAdjacency::walk_edge_ring);
	ClassDB::bind_method(D_METHOD("coplanar_flood_fill", "seed_face_id", "normal_angle_epsilon", "plane_distance_epsilon"), &LevelMeshAdjacency::coplanar_flood_fill, DEFVAL((real_t)0.001), DEFVAL((real_t)0.0001));
	ClassDB::bind_method(D_METHOD("faces_on_plane", "seed_face_id", "normal_angle_epsilon", "plane_distance_epsilon"), &LevelMeshAdjacency::faces_on_plane, DEFVAL((real_t)0.001), DEFVAL((real_t)0.0001));
	ClassDB::bind_method(D_METHOD("classify_region_rim", "face_ids"), &LevelMeshAdjacency::classify_region_rim);
	ClassDB::bind_method(D_METHOD("get_polygroup_faces", "seed_face_id"), &LevelMeshAdjacency::get_polygroup_faces);
	ClassDB::bind_method(D_METHOD("get_polygroup_boundary_edges", "seed_face_id"), &LevelMeshAdjacency::get_polygroup_boundary_edges);
}
