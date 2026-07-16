/**************************************************************************/
/*  level_mesh_operations.cpp                                             */
/**************************************************************************/

#include "level_mesh.h"
#include "level_mesh_adjacency.h"
#include "level_mesh_data.h"
#include "level_mesh_diff.h"

#include "core/math/math_funcs.h"
#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"

namespace {

bool packed_contains(const PackedInt32Array &p_values, int p_value) {
	return p_values.has(p_value);
}

bool point_sets_match_transform(const Vector<Vector3> &p_old_positions, const Vector<Vector3> &p_new_positions,
		const Transform3D &p_transform) {
	if (p_old_positions.size() != p_new_positions.size() || p_old_positions.is_empty() || !p_transform.is_finite()) {
		return false;
	}
	real_t scale = 1.0;
	for (int i = 0; i < p_old_positions.size(); i++) {
		scale = MAX(scale, MAX(p_old_positions[i].length(), p_new_positions[i].length()));
	}
	const real_t tolerance_squared = scale * scale * (real_t)1e-10;
	for (int i = 0; i < p_old_positions.size(); i++) {
		if (p_transform.xform(p_old_positions[i]).distance_squared_to(p_new_positions[i]) > tolerance_squared) {
			return false;
		}
	}
	return true;
}

bool solve_coplanar_affine_transform(const Vector<Vector3> &p_old_positions,
		const Vector<Vector3> &p_new_positions, Transform3D &r_transform);

bool solve_common_affine_transform(const PackedInt32Array &p_vertex_ids, const PackedVector3Array &p_old_positions,
		const PackedVector3Array &p_new_positions, Transform3D &r_transform) {
	if (p_vertex_ids.is_empty()) {
		return false;
	}
	Vector<Vector3> old_positions;
	Vector<Vector3> new_positions;
	old_positions.resize(p_vertex_ids.size());
	new_positions.resize(p_vertex_ids.size());
	for (int i = 0; i < p_vertex_ids.size(); i++) {
		const int vertex_id = p_vertex_ids[i];
		if (vertex_id < 0 || vertex_id >= p_old_positions.size() || vertex_id >= p_new_positions.size()) {
			return false;
		}
		old_positions.write[i] = p_old_positions[vertex_id];
		new_positions.write[i] = p_new_positions[vertex_id];
	}

	const Vector3 translation = new_positions[0] - old_positions[0];
	bool is_translation = true;
	for (int i = 1; i < old_positions.size(); i++) {
		if (!(new_positions[i] - old_positions[i]).is_equal_approx(translation)) {
			is_translation = false;
			break;
		}
	}
	if (is_translation) {
		r_transform = Transform3D(Basis(), translation);
		return true;
	}

	// Normal equations for new_xyz = A * [old_xyz, 1]. Four independent
	// parameters per output coordinate are solved together by pivoted Gaussian
	// elimination. Coplanar selections intentionally fall through to the
	// per-face affine construction below.
	real_t augmented[4][7] = {};
	for (int i = 0; i < old_positions.size(); i++) {
		const real_t row[4] = { old_positions[i].x, old_positions[i].y, old_positions[i].z, (real_t)1.0 };
		for (int parameter = 0; parameter < 4; parameter++) {
			for (int other = 0; other < 4; other++) {
				augmented[parameter][other] += row[parameter] * row[other];
			}
			augmented[parameter][4] += row[parameter] * new_positions[i].x;
			augmented[parameter][5] += row[parameter] * new_positions[i].y;
			augmented[parameter][6] += row[parameter] * new_positions[i].z;
		}
	}
	real_t matrix_scale = 0.0;
	for (int row = 0; row < 4; row++) {
		for (int column = 0; column < 4; column++) {
			matrix_scale = MAX(matrix_scale, Math::abs(augmented[row][column]));
		}
	}
	if (!Math::is_finite(matrix_scale) || matrix_scale <= CMP_EPSILON2) {
		return solve_coplanar_affine_transform(old_positions, new_positions, r_transform);
	}
	for (int column = 0; column < 4; column++) {
		int pivot = column;
		for (int row = column + 1; row < 4; row++) {
			if (Math::abs(augmented[row][column]) > Math::abs(augmented[pivot][column])) {
				pivot = row;
			}
		}
		if (Math::abs(augmented[pivot][column]) <= matrix_scale * (real_t)1e-10) {
			return solve_coplanar_affine_transform(old_positions, new_positions, r_transform);
		}
		if (pivot != column) {
			for (int entry = column; entry < 7; entry++) {
				SWAP(augmented[column][entry], augmented[pivot][entry]);
			}
		}
		const real_t inverse_pivot = (real_t)1.0 / augmented[column][column];
		for (int entry = column; entry < 7; entry++) {
			augmented[column][entry] *= inverse_pivot;
		}
		for (int row = 0; row < 4; row++) {
			if (row == column) {
				continue;
			}
			const real_t factor = augmented[row][column];
			for (int entry = column; entry < 7; entry++) {
				augmented[row][entry] -= factor * augmented[column][entry];
			}
		}
	}
	const Basis basis(
			augmented[0][4], augmented[1][4], augmented[2][4],
			augmented[0][5], augmented[1][5], augmented[2][5],
			augmented[0][6], augmented[1][6], augmented[2][6]);
	r_transform = Transform3D(basis, Vector3(augmented[3][4], augmented[3][5], augmented[3][6]));
	if (point_sets_match_transform(old_positions, new_positions, r_transform)) {
		return true;
	}
	return solve_coplanar_affine_transform(old_positions, new_positions, r_transform);
}

bool solve_coplanar_affine_transform(const Vector<Vector3> &p_old_positions,
		const Vector<Vector3> &p_new_positions, Transform3D &r_transform) {
	if (p_old_positions.size() != p_new_positions.size() || p_old_positions.size() < 3) {
		return false;
	}
	int first = 0;
	int second = 1;
	real_t longest_squared = 0.0;
	for (int a = 0; a < p_old_positions.size(); a++) {
		for (int b = a + 1; b < p_old_positions.size(); b++) {
			const real_t length_squared = p_old_positions[a].distance_squared_to(p_old_positions[b]);
			if (length_squared > longest_squared) {
				longest_squared = length_squared;
				first = a;
				second = b;
			}
		}
	}
	if (longest_squared <= CMP_EPSILON2) {
		return false;
	}
	const Vector3 old_edge1 = p_old_positions[second] - p_old_positions[first];
	int third = -1;
	real_t largest_cross_squared = 0.0;
	for (int i = 0; i < p_old_positions.size(); i++) {
		if (i == first || i == second) {
			continue;
		}
		const real_t cross_squared = old_edge1.cross(p_old_positions[i] - p_old_positions[first]).length_squared();
		if (cross_squared > largest_cross_squared) {
			largest_cross_squared = cross_squared;
			third = i;
		}
	}
	if (third < 0 || largest_cross_squared <= CMP_EPSILON2) {
		return false;
	}
	const Vector3 old_edge2 = p_old_positions[third] - p_old_positions[first];
	const Vector3 new_edge1 = p_new_positions[second] - p_new_positions[first];
	const Vector3 new_edge2 = p_new_positions[third] - p_new_positions[first];
	Vector3 old_normal = old_edge1.cross(old_edge2);
	Vector3 new_normal = new_edge1.cross(new_edge2);
	if (old_normal.length_squared() <= CMP_EPSILON2 || new_normal.length_squared() <= CMP_EPSILON2) {
		return false;
	}
	old_normal.normalize();
	new_normal.normalize();
	const Basis old_basis(old_edge1, old_edge2, old_normal);
	const Basis new_basis(new_edge1, new_edge2, new_normal);
	if (Math::abs(old_basis.determinant()) <= CMP_EPSILON || Math::abs(new_basis.determinant()) <= CMP_EPSILON) {
		return false;
	}
	const Basis basis = new_basis * old_basis.inverse();
	r_transform = Transform3D(basis, p_new_positions[first] - basis.xform(p_old_positions[first]));
	return point_sets_match_transform(p_old_positions, p_new_positions, r_transform);
}

} // namespace

