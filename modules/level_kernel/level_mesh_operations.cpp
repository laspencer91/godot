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

} // namespace

bool LevelMesh::_compute_face_basis(const LevelMeshData &p_data, int p_face_id, bool p_canonical_tangent,
		Vector3 &r_origin, Vector3 &r_tangent, Vector3 &r_bitangent, Vector3 &r_normal) {
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

	r_origin = Vector3();
	r_normal = Vector3();
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
		r_origin += current;
		// Newell's method remains stable for mildly non-planar edited faces.
		r_normal.x += (current.y - next.y) * (current.z + next.z);
		r_normal.y += (current.z - next.z) * (current.x + next.x);
		r_normal.z += (current.x - next.x) * (current.y + next.y);
	}
	if (r_normal.length_squared() <= CMP_EPSILON2) {
		return false;
	}
	r_origin /= (real_t)loop_count;
	r_normal.normalize();

	r_tangent = Vector3();
	if (!p_canonical_tangent && p_face_id < p_data.face_uv_tangents.size()) {
		r_tangent = p_data.face_uv_tangents[p_face_id];
		r_tangent -= r_normal * r_normal.dot(r_tangent);
	}
	if (r_tangent.length_squared() <= CMP_EPSILON2 && p_canonical_tangent) {
		for (int corner = 0; corner < loop_count; corner++) {
			const int vertex_a = p_data.loop_vertex_indices[loop_start + corner];
			const int vertex_b = p_data.loop_vertex_indices[loop_start + ((corner + 1) % loop_count)];
			r_tangent = p_data.vertex_positions[vertex_b] - p_data.vertex_positions[vertex_a];
			r_tangent -= r_normal * r_normal.dot(r_tangent);
			if (r_tangent.length_squared() > CMP_EPSILON2) {
				break;
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
	Vector3 origin;
	Vector3 tangent;
	Vector3 bitangent;
	Vector3 normal;
	if (!_compute_face_basis(**data, p_face_id, false, origin, tangent, bitangent, normal)) {
		return false;
	}
	data->face_uv_tangents.set(p_face_id, tangent);
	LevelMeshData::_write_uv_transform(**data, p_face_id, Transform2D());
	return _reconcile_face_uv(p_face_id);
}

bool LevelMesh::_solve_texture_lock(int p_face_id, const LevelMeshData &p_before) {
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

	const bool projected = p_face_id < data->face_uv_modes.size() &&
			data->face_uv_modes[p_face_id] == LevelMeshData::UV_MODE_PROJECTED;
	const bool texture_locked = p_face_id < data->face_flags.size() &&
			(data->face_flags[p_face_id] & LevelMeshData::FACE_FLAG_TEXTURE_LOCK) != 0;
	if (!projected || !texture_locked) {
		return _reconcile_face_uv(p_face_id);
	}

	Vector3 old_basis_origin;
	Vector3 old_tangent;
	Vector3 old_bitangent;
	Vector3 old_normal;
	if (!_compute_face_basis(p_before, p_face_id, true, old_basis_origin, old_tangent, old_bitangent, old_normal)) {
		// A zero-distance extrude wall has no pre-transform plane. Its first
		// non-degenerate preview frame receives the standard fresh projection.
		return _initialize_face_projection(p_face_id);
	}
	Vector3 new_basis_origin;
	Vector3 new_tangent;
	Vector3 new_bitangent;
	Vector3 new_normal;
	if (!_compute_face_basis(**data, p_face_id, true, new_basis_origin, new_tangent, new_bitangent, new_normal)) {
		return false;
	}

	Vector<Vector3> old_positions;
	Vector<Vector3> new_positions;
	Vector<Vector2> target_uvs;
	Vector<Vector2> projected_positions;
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
		const Vector3 relative = new_positions[corner] - new_basis_origin;
		projected_positions.write[corner] = Vector2(relative.dot(new_tangent), relative.dot(new_bitangent));
	}

	const Vector3 translation = new_positions[0] - old_positions[0];
	bool is_translation = true;
	for (int corner = 1; corner < loop_count; corner++) {
		if (!(new_positions[corner] - old_positions[corner]).is_equal_approx(translation)) {
			is_translation = false;
			break;
		}
	}
	if (is_translation && p_face_id < p_before.face_uv_origins.size() && p_face_id < p_before.face_uv_tangents.size()) {
		data->face_uv_origins.set(p_face_id, p_before.face_uv_origins[p_face_id] + translation);
		data->face_uv_tangents.set(p_face_id, p_before.face_uv_tangents[p_face_id]);
		LevelMeshData::_write_uv_transform(**data, p_face_id, LevelMeshData::_read_uv_transform(p_before, p_face_id));
		return _reconcile_face_uv(p_face_id);
	}

	bool is_rigid = true;
	for (int a = 0; a < loop_count && is_rigid; a++) {
		for (int b = a + 1; b < loop_count; b++) {
			const real_t old_length_squared = old_positions[a].distance_squared_to(old_positions[b]);
			const real_t new_length_squared = new_positions[a].distance_squared_to(new_positions[b]);
			const real_t tolerance = MAX((real_t)1.0, MAX(old_length_squared, new_length_squared)) * (real_t)1e-5;
			if (Math::abs(old_length_squared - new_length_squared) > tolerance) {
				is_rigid = false;
				break;
			}
		}
	}

	data->face_uv_origins.set(p_face_id, new_basis_origin);
	data->face_uv_tangents.set(p_face_id, new_tangent);
	if (is_rigid) {
		int second = 1;
		real_t longest = 0.0;
		for (int corner = 1; corner < loop_count; corner++) {
			const real_t distance_squared = projected_positions[corner].distance_squared_to(projected_positions[0]);
			if (distance_squared > longest) {
				longest = distance_squared;
				second = corner;
			}
		}
		int third = -1;
		real_t largest_area = 0.0;
		const Vector2 first_edge = projected_positions[second] - projected_positions[0];
		for (int corner = 1; corner < loop_count; corner++) {
			if (corner == second) {
				continue;
			}
			const real_t area = Math::abs(first_edge.cross(projected_positions[corner] - projected_positions[0]));
			if (area > largest_area) {
				largest_area = area;
				third = corner;
			}
		}
		if (third >= 0 && largest_area > CMP_EPSILON2) {
			const Vector2 q1 = projected_positions[second] - projected_positions[0];
			const Vector2 q2 = projected_positions[third] - projected_positions[0];
			const Vector2 d1 = target_uvs[second] - target_uvs[0];
			const Vector2 d2 = target_uvs[third] - target_uvs[0];
			const real_t determinant = q1.cross(q2);
			const real_t m00 = (d1.x * q2.y - d2.x * q1.y) / determinant;
			const real_t m01 = (-d1.x * q2.x + d2.x * q1.x) / determinant;
			const real_t m10 = (d1.y * q2.y - d2.y * q1.y) / determinant;
			const real_t m11 = (-d1.y * q2.x + d2.y * q1.x) / determinant;
			const Vector2 offset = target_uvs[0] - Vector2(m00 * projected_positions[0].x + m01 * projected_positions[0].y, m10 * projected_positions[0].x + m11 * projected_positions[0].y);
			LevelMeshData::_write_uv_transform(**data, p_face_id,
					Transform2D(m00, m10, m01, m11, offset.x, offset.y));
			return _reconcile_face_uv(p_face_id);
		}
	}

	Vector2 mean_position;
	Vector2 mean_uv;
	for (int corner = 0; corner < loop_count; corner++) {
		mean_position += projected_positions[corner];
		mean_uv += target_uvs[corner];
	}
	mean_position /= (real_t)loop_count;
	mean_uv /= (real_t)loop_count;
	real_t a00 = 0.0;
	real_t a01 = 0.0;
	real_t a11 = 0.0;
	real_t b00 = 0.0;
	real_t b01 = 0.0;
	real_t b10 = 0.0;
	real_t b11 = 0.0;
	for (int corner = 0; corner < loop_count; corner++) {
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
	const real_t scale = MAX((real_t)1.0, (a00 + a11) * (a00 + a11));
	Transform2D solved;
	if (Math::abs(determinant) > scale * (real_t)1e-10) {
		const real_t inv00 = a11 / determinant;
		const real_t inv01 = -a01 / determinant;
		const real_t inv11 = a00 / determinant;
		const real_t m00 = b00 * inv00 + b01 * inv01;
		const real_t m01 = b00 * inv01 + b01 * inv11;
		const real_t m10 = b10 * inv00 + b11 * inv01;
		const real_t m11 = b10 * inv01 + b11 * inv11;
		const Vector2 offset = mean_uv - Vector2(m00 * mean_position.x + m01 * mean_position.y, m10 * mean_position.x + m11 * mean_position.y);
		solved = Transform2D(m00, m10, m01, m11, offset.x, offset.y);
	} else {
		// Near-degenerate faces keep a finite linear map and fit only the offset.
		const Transform2D old_transform = LevelMeshData::_read_uv_transform(p_before, p_face_id);
		solved = Transform2D(old_transform[0], old_transform[1],
				mean_uv - old_transform[0] * mean_position.x - old_transform[1] * mean_position.y);
	}
	LevelMeshData::_write_uv_transform(**data, p_face_id, solved);
	return _reconcile_face_uv(p_face_id);
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
	for (const int face_id : transform_preview_face_ids) {
		if (face_id < 0 || face_id >= data->face_loop_starts.size() || face_id >= transaction_before->face_loop_starts.size() ||
				face_id >= data->face_uv_origins.size() || face_id >= transaction_before->face_uv_origins.size() ||
				face_id >= data->face_uv_tangents.size() || face_id >= transaction_before->face_uv_tangents.size()) {
			return false;
		}
		data->face_uv_origins.set(face_id, transaction_before->face_uv_origins[face_id]);
		data->face_uv_tangents.set(face_id, transaction_before->face_uv_tangents[face_id]);
		LevelMeshData::_write_uv_transform(**data, face_id, LevelMeshData::_read_uv_transform(**transaction_before, face_id));
		const int loop_start = data->face_loop_starts[face_id];
		const int before_loop_start = transaction_before->face_loop_starts[face_id];
		const int loop_count = data->face_loop_counts[face_id];
		if (loop_count != transaction_before->face_loop_counts[face_id] || loop_start < 0 || before_loop_start < 0 ||
				loop_start > data->loop_uv0.size() - loop_count || before_loop_start > transaction_before->loop_uv0.size() - loop_count ||
				loop_start > data->loop_normals.size() - loop_count || before_loop_start > transaction_before->loop_normals.size() - loop_count) {
			return false;
		}
		for (int corner = 0; corner < loop_count; corner++) {
			data->loop_uv0.set(loop_start + corner, transaction_before->loop_uv0[before_loop_start + corner]);
			data->loop_normals.set(loop_start + corner, transaction_before->loop_normals[before_loop_start + corner]);
		}
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
	for (const int face_id : transform_preview_face_ids) {
		if (!_solve_texture_lock(face_id, **transaction_before)) {
			_restore_transform_preview_baseline();
			transaction_changed = false;
			return false;
		}
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
		_append_quad_face(wall_vertices, data->face_material_indices[owner], next_polygroup++, data->face_flags[owner]);
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
		_append_quad_face(wall_vertices, data->face_material_indices[owners[i]], next_polygroup++, data->face_flags[owners[i]]);
	}
	_invalidate_topology();
	data->emit_changed();
	return commit();
}