bool LevelMesh::_compute_face_geometry(const LevelMeshData &p_data, int p_face_id,
		Vector3 &r_centroid, Vector3 &r_normal, real_t &r_area_x2, real_t &r_longest_edge_squared) {
	if (p_face_id < 0 || p_face_id >= p_data.face_alive.size() || p_data.face_alive[p_face_id] == 0 ||
			p_face_id >= p_data.face_loop_starts.size() || p_face_id >= p_data.face_loop_counts.size()) {
		return false;
	}
	const int loop_start = p_data.face_loop_starts[p_face_id];
	const int loop_count = p_data.face_loop_counts[p_face_id];
	if (loop_count < 3 || loop_start < 0 || loop_start > p_data.loop_vertex_indices.size() - loop_count ||
			loop_start > p_data.loop_alive.size() - loop_count) {
		return false;
	}

	r_centroid = Vector3();
	r_normal = Vector3();
	r_longest_edge_squared = 0.0;
	for (int corner = 0; corner < loop_count; corner++) {
		const int loop_id = loop_start + corner;
		const int next_loop_id = loop_start + ((corner + 1) % loop_count);
		const int vertex_id = p_data.loop_vertex_indices[loop_id];
		const int next_vertex_id = p_data.loop_vertex_indices[next_loop_id];
		if (p_data.loop_alive[loop_id] == 0 || vertex_id < 0 || next_vertex_id < 0 ||
				vertex_id >= p_data.vertex_positions.size() || next_vertex_id >= p_data.vertex_positions.size() ||
				vertex_id >= p_data.vertex_alive.size() || next_vertex_id >= p_data.vertex_alive.size() ||
				p_data.vertex_alive[vertex_id] == 0 || p_data.vertex_alive[next_vertex_id] == 0) {
			return false;
		}
		const Vector3 current = p_data.vertex_positions[vertex_id];
		const Vector3 next = p_data.vertex_positions[next_vertex_id];
		if (!current.is_finite() || !next.is_finite()) {
			return false;
		}
		r_centroid += current;
		r_longest_edge_squared = MAX(r_longest_edge_squared, current.distance_squared_to(next));
		// Newell's method remains stable for mildly non-planar edited faces.
		r_normal.x += (current.y - next.y) * (current.z + next.z);
		r_normal.y += (current.z - next.z) * (current.x + next.x);
		r_normal.z += (current.x - next.x) * (current.y + next.y);
	}
	r_area_x2 = r_normal.length();
	if (!Math::is_finite(r_area_x2) || r_area_x2 <= CMP_EPSILON || r_longest_edge_squared <= CMP_EPSILON2) {
		return false;
	}
	r_centroid /= (real_t)loop_count;
	r_normal.normalize();
	return true;
}

bool LevelMesh::_compute_face_basis(const LevelMeshData &p_data, int p_face_id, bool p_canonical_tangent,
		Vector3 &r_origin, Vector3 &r_tangent, Vector3 &r_bitangent, Vector3 &r_normal) {
	real_t area_x2 = 0.0;
	real_t longest_edge_squared = 0.0;
	if (!_compute_face_geometry(p_data, p_face_id, r_origin, r_normal, area_x2, longest_edge_squared)) {
		return false;
	}
	const int loop_start = p_data.face_loop_starts[p_face_id];
	const int loop_count = p_data.face_loop_counts[p_face_id];

	r_tangent = Vector3();
	if (!p_canonical_tangent && p_face_id < p_data.face_uv_tangents.size()) {
		r_tangent = p_data.face_uv_tangents[p_face_id];
		r_tangent -= r_normal * r_normal.dot(r_tangent);
	}
	if (r_tangent.length_squared() <= CMP_EPSILON2 || p_canonical_tangent) {
		real_t best_length_squared = 0.0;
		for (int corner = 0; corner < loop_count; corner++) {
			const int vertex_a = p_data.loop_vertex_indices[loop_start + corner];
			const int vertex_b = p_data.loop_vertex_indices[loop_start + ((corner + 1) % loop_count)];
			Vector3 candidate = p_data.vertex_positions[vertex_b] - p_data.vertex_positions[vertex_a];
			const real_t boundary_length_squared = candidate.length_squared();
			candidate -= r_normal * r_normal.dot(candidate);
			const real_t tie_tolerance = MAX((real_t)1.0, best_length_squared) * (real_t)1e-6;
			if (boundary_length_squared > best_length_squared + tie_tolerance) {
				best_length_squared = boundary_length_squared;
				r_tangent = candidate;
			}
		}
	}
	if (r_tangent.length_squared() <= CMP_EPSILON2) {
		r_tangent = _dominant_axis_tangent(r_normal);
	}
	r_tangent.normalize();
	r_bitangent = r_normal.cross(r_tangent);
	if (r_bitangent.length_squared() <= CMP_EPSILON2) {
		return false;
	}
	r_bitangent.normalize();
	return true;
}

int LevelMesh::_append_vertex(const Vector3 &p_position) {
	const int vertex_id = data->vertex_positions.size();
	data->vertex_positions.push_back(p_position);
	data->vertex_alive.push_back(1);
	data->vertex_generations.push_back((int32_t)data->_claim_vertex_generation());
	return vertex_id;
}

int LevelMesh::_append_edge(int p_vertex_a, int p_vertex_b) {
	const int edge_id = data->edge_alive.size();
	data->edge_vertices.push_back(p_vertex_a);
	data->edge_vertices.push_back(p_vertex_b);
	data->edge_alive.push_back(1);
	data->edge_generations.push_back((int32_t)data->_claim_edge_generation());
	return edge_id;
}

int LevelMesh::_append_quad_face(const int p_vertex_ids[4], int p_material_index, int p_polygroup_id, int p_face_flags) {
	const int face_id = data->face_alive.size();
	const int loop_start = data->loop_vertex_indices.size();
	data->face_loop_starts.push_back(loop_start);
	data->face_loop_counts.push_back(4);
	data->face_material_indices.push_back(p_material_index);
	data->face_uv_modes.push_back(LevelMeshData::UV_MODE_PROJECTED);
	data->face_uv_origins.push_back(Vector3());
	data->face_uv_tangents.push_back(Vector3());
	_append_uv_transform(**data, Transform2D());
	data->face_hotspot_patch_names.push_back(String());
	data->face_polygroup_ids.push_back(p_polygroup_id);
	data->face_flags.push_back(p_face_flags);
	data->face_alive.push_back(1);
	data->face_generations.push_back((int32_t)data->_claim_face_generation());
	for (int corner = 0; corner < 4; corner++) {
		data->loop_vertex_indices.push_back(p_vertex_ids[corner]);
		data->loop_uv0.push_back(Vector2());
		data->loop_colors.push_back(Color(1, 1, 1, 1));
		data->loop_normals.push_back(Vector3());
		data->loop_alive.push_back(1);
	}
	return face_id;
}

bool LevelMesh::_get_face_directed_edge(int p_face_id, int p_edge_id, int &r_vertex_a, int &r_vertex_b) const {
	const PackedInt32Array face_edges = adjacency->get_face_edges(p_face_id);
	if (p_face_id < 0 || p_face_id >= data->face_loop_starts.size() || p_face_id >= data->face_loop_counts.size()) {
		return false;
	}
	const int loop_start = data->face_loop_starts[p_face_id];
	const int loop_count = data->face_loop_counts[p_face_id];
	if (face_edges.size() != loop_count || loop_count < 3 || loop_start < 0 ||
			loop_start > data->loop_vertex_indices.size() - loop_count) {
		return false;
	}
	for (int corner = 0; corner < loop_count; corner++) {
		if (face_edges[corner] == p_edge_id) {
			r_vertex_a = data->loop_vertex_indices[loop_start + corner];
			r_vertex_b = data->loop_vertex_indices[loop_start + ((corner + 1) % loop_count)];
			return true;
		}
	}
	return false;
}

bool LevelMesh::_initialize_face_projection(int p_face_id) {
	return _set_grid_frame(p_face_id, true);
}

bool LevelMesh::_set_grid_frame(int p_face_id, bool p_reset_transform) {
	Vector3 centroid;
	Vector3 normal;
	real_t area_x2 = 0.0;
	real_t longest_edge_squared = 0.0;
	if (!_compute_face_geometry(**data, p_face_id, centroid, normal, area_x2, longest_edge_squared) ||
			p_face_id >= data->face_uv_modes.size() || p_face_id >= data->face_uv_origins.size() ||
			p_face_id >= data->face_uv_tangents.size()) {
		return false;
	}
	const Vector3 tangent = grid_uv_tangent_for_normal(normal);
	if (tangent.length_squared() <= CMP_EPSILON2) {
		return false;
	}
	data->face_uv_modes.set(p_face_id, LevelMeshData::UV_MODE_PROJECTED);
	data->face_uv_origins.set(p_face_id, Vector3());
	data->face_uv_tangents.set(p_face_id, tangent);
	if (p_reset_transform) {
		LevelMeshData::_write_uv_transform(**data, p_face_id, Transform2D());
	}
	return _reconcile_face_uv(p_face_id);
}

bool LevelMesh::_set_face_frame(int p_face_id, bool p_reset_transform) {
	Vector3 centroid;
	Vector3 normal;
	real_t area_x2 = 0.0;
	real_t longest_edge_squared = 0.0;
	if (!_compute_face_geometry(**data, p_face_id, centroid, normal, area_x2, longest_edge_squared) ||
			p_face_id >= data->face_uv_modes.size() || p_face_id >= data->face_uv_origins.size() ||
			p_face_id >= data->face_uv_tangents.size()) {
		return false;
	}
	Vector3 tangent = data->face_uv_tangents[p_face_id];
	if (tangent.is_finite() && tangent.length_squared() > CMP_EPSILON2) {
		tangent -= normal * tangent.dot(normal);
	}
	if (!tangent.is_finite() || tangent.length_squared() <= CMP_EPSILON2) {
		const int loop_start = data->face_loop_starts[p_face_id];
		const int loop_count = data->face_loop_counts[p_face_id];
		real_t best_length_squared = 0.0;
		for (int corner = 0; corner < loop_count; corner++) {
			const int vertex_a = data->loop_vertex_indices[loop_start + corner];
			const int vertex_b = data->loop_vertex_indices[loop_start + ((corner + 1) % loop_count)];
			Vector3 candidate = data->vertex_positions[vertex_b] - data->vertex_positions[vertex_a];
			const real_t boundary_length_squared = candidate.length_squared();
			candidate -= normal * candidate.dot(normal);
			const real_t tie_tolerance = MAX((real_t)1.0, best_length_squared) * (real_t)1e-6;
			if (boundary_length_squared > best_length_squared + tie_tolerance) {
				best_length_squared = boundary_length_squared;
				tangent = candidate;
			}
		}
	}
	if (!tangent.is_finite() || tangent.length_squared() <= CMP_EPSILON2) {
		return false;
	}
	tangent.normalize();
	data->face_uv_modes.set(p_face_id, LevelMeshData::UV_MODE_PROJECTED);
	data->face_uv_origins.set(p_face_id, centroid);
	data->face_uv_tangents.set(p_face_id, tangent);
	if (p_reset_transform) {
		LevelMeshData::_write_uv_transform(**data, p_face_id, Transform2D());
	}
	return _reconcile_face_uv(p_face_id);
}

bool LevelMesh::_initialize_wall_projection(int p_face_id, int p_owner_face_id, int p_vertex_a, int p_vertex_b) {
	if (p_face_id < 0 || p_face_id >= data->face_alive.size() || data->face_alive[p_face_id] == 0 ||
			p_face_id >= data->face_loop_starts.size() || p_face_id >= data->face_loop_counts.size() ||
			p_face_id >= data->face_uv_modes.size() || p_face_id >= data->face_uv_origins.size() ||
			p_face_id >= data->face_uv_tangents.size() || p_vertex_a < 0 || p_vertex_b < 0 ||
			p_vertex_a >= data->vertex_positions.size() || p_vertex_b >= data->vertex_positions.size()) {
		return false;
	}
	Vector3 owner_centroid;
	Vector3 owner_normal;
	real_t owner_area_x2 = 0.0;
	real_t owner_longest_edge_squared = 0.0;
	if (!_compute_face_geometry(**data, p_owner_face_id, owner_centroid, owner_normal,
			owner_area_x2, owner_longest_edge_squared)) {
		return false;
	}
	const Vector3 edge = data->vertex_positions[p_vertex_b] - data->vertex_positions[p_vertex_a];
	Vector3 predicted_normal = edge.cross(owner_normal);
	if (!predicted_normal.is_finite() || predicted_normal.length_squared() <= CMP_EPSILON2) {
		return false;
	}
	predicted_normal.normalize();
	const Vector3 tangent = grid_uv_tangent_for_normal(predicted_normal);
	Vector3 bitangent = predicted_normal.cross(tangent);
	if (tangent.length_squared() <= CMP_EPSILON2 || bitangent.length_squared() <= CMP_EPSILON2) {
		return false;
	}
	bitangent.normalize();
	data->face_uv_modes.set(p_face_id, LevelMeshData::UV_MODE_PROJECTED);
	data->face_uv_origins.set(p_face_id, Vector3());
	data->face_uv_tangents.set(p_face_id, tangent);
	LevelMeshData::_write_uv_transform(**data, p_face_id, Transform2D());
	return _reconcile_face_uv(p_face_id, nullptr, -1, false, &predicted_normal);
}

bool LevelMesh::_solve_texture_lock(int p_face_id, const LevelMeshData &p_before, const Transform3D *p_exact_transform) {
	if (p_face_id < 0 || p_face_id >= data->face_alive.size() || p_face_id >= p_before.face_alive.size() ||
			data->face_alive[p_face_id] == 0 || p_before.face_alive[p_face_id] == 0 ||
			p_face_id >= data->face_loop_starts.size() || p_face_id >= data->face_loop_counts.size() ||
			p_face_id >= p_before.face_loop_starts.size() || p_face_id >= p_before.face_loop_counts.size() ||
			data->face_loop_counts[p_face_id] != p_before.face_loop_counts[p_face_id]) {
		return false;
	}
	const int loop_start = data->face_loop_starts[p_face_id];
	const int before_loop_start = p_before.face_loop_starts[p_face_id];
	const int loop_count = data->face_loop_counts[p_face_id];
	if (loop_count < 3 || loop_start < 0 || before_loop_start < 0 ||
			loop_start > data->loop_vertex_indices.size() - loop_count ||
			before_loop_start > p_before.loop_vertex_indices.size() - loop_count ||
			before_loop_start > p_before.loop_uv0.size() - loop_count) {
		return false;
	}

	if (p_face_id >= data->face_uv_modes.size() || p_face_id >= p_before.face_uv_modes.size()) {
		return false;
	}
	const bool projected = data->face_uv_modes[p_face_id] == LevelMeshData::UV_MODE_PROJECTED;
	const bool texture_locked = p_face_id < data->face_flags.size() &&
			(data->face_flags[p_face_id] & LevelMeshData::FACE_FLAG_TEXTURE_LOCK) != 0;
	if (!projected) {
		if (data->face_uv_modes[p_face_id] != LevelMeshData::UV_MODE_EXPLICIT) {
			return false;
		}
		Vector3 centroid;
		Vector3 normal;
		real_t area_x2 = 0.0;
		real_t longest_edge_squared = 0.0;
		return !_compute_face_geometry(**data, p_face_id, centroid, normal, area_x2, longest_edge_squared) ||
				_reconcile_face_uv(p_face_id);
	}

	Vector3 old_centroid;
	Vector3 old_normal;
	real_t old_area_x2 = 0.0;
	real_t old_longest_edge_squared = 0.0;
	if (!_compute_face_geometry(p_before, p_face_id, old_centroid, old_normal, old_area_x2, old_longest_edge_squared)) {
		// A zero-distance extrude wall has no pre-transform plane. Its first
		// non-degenerate preview frame receives the standard fresh projection.
		Vector3 new_centroid;
		Vector3 new_normal;
		real_t new_area_x2 = 0.0;
		real_t new_longest_edge_squared = 0.0;
		return !_compute_face_geometry(**data, p_face_id, new_centroid, new_normal,
				new_area_x2, new_longest_edge_squared) || _initialize_face_projection(p_face_id);
	}
	Vector3 new_centroid;
	Vector3 new_normal;
	real_t new_area_x2 = 0.0;
	real_t new_longest_edge_squared = 0.0;
	if (!_compute_face_geometry(**data, p_face_id, new_centroid, new_normal,
			new_area_x2, new_longest_edge_squared)) {
		// A collapsed face cannot define B or a stable normal matrix. Keep every
		// UV field exactly as it was; geometry commit still remains finite.
		return true;
	}
	if (!texture_locked) {
		return _reconcile_face_uv(p_face_id);
	}

	if (p_face_id >= p_before.face_uv_origins.size() || p_face_id >= p_before.face_uv_tangents.size()) {
		return false;
	}
	const Transform2D old_transform = LevelMeshData::_read_uv_transform(p_before, p_face_id);
	if (!old_transform.is_finite()) {
		return false;
	}

	if (p_exact_transform != nullptr) {
		if (!p_exact_transform->is_finite()) {
			return false;
		}
		Vector3 tangent = p_exact_transform->basis.xform(p_before.face_uv_tangents[p_face_id]);
		tangent -= new_normal * tangent.dot(new_normal);
		if (!tangent.is_finite() || tangent.length_squared() <= CMP_EPSILON2) {
			return true;
		}
		tangent.normalize();
		const Vector3 origin = p_exact_transform->xform(p_before.face_uv_origins[p_face_id]);
		if (!origin.is_finite()) {
			return false;
		}
		data->face_uv_origins.set(p_face_id, origin);
		data->face_uv_tangents.set(p_face_id, tangent);
		LevelMeshData::_write_uv_transform(**data, p_face_id, old_transform);
		return _reconcile_face_uv(p_face_id);
	}

	// Partial/asymmetric motion keeps the frozen O/T frame. Only B follows the
	// live normal; the six affine UV coefficients are refit against that new
	// native projection.
	data->face_uv_origins.set(p_face_id, p_before.face_uv_origins[p_face_id]);
	Vector3 continuous_tangent = p_before.face_uv_tangents[p_face_id];
	continuous_tangent -= new_normal * continuous_tangent.dot(new_normal);
	if (!continuous_tangent.is_finite() || continuous_tangent.length_squared() <= CMP_EPSILON2) {
		return true;
	}
	continuous_tangent.normalize();
	data->face_uv_tangents.set(p_face_id, continuous_tangent);
	Vector3 new_tangent;
	Vector3 new_bitangent;
	if (!_get_projection_basis(**data, p_face_id, new_tangent, new_bitangent, new_normal)) {
		return true;
	}
	const real_t area_threshold = MAX((real_t)CMP_EPSILON,
			new_longest_edge_squared * (real_t)1e-6);
	if (new_area_x2 <= area_threshold) {
		return true;
	}

	Vector<Vector3> old_positions;
	Vector<Vector3> new_positions;
	Vector<Vector2> target_uvs;
	Vector<Vector2> projected_positions;
	Vector<int> stable_corners;
	old_positions.resize(loop_count);
	new_positions.resize(loop_count);
	target_uvs.resize(loop_count);
	projected_positions.resize(loop_count);
	for (int corner = 0; corner < loop_count; corner++) {
		const int vertex_id = data->loop_vertex_indices[loop_start + corner];
		const int old_vertex_id = p_before.loop_vertex_indices[before_loop_start + corner];
		if (vertex_id < 0 || old_vertex_id < 0 || vertex_id >= data->vertex_positions.size() ||
				old_vertex_id >= p_before.vertex_positions.size()) {
			return false;
		}
		old_positions.write[corner] = p_before.vertex_positions[old_vertex_id];
		new_positions.write[corner] = data->vertex_positions[vertex_id];
		target_uvs.write[corner] = p_before.loop_uv0[before_loop_start + corner];
		const Vector3 relative = new_positions[corner] - p_before.face_uv_origins[p_face_id];
		projected_positions.write[corner] = Vector2(relative.dot(new_tangent), relative.dot(new_bitangent));
		if (new_positions[corner] == old_positions[corner]) {
			stable_corners.push_back(corner);
		}
	}

	auto solve_affine = [&](const Vector<int> &p_corners, Transform2D &r_solved) -> bool {
		if (p_corners.size() < 3) {
			return false;
		}
		Vector2 mean_position;
		Vector2 mean_uv;
		for (const int corner : p_corners) {
			mean_position += projected_positions[corner];
			mean_uv += target_uvs[corner];
		}
		mean_position /= (real_t)p_corners.size();
		mean_uv /= (real_t)p_corners.size();
		real_t a00 = 0.0;
		real_t a01 = 0.0;
		real_t a11 = 0.0;
		real_t b00 = 0.0;
		real_t b01 = 0.0;
		real_t b10 = 0.0;
		real_t b11 = 0.0;
		for (const int corner : p_corners) {
			const Vector2 p = projected_positions[corner] - mean_position;
			const Vector2 uv = target_uvs[corner] - mean_uv;
			a00 += p.x * p.x;
			a01 += p.x * p.y;
			a11 += p.y * p.y;
			b00 += uv.x * p.x;
			b01 += uv.x * p.y;
			b10 += uv.y * p.x;
			b11 += uv.y * p.y;
		}
		const real_t determinant = a00 * a11 - a01 * a01;
		const real_t trace = a00 + a11;
		if (!Math::is_finite(determinant) || trace <= CMP_EPSILON2 ||
				Math::abs(determinant) <= trace * trace * (real_t)1e-10) {
			return false;
		}
		const real_t discriminant = Math::sqrt(MAX((real_t)0.0, trace * trace - (real_t)4.0 * determinant));
		const real_t largest_eigenvalue = (trace + discriminant) * (real_t)0.5;
		const real_t smallest_eigenvalue = (trace - discriminant) * (real_t)0.5;
		if (smallest_eigenvalue <= largest_eigenvalue * (real_t)1e-8) {
			return false;
		}
		const real_t inv00 = a11 / determinant;
		const real_t inv01 = -a01 / determinant;
		const real_t inv11 = a00 / determinant;
		const real_t m00 = b00 * inv00 + b01 * inv01;
		const real_t m01 = b00 * inv01 + b01 * inv11;
		const real_t m10 = b10 * inv00 + b11 * inv01;
		const real_t m11 = b10 * inv01 + b11 * inv11;
		const Vector2 offset = mean_uv - Vector2(m00 * mean_position.x + m01 * mean_position.y, m10 * mean_position.x + m11 * mean_position.y);
		r_solved = Transform2D(m00, m10, m01, m11, offset.x, offset.y);
		return r_solved.is_finite();
	};

	Transform2D solved;
	bool used_stable_constraints = stable_corners.size() >= 3 && solve_affine(stable_corners, solved);
	if (!used_stable_constraints) {
		Vector<int> all_corners;
		all_corners.resize(loop_count);
		for (int corner = 0; corner < loop_count; corner++) {
			all_corners.write[corner] = corner;
		}
		if (!solve_affine(all_corners, solved)) {
			// Ill-conditioned normal equations freeze the packed transform bytes.
			return _reconcile_face_uv(p_face_id);
		}
	}
	LevelMeshData::_write_uv_transform(**data, p_face_id, solved);
	if (!_reconcile_face_uv(p_face_id)) {
		return false;
	}
	if (used_stable_constraints) {
		// Re-materialization passes through PackedFloat32 coefficients. Snap exact
		// stationary anchors back to their already-authoritative old values so a
		// one-corner drag is bit-stable at every untouched loop.
		for (const int corner : stable_corners) {
			data->loop_uv0.set(loop_start + corner, target_uvs[corner]);
		}
	}
	return true;
}

bool LevelMesh::_restore_transform_preview_baseline() {
	if (!transform_preview_active || !transaction_active || transaction_before.is_null()) {
		return false;
	}
	for (const int vertex_id : transform_preview_vertex_ids) {
		if (vertex_id < 0 || vertex_id >= data->vertex_positions.size() || vertex_id >= transaction_before->vertex_positions.size()) {
			return false;
		}
		data->vertex_positions.set(vertex_id, transaction_before->vertex_positions[vertex_id]);
	}
	return true;
}

bool LevelMesh::set_face_texture_lock(int p_face_id, bool p_enabled) {
	if (!transaction_active || transform_preview_active || p_face_id < 0 || p_face_id >= data->face_alive.size() ||
			p_face_id >= data->face_flags.size() || data->face_alive[p_face_id] == 0) {
		return false;
	}
	const int old_flags = data->face_flags[p_face_id];
	const int new_flags = p_enabled ? old_flags | LevelMeshData::FACE_FLAG_TEXTURE_LOCK : old_flags & ~LevelMeshData::FACE_FLAG_TEXTURE_LOCK;
	if (old_flags == new_flags) {
		return true;
	}
	data->face_flags.set(p_face_id, new_flags);
	transaction_changed = true;
	geometry_change_notification = true;
	data->emit_changed();
	geometry_change_notification = false;
	return true;
}

bool LevelMesh::is_face_texture_locked(int p_face_id) const {
	return p_face_id >= 0 && p_face_id < data->face_alive.size() && p_face_id < data->face_flags.size() &&
			data->face_alive[p_face_id] != 0 && (data->face_flags[p_face_id] & LevelMeshData::FACE_FLAG_TEXTURE_LOCK) != 0;
}

bool LevelMesh::begin_transform_preview(const PackedInt32Array &p_vertex_ids) {
	if (transaction_active || transform_preview_active || p_vertex_ids.is_empty() || !adjacency->is_valid()) {
		return false;
	}
	PackedInt32Array unique_vertices;
	HashSet<int> affected_faces;
	for (const int vertex_id : p_vertex_ids) {
		if (packed_contains(unique_vertices, vertex_id)) {
			continue;
		}
		if (vertex_id < 0 || vertex_id >= data->vertex_alive.size() || vertex_id >= data->vertex_positions.size() ||
				data->vertex_alive[vertex_id] == 0) {
			return false;
		}
		unique_vertices.push_back(vertex_id);
		for (const int face_id : adjacency->get_vertex_faces(vertex_id)) {
			affected_faces.insert(face_id);
		}
	}
	Vector<int> sorted_faces;
	for (const int face_id : affected_faces) {
		sorted_faces.push_back(face_id);
	}
	sorted_faces.sort();
	PackedInt32Array faces;
	for (const int face_id : sorted_faces) {
		faces.push_back(face_id);
	}

	begin_transaction();
	transform_preview_active = true;
	transform_preview_vertex_ids = unique_vertices;
	transform_preview_face_ids = faces;
	return true;
}

bool LevelMesh::preview_transform_vertices(const PackedVector3Array &p_new_positions) {
	if (!transform_preview_active || !transaction_active || p_new_positions.size() != transform_preview_vertex_ids.size()) {
		return false;
	}
	for (const Vector3 &position : p_new_positions) {
		if (!position.is_finite()) {
			return false;
		}
	}
	if (!_restore_transform_preview_baseline()) {
		return false;
	}
	bool changed = false;
	for (int i = 0; i < transform_preview_vertex_ids.size(); i++) {
		const int vertex_id = transform_preview_vertex_ids[i];
		if (p_new_positions[i] != transaction_before->vertex_positions[vertex_id]) {
			changed = true;
		}
		data->vertex_positions.set(vertex_id, p_new_positions[i]);
	}
	if (!changed) {
		transaction_changed = false;
		_invalidate_geometry();
		data->_emit_mesh_preview_changed();
		return true;
	}
	transaction_changed = true;
	_invalidate_geometry();
	data->_emit_mesh_preview_changed();
	return true;
}

Ref<LevelMeshDiff> LevelMesh::commit_transform_preview() {
	if (!transform_preview_active || !transaction_active) {
		return Ref<LevelMeshDiff>();
	}
	if (transaction_changed) {
		Transform3D common_transform;
		const bool has_common_transform = solve_common_affine_transform(transform_preview_vertex_ids,
				transaction_before->vertex_positions, data->vertex_positions, common_transform);
		for (const int face_id : transform_preview_face_ids) {
			const PackedInt32Array face_vertices = get_face_corner_vertex_ids(face_id);
			bool whole_face_selected = !face_vertices.is_empty();
			for (const int vertex_id : face_vertices) {
				if (!transform_preview_vertex_ids.has(vertex_id)) {
					whole_face_selected = false;
					break;
				}
			}

			const Transform3D *exact_transform = nullptr;
			if (whole_face_selected && has_common_transform) {
				exact_transform = &common_transform;
			}

			if (!_solve_texture_lock(face_id, **transaction_before, exact_transform)) {
				transform_preview_active = false;
				transform_preview_vertex_ids.clear();
				transform_preview_face_ids.clear();
				rollback();
				return Ref<LevelMeshDiff>();
			}
		}
	}
	transform_preview_active = false;
	transform_preview_vertex_ids.clear();
	transform_preview_face_ids.clear();
	if (transaction_changed) {
		geometry_change_notification = true;
		data->emit_changed();
		geometry_change_notification = false;
	}
	return commit();
}

void LevelMesh::cancel_transform_preview() {
	if (!transform_preview_active || !transaction_active) {
		return;
	}
	transform_preview_active = false;
	transform_preview_vertex_ids.clear();
	transform_preview_face_ids.clear();
	rollback();
}

bool LevelMesh::is_transform_preview_active() const {
	return transform_preview_active;
}

Ref<LevelMeshDiff> LevelMesh::extrude_faces(const PackedInt32Array &p_face_ids) {
	if (transaction_active || transform_preview_active || p_face_ids.is_empty()) {
		return Ref<LevelMeshDiff>();
	}
	PackedInt32Array face_ids;
	for (const int face_id : p_face_ids) {
		if (face_id < 0 || face_id >= data->face_alive.size() || data->face_alive[face_id] == 0) {
			return Ref<LevelMeshDiff>();
		}
		if (!packed_contains(face_ids, face_id)) {
			face_ids.push_back(face_id);
		}
	}
	const Dictionary classification = adjacency->classify_region_rim(face_ids);
	if (!(bool)classification.get("valid", false) || (bool)classification.get("ambiguous_non_manifold", false)) {
		return Ref<LevelMeshDiff>();
	}
	const PackedInt32Array rim_edges = classification.get("rim_edges", PackedInt32Array());
	const PackedInt32Array rim_owners = classification.get("rim_owner_faces", PackedInt32Array());
	const PackedInt32Array interior_edges = classification.get("interior_edges", PackedInt32Array());
	if (rim_edges.is_empty() || rim_edges.size() != rim_owners.size()) {
		return Ref<LevelMeshDiff>();
	}

	Vector<int> rim_vertex_a;
	Vector<int> rim_vertex_b;
	rim_vertex_a.resize(rim_edges.size());
	rim_vertex_b.resize(rim_edges.size());
	for (int i = 0; i < rim_edges.size(); i++) {
		if (!_get_face_directed_edge(rim_owners[i], rim_edges[i], rim_vertex_a.write[i], rim_vertex_b.write[i])) {
			return Ref<LevelMeshDiff>();
		}
	}

	HashSet<int> selected_vertex_set;
	for (const int face_id : face_ids) {
		for (const int vertex_id : get_face_corner_vertex_ids(face_id)) {
			selected_vertex_set.insert(vertex_id);
		}
	}
	Vector<int> selected_vertices;
	for (const int vertex_id : selected_vertex_set) {
		selected_vertices.push_back(vertex_id);
	}
	selected_vertices.sort();
	if (selected_vertices.is_empty()) {
		return Ref<LevelMeshDiff>();
	}

	begin_transaction();
	// From this point every failure must restore the exact pre-op snapshot.
	transaction_changed = true;
	HashMap<int, int> duplicates;
	for (const int vertex_id : selected_vertices) {
		duplicates.insert(vertex_id, _append_vertex(data->vertex_positions[vertex_id]));
	}
	for (const int face_id : face_ids) {
		const int loop_start = data->face_loop_starts[face_id];
		const int loop_count = data->face_loop_counts[face_id];
		for (int corner = 0; corner < loop_count; corner++) {
			const int old_vertex = data->loop_vertex_indices[loop_start + corner];
			const int *new_vertex = duplicates.getptr(old_vertex);
			if (!new_vertex) {
				rollback();
				return Ref<LevelMeshDiff>();
			}
			data->loop_vertex_indices.set(loop_start + corner, *new_vertex);
		}
	}
	for (const int edge_id : interior_edges) {
		const int offset = edge_id * 2;
		if (edge_id < 0 || edge_id >= data->edge_alive.size() || offset + 1 >= data->edge_vertices.size()) {
			rollback();
			return Ref<LevelMeshDiff>();
		}
		const int *new_a = duplicates.getptr(data->edge_vertices[offset]);
		const int *new_b = duplicates.getptr(data->edge_vertices[offset + 1]);
		if (!new_a || !new_b) {
			rollback();
			return Ref<LevelMeshDiff>();
		}
		data->edge_vertices.set(offset, *new_a);
		data->edge_vertices.set(offset + 1, *new_b);
	}

	HashSet<int> spoke_vertices;
	int next_polygroup = _next_polygroup_id();
	for (int i = 0; i < rim_edges.size(); i++) {
		const int vertex_a = rim_vertex_a[i];
		const int vertex_b = rim_vertex_b[i];
		const int duplicate_a = *duplicates.getptr(vertex_a);
		const int duplicate_b = *duplicates.getptr(vertex_b);
		_append_edge(duplicate_a, duplicate_b);
		if (!spoke_vertices.has(vertex_a)) {
			spoke_vertices.insert(vertex_a);
			_append_edge(vertex_a, duplicate_a);
		}
		if (!spoke_vertices.has(vertex_b)) {
			spoke_vertices.insert(vertex_b);
			_append_edge(vertex_b, duplicate_b);
		}
		const int owner = rim_owners[i];
		const int wall_vertices[4] = { vertex_a, vertex_b, duplicate_b, duplicate_a };
		const int wall_flags = (data->face_flags[owner] & LevelMeshData::FACE_FLAG_SMOOTH) |
				LevelMeshData::FACE_FLAG_TEXTURE_LOCK;
		const int wall_face = _append_quad_face(wall_vertices, data->face_material_indices[owner], next_polygroup++, wall_flags);
		if (!_initialize_wall_projection(wall_face, owner, vertex_a, vertex_b)) {
			rollback();
			return Ref<LevelMeshDiff>();
		}
	}
	for (const int face_id : face_ids) {
		if (!_reconcile_face_uv(face_id)) {
			rollback();
			return Ref<LevelMeshDiff>();
		}
	}
	_invalidate_topology();
	data->emit_changed();
	return commit();
}

bool LevelMesh::_collect_push_pull(const PackedInt32Array &p_face_ids, real_t p_distance,
		PackedInt32Array &r_vertex_ids, PackedVector3Array &r_positions) const {
	r_vertex_ids.clear();
	r_positions.clear();
	if (p_face_ids.is_empty() || !Math::is_finite(p_distance)) {
		return false;
	}
	HashMap<int, Vector3> normal_sums;
	HashSet<int> selected_faces;
	for (const int face_id : p_face_ids) {
		if (selected_faces.has(face_id)) {
			continue;
		}
		selected_faces.insert(face_id);
		Vector3 origin;
		Vector3 tangent;
		Vector3 bitangent;
		Vector3 normal;
		if (!_compute_face_basis(**data, face_id, true, origin, tangent, bitangent, normal)) {
			return false;
		}
		const int loop_start = data->face_loop_starts[face_id];
		const int loop_count = data->face_loop_counts[face_id];
		for (int corner = 0; corner < loop_count; corner++) {
			const int previous_vertex = data->loop_vertex_indices[loop_start + ((corner + loop_count - 1) % loop_count)];
			const int vertex_id = data->loop_vertex_indices[loop_start + corner];
			const int next_vertex = data->loop_vertex_indices[loop_start + ((corner + 1) % loop_count)];
			const Vector3 incoming = (data->vertex_positions[previous_vertex] - data->vertex_positions[vertex_id]).normalized();
			const Vector3 outgoing = (data->vertex_positions[next_vertex] - data->vertex_positions[vertex_id]).normalized();
			if (incoming.length_squared() <= CMP_EPSILON2 || outgoing.length_squared() <= CMP_EPSILON2) {
				return false;
			}
			const real_t angle = Math::acos(CLAMP(incoming.dot(outgoing), (real_t)-1.0, (real_t)1.0));
			Vector3 *sum = normal_sums.getptr(vertex_id);
			if (sum) {
				*sum += normal * angle;
			} else {
				normal_sums.insert(vertex_id, normal * angle);
			}
		}
	}
	Vector<int> vertex_ids;
	for (const KeyValue<int, Vector3> &entry : normal_sums) {
		vertex_ids.push_back(entry.key);
	}
	vertex_ids.sort();
	for (const int vertex_id : vertex_ids) {
		const Vector3 *normal_sum = normal_sums.getptr(vertex_id);
		if (!normal_sum || normal_sum->length_squared() <= CMP_EPSILON2) {
			return false;
		}
		r_vertex_ids.push_back(vertex_id);
		r_positions.push_back(data->vertex_positions[vertex_id] + normal_sum->normalized() * p_distance);
	}
	return !r_vertex_ids.is_empty();
}

Ref<LevelMeshDiff> LevelMesh::push_pull_faces(const PackedInt32Array &p_face_ids, real_t p_distance) {
	if (transaction_active || transform_preview_active) {
		return Ref<LevelMeshDiff>();
	}
	PackedInt32Array vertex_ids;
	PackedVector3Array positions;
	if (!_collect_push_pull(p_face_ids, p_distance, vertex_ids, positions) || !begin_transform_preview(vertex_ids)) {
		return Ref<LevelMeshDiff>();
	}
	if (!preview_transform_vertices(positions)) {
		cancel_transform_preview();
		return Ref<LevelMeshDiff>();
	}
	return commit_transform_preview();
}

Dictionary LevelMesh::calculate_push_pull(const PackedInt32Array &p_face_ids, real_t p_distance) const {
	Dictionary result;
	PackedInt32Array vertex_ids;
	PackedVector3Array positions;
	const bool valid = !transaction_active && !transform_preview_active &&
			_collect_push_pull(p_face_ids, p_distance, vertex_ids, positions);
	result["valid"] = valid;
	result["vertex_ids"] = valid ? vertex_ids : PackedInt32Array();
	result["positions"] = valid ? positions : PackedVector3Array();
	return result;
}

Ref<LevelMeshDiff> LevelMesh::extrude_boundary_edges(const PackedInt32Array &p_edge_ids) {
	if (transaction_active || transform_preview_active || p_edge_ids.is_empty() || !adjacency->is_valid()) {
		return Ref<LevelMeshDiff>();
	}
	PackedInt32Array edge_ids;
	Vector<int> owners;
	Vector<int> vertex_a;
	Vector<int> vertex_b;
	for (const int edge_id : p_edge_ids) {
		if (packed_contains(edge_ids, edge_id)) {
			continue;
		}
		const PackedInt32Array edge_faces = adjacency->get_edge_faces(edge_id);
		if (edge_faces.size() != 1) {
			return Ref<LevelMeshDiff>();
		}
		int directed_a = -1;
		int directed_b = -1;
		if (!_get_face_directed_edge(edge_faces[0], edge_id, directed_a, directed_b)) {
			return Ref<LevelMeshDiff>();
		}
		edge_ids.push_back(edge_id);
		owners.push_back(edge_faces[0]);
		vertex_a.push_back(directed_a);
		vertex_b.push_back(directed_b);
	}

	HashSet<int> vertex_set;
	for (int i = 0; i < edge_ids.size(); i++) {
		vertex_set.insert(vertex_a[i]);
		vertex_set.insert(vertex_b[i]);
	}
	Vector<int> vertices;
	for (const int vertex_id : vertex_set) {
		vertices.push_back(vertex_id);
	}
	vertices.sort();
	begin_transaction();
	// From this point every failure must restore the exact pre-op snapshot.
	transaction_changed = true;
	HashMap<int, int> duplicates;
	for (const int vertex_id : vertices) {
		duplicates.insert(vertex_id, _append_vertex(data->vertex_positions[vertex_id]));
	}
	HashSet<int> spoke_vertices;
	int next_polygroup = _next_polygroup_id();
	for (int i = 0; i < edge_ids.size(); i++) {
		const int duplicate_a = *duplicates.getptr(vertex_a[i]);
		const int duplicate_b = *duplicates.getptr(vertex_b[i]);
		_append_edge(duplicate_a, duplicate_b);
		if (!spoke_vertices.has(vertex_a[i])) {
			spoke_vertices.insert(vertex_a[i]);
			_append_edge(vertex_a[i], duplicate_a);
		}
		if (!spoke_vertices.has(vertex_b[i])) {
			spoke_vertices.insert(vertex_b[i]);
			_append_edge(vertex_b[i], duplicate_b);
		}
		const int wall_vertices[4] = { vertex_a[i], vertex_b[i], duplicate_b, duplicate_a };
		const int owner = owners[i];
		const int wall_flags = (data->face_flags[owner] & LevelMeshData::FACE_FLAG_SMOOTH) |
				LevelMeshData::FACE_FLAG_TEXTURE_LOCK;
		const int wall_face = _append_quad_face(wall_vertices, data->face_material_indices[owner], next_polygroup++, wall_flags);
		if (!_initialize_wall_projection(wall_face, owner, vertex_a[i], vertex_b[i])) {
			rollback();
			return Ref<LevelMeshDiff>();
		}
	}
	for (const int owner : owners) {
		if (!_reconcile_face_uv(owner)) {
			rollback();
			return Ref<LevelMeshDiff>();
		}
	}
	_invalidate_topology();
	data->emit_changed();
	return commit();
}
