/**************************************************************************/
/*  level_mesh.cpp                                                        */
/**************************************************************************/

#include "level_mesh.h"

#include "level_mesh_adjacency.h"
#include "level_mesh_data.h"
#include "level_mesh_diff.h"
#include "level_mesh_element_bvh.h"

#include "core/math/math_funcs.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"

Vector3 LevelMesh::_dominant_axis_tangent(const Vector3 &p_normal) {
	const Vector3 absolute_normal = p_normal.abs();
	// Same sign table as grid_uv_tangent_for_normal so every default frame —
	// create_box, the no-frame fallback, and Align-to-Grid — agrees; a split
	// convention mirrors textures between fresh boxes and aligned/extruded faces.
	Vector3 world_tangent;
	if (absolute_normal.x >= absolute_normal.y && absolute_normal.x >= absolute_normal.z) {
		world_tangent = Vector3(0, 0, -1);
	} else {
		world_tangent = Vector3(-1, 0, 0);
	}

	// Project the chosen world axis onto the actual face plane. For axis-aligned
	// faces this remains an exact cardinal axis; sloped frames stay orthonormal.
	Vector3 tangent = world_tangent - p_normal * p_normal.dot(world_tangent);
	if (tangent.length_squared() <= CMP_EPSILON2) {
		world_tangent = Vector3(0, -1, 0);
		tangent = world_tangent - p_normal * p_normal.dot(world_tangent);
	}
	return tangent.normalized();
}

Vector3 LevelMesh::grid_uv_tangent_for_normal(const Vector3 &p_normal) {
	if (!p_normal.is_finite() || p_normal.length_squared() <= CMP_EPSILON2) {
		return Vector3();
	}
	const Vector3 absolute_normal = p_normal.abs();
	if (absolute_normal.x >= absolute_normal.y && absolute_normal.x >= absolute_normal.z) {
		return Vector3(0, 0, -1);
	}
	return Vector3(-1, 0, 0);
}

void LevelMesh::_append_uv_transform(LevelMeshData &r_data, const Transform2D &p_transform) {
	r_data.face_uv_transforms.push_back((float)p_transform[0].x);
	r_data.face_uv_transforms.push_back((float)p_transform[0].y);
	r_data.face_uv_transforms.push_back((float)p_transform[1].x);
	r_data.face_uv_transforms.push_back((float)p_transform[1].y);
	r_data.face_uv_transforms.push_back((float)p_transform[2].x);
	r_data.face_uv_transforms.push_back((float)p_transform[2].y);
}

void LevelMesh::_invalidate_topology() {
	if (adjacency.is_valid()) {
		adjacency->_mark_dirty();
	}
	_invalidate_geometry();
}

void LevelMesh::_invalidate_geometry() {
	if (element_bvh.is_valid()) {
		element_bvh->_mark_dirty();
	}
}

void LevelMesh::_on_data_changed() {
	// Raw Resource setters are intentionally available to importers and tests.
	// Without this guard they could bypass transaction/cache invalidation.
	if (geometry_change_notification) {
		_invalidate_geometry();
	} else {
		_invalidate_topology();
	}
	if (transaction_active) {
		transaction_changed = true;
	}
}

int LevelMesh::_next_polygroup_id() const {
	int next_id = 0;
	const int face_rows = MIN(data->face_polygroup_ids.size(), data->face_alive.size());
	for (int face_id = 0; face_id < face_rows; face_id++) {
		if (data->face_alive[face_id] != 0) {
			next_id = MAX(next_id, data->face_polygroup_ids[face_id] + 1);
		}
	}
	return next_id;
}

bool LevelMesh::_get_projection_basis(const LevelMeshData &p_data, int p_face_id,
		Vector3 &r_tangent, Vector3 &r_bitangent, Vector3 &r_normal) {
	if (p_face_id < 0 || p_face_id >= p_data.face_uv_tangents.size()) {
		return false;
	}
	Vector3 centroid;
	real_t area_x2 = 0.0;
	real_t longest_edge_squared = 0.0;
	if (!_compute_face_geometry(p_data, p_face_id, centroid, r_normal, area_x2, longest_edge_squared)) {
		return false;
	}
	r_tangent = p_data.face_uv_tangents[p_face_id];
	if (!r_tangent.is_finite() || r_tangent.length_squared() <= CMP_EPSILON2) {
		return false;
	}
	r_tangent.normalize();
	r_bitangent = r_normal.cross(r_tangent);
	if (!r_bitangent.is_finite() || r_bitangent.length_squared() <= CMP_EPSILON2) {
		// A Planar island deliberately stores one tangent verbatim on every
		// member. On a non-coplanar island that shared tangent can coincide with
		// one member's live normal; retain the stored recipe and use the same
		// deterministic box tangent as the effective projection basis there.
		r_tangent = _dominant_axis_tangent(r_normal);
		r_bitangent = r_normal.cross(r_tangent);
		if (!r_tangent.is_finite() || r_tangent.length_squared() <= CMP_EPSILON2 ||
				!r_bitangent.is_finite() || r_bitangent.length_squared() <= CMP_EPSILON2) {
			return false;
		}
	}
	r_bitangent.normalize();
	return true;
}

bool LevelMesh::_sample_explicit_uv(const LevelMeshData &p_data, int p_face_id, const Vector3 &p_point,
		int p_loop_id, Vector2 &r_uv) {
	if (!p_point.is_finite() || p_face_id < 0 || p_face_id >= p_data.face_alive.size() ||
			p_data.face_alive[p_face_id] == 0 || p_face_id >= p_data.face_loop_starts.size() ||
			p_face_id >= p_data.face_loop_counts.size()) {
		return false;
	}
	const int loop_start = p_data.face_loop_starts[p_face_id];
	const int loop_count = p_data.face_loop_counts[p_face_id];
	if (loop_count < 3 || loop_start < 0 || loop_start > p_data.loop_vertex_indices.size() - loop_count ||
			loop_start > p_data.loop_uv0.size() - loop_count || loop_start > p_data.loop_alive.size() - loop_count) {
		return false;
	}
	if (p_loop_id >= loop_start && p_loop_id < loop_start + loop_count && p_data.loop_alive[p_loop_id] != 0) {
		r_uv = p_data.loop_uv0[p_loop_id];
		return true;
	}

	for (int corner = 0; corner < loop_count; corner++) {
		const int loop_id = loop_start + corner;
		const int vertex_id = p_data.loop_vertex_indices[loop_id];
		if (p_data.loop_alive[loop_id] == 0 || vertex_id < 0 || vertex_id >= p_data.vertex_positions.size()) {
			return false;
		}
		if (p_data.vertex_positions[vertex_id].distance_squared_to(p_point) <= CMP_EPSILON2) {
			r_uv = p_data.loop_uv0[loop_id];
			return true;
		}
	}

	const int first_vertex = p_data.loop_vertex_indices[loop_start];
	const Vector3 p0 = p_data.vertex_positions[first_vertex];
	for (int corner = 1; corner < loop_count - 1; corner++) {
		const int loop1 = loop_start + corner;
		const int loop2 = loop_start + corner + 1;
		const Vector3 p1 = p_data.vertex_positions[p_data.loop_vertex_indices[loop1]];
		const Vector3 p2 = p_data.vertex_positions[p_data.loop_vertex_indices[loop2]];
		const Vector3 edge0 = p1 - p0;
		const Vector3 edge1 = p2 - p0;
		const Vector3 relative = p_point - p0;
		const real_t d00 = edge0.dot(edge0);
		const real_t d01 = edge0.dot(edge1);
		const real_t d11 = edge1.dot(edge1);
		const real_t d20 = relative.dot(edge0);
		const real_t d21 = relative.dot(edge1);
		const real_t denominator = d00 * d11 - d01 * d01;
		if (Math::abs(denominator) <= CMP_EPSILON2) {
			continue;
		}
		const real_t weight1 = (d11 * d20 - d01 * d21) / denominator;
		const real_t weight2 = (d00 * d21 - d01 * d20) / denominator;
		const real_t weight0 = (real_t)1.0 - weight1 - weight2;
		const Vector3 reconstructed = p0 * weight0 + p1 * weight1 + p2 * weight2;
		const real_t triangle_scale = MAX((real_t)1.0, MAX(d00, d11));
		const real_t tolerance = (real_t)1e-5;
		if (weight0 >= -tolerance && weight1 >= -tolerance && weight2 >= -tolerance &&
				reconstructed.distance_squared_to(p_point) <= triangle_scale * tolerance * tolerance) {
			r_uv = p_data.loop_uv0[loop_start] * weight0 + p_data.loop_uv0[loop1] * weight1 + p_data.loop_uv0[loop2] * weight2;
			return r_uv.is_finite();
		}
	}
	return false;
}

Vector2 LevelMesh::project_native(int p_face_id, const Vector3 &p_point) const {
	ERR_FAIL_COND_V(!p_point.is_finite(), Vector2());
	ERR_FAIL_INDEX_V(p_face_id, data->face_uv_origins.size(), Vector2());
	Vector3 tangent;
	Vector3 bitangent;
	Vector3 normal;
	ERR_FAIL_COND_V_MSG(!_get_projection_basis(**data, p_face_id, tangent, bitangent, normal), Vector2(),
			"Cannot project UVs for an invalid or degenerate face frame.");
	const Vector3 relative = p_point - data->face_uv_origins[p_face_id];
	return Vector2(relative.dot(tangent), relative.dot(bitangent));
}

Vector2 LevelMesh::get_uv(int p_face_id, const Vector3 &p_point, int p_loop_id) const {
	ERR_FAIL_INDEX_V(p_face_id, data->face_uv_modes.size(), Vector2());
	if (data->face_uv_modes[p_face_id] == LevelMeshData::UV_MODE_PROJECTED) {
		return LevelMeshData::_read_uv_transform(**data, p_face_id).xform(project_native(p_face_id, p_point));
	}
	ERR_FAIL_COND_V_MSG(data->face_uv_modes[p_face_id] != LevelMeshData::UV_MODE_EXPLICIT, Vector2(),
			"Face has an unknown UV mode.");
	Vector2 uv;
	ERR_FAIL_COND_V_MSG(!_sample_explicit_uv(**data, p_face_id, p_point, p_loop_id, uv), Vector2(),
			"Point cannot be sampled from the explicit face UVs.");
	return uv;
}

int LevelMesh::get_face_uv_mode(int p_face_id) const {
	return data->get_face_uv_mode(p_face_id);
}

Vector3 LevelMesh::get_face_uv_origin(int p_face_id) const {
	return data->get_face_uv_origin(p_face_id);
}

Vector3 LevelMesh::get_face_uv_tangent(int p_face_id) const {
	return data->get_face_uv_tangent(p_face_id);
}

Transform2D LevelMesh::get_face_uv_transform(int p_face_id) const {
	return data->get_face_uv_transform(p_face_id);
}

bool LevelMesh::_reconcile_face_uv(int p_face_id, const LevelMeshData *p_before,
		int p_source_face_id, bool p_transfer_explicit, const Vector3 *p_normal_override) {
	if (p_face_id < 0 || p_face_id >= data->face_alive.size() || data->face_alive[p_face_id] == 0 ||
			p_face_id >= data->face_loop_starts.size() || p_face_id >= data->face_loop_counts.size() ||
			p_face_id >= data->face_uv_modes.size() || p_face_id >= data->face_uv_origins.size() ||
			p_face_id >= data->face_uv_tangents.size()) {
		return false;
	}

	const int loop_start = data->face_loop_starts[p_face_id];
	const int loop_count = data->face_loop_counts[p_face_id];
	if (loop_count < 3 || loop_start < 0 || loop_start + loop_count > data->loop_vertex_indices.size() ||
			loop_start + loop_count > data->loop_uv0.size() || loop_start + loop_count > data->loop_normals.size() ||
			loop_start + loop_count > data->loop_alive.size()) {
		return false;
	}

	Vector3 centroid;
	Vector3 tangent;
	Vector3 bitangent;
	Vector3 normal;
	real_t area_x2 = 0.0;
	real_t longest_edge_squared = 0.0;
	const bool has_geometry = _compute_face_geometry(**data, p_face_id, centroid, normal, area_x2, longest_edge_squared);
	if (!has_geometry) {
		if (p_normal_override == nullptr || !p_normal_override->is_finite() ||
				p_normal_override->length_squared() <= CMP_EPSILON2) {
			return false;
		}
		normal = p_normal_override->normalized();
	}

	const bool projected = data->face_uv_modes[p_face_id] == LevelMeshData::UV_MODE_PROJECTED;
	if (!projected && data->face_uv_modes[p_face_id] != LevelMeshData::UV_MODE_EXPLICIT) {
		return false;
	}
	const Vector3 origin = data->face_uv_origins[p_face_id];
	const Transform2D uv_transform = LevelMeshData::_read_uv_transform(**data, p_face_id);
	if (projected) {
		if (has_geometry) {
			if (!_get_projection_basis(**data, p_face_id, tangent, bitangent, normal)) {
				return false;
			}
		} else {
			tangent = data->face_uv_tangents[p_face_id];
			if (!tangent.is_finite() || tangent.length_squared() <= CMP_EPSILON2) {
				return false;
			}
			tangent.normalize();
			bitangent = normal.cross(tangent);
			if (!bitangent.is_finite() || bitangent.length_squared() <= CMP_EPSILON2) {
				return false;
			}
			bitangent.normalize();
		}
		if (!uv_transform.is_finite()) {
			return false;
		}
	}

	Vector<Vector2> transferred_uvs;
	if (!projected && p_transfer_explicit) {
		if (p_before == nullptr || p_source_face_id < 0) {
			return false;
		}
		transferred_uvs.resize(loop_count);
		for (int corner = 0; corner < loop_count; corner++) {
			const int vertex_id = data->loop_vertex_indices[loop_start + corner];
			if (vertex_id < 0 || vertex_id >= data->vertex_positions.size() ||
					!_sample_explicit_uv(*p_before, p_source_face_id, data->vertex_positions[vertex_id], -1, transferred_uvs.write[corner])) {
				return false;
			}
		}
	}
	for (int corner = 0; corner < loop_count; corner++) {
		const int loop_id = loop_start + corner;
		if (data->loop_alive[loop_id] == 0) {
			continue;
		}
		const int vertex_id = data->loop_vertex_indices[loop_id];
		if (vertex_id < 0 || vertex_id >= data->vertex_positions.size()) {
			return false;
		}
		data->loop_normals.set(loop_id, normal);
		if (projected) {
			const Vector3 relative_position = data->vertex_positions[vertex_id] - origin;
			const Vector2 projected_uv(relative_position.dot(tangent), relative_position.dot(bitangent));
			const Vector2 uv = uv_transform.xform(projected_uv);
			if (!uv.is_finite()) {
				return false;
			}
			data->loop_uv0.set(loop_id, uv);
		} else if (p_transfer_explicit) {
			data->loop_uv0.set(loop_id, transferred_uvs[corner]);
		}
	}

	return true;
}

void LevelMesh::set_data(const Ref<LevelMeshData> &p_data) {
	ERR_FAIL_COND_MSG(transaction_active || transform_preview_active, "Cannot replace LevelMesh data during a transaction or transform preview.");
	if (data.is_valid()) {
		data->disconnect_changed(callable_mp(this, &LevelMesh::_on_data_changed));
	}
	if (p_data.is_valid()) {
		data = p_data;
	} else {
		data.instantiate();
	}
	data->_ensure_generation_columns();
	data->connect_changed(callable_mp(this, &LevelMesh::_on_data_changed));
	adjacency->_set_data(data);
	element_bvh->_set_data(data);
	last_hotspot_fit_diagnostics.clear();
}

Ref<LevelMeshData> LevelMesh::get_data() const {
	return data;
}

void LevelMesh::begin_transaction() {
	ERR_FAIL_COND_MSG(transaction_active || transform_preview_active, "LevelMesh already has an active transaction or transform preview.");
	transaction_before = data->duplicate_data();
	transaction_active = true;
	transaction_changed = false;
}

Ref<LevelMeshDiff> LevelMesh::commit() {
	ERR_FAIL_COND_V_MSG(!transaction_active, Ref<LevelMeshDiff>(), "LevelMesh has no active transaction to commit.");

	Ref<LevelMeshData> before = transaction_before;
	transaction_before.unref();
	transaction_active = false;

	if (!transaction_changed) {
		return Ref<LevelMeshDiff>();
	}

	Ref<LevelMeshDiff> diff;
	diff.instantiate();
	diff->_set_states(before, data->duplicate_data(), false);
	if (diff->topology_changed) {
		_invalidate_topology();
	} else if (diff->geometry_changed) {
		_invalidate_geometry();
	}
	transaction_changed = false;
	data->_emit_mesh_diff_applied(diff, false);
	return diff;
}

void LevelMesh::rollback() {
	ERR_FAIL_COND_MSG(!transaction_active, "LevelMesh has no active transaction to roll back.");
	if (transaction_changed) {
		data->_copy_from(**transaction_before, true);
		_invalidate_topology();
	}
	transaction_before.unref();
	transaction_active = false;
	transaction_changed = false;
}

bool LevelMesh::is_transaction_active() const {
	return transaction_active;
}

bool LevelMesh::create_box(const Transform3D &p_frame, const Vector3 &p_size, int p_material_index) {
	if (!transaction_active || !p_frame.is_finite() || !p_size.is_finite() || p_material_index < 0 ||
			p_size.x <= CMP_EPSILON || p_size.y <= CMP_EPSILON || p_size.z <= CMP_EPSILON ||
			!p_frame.basis.is_orthogonal() || !Math::is_equal_approx(p_frame.basis.determinant(), (real_t)1.0)) {
		return false;
	}

	const Vector3 half_size = p_size * (real_t)0.5;
	const Vector3 local_vertices[8] = {
		Vector3(-half_size.x, -half_size.y, -half_size.z),
		Vector3(half_size.x, -half_size.y, -half_size.z),
		Vector3(half_size.x, half_size.y, -half_size.z),
		Vector3(-half_size.x, half_size.y, -half_size.z),
		Vector3(-half_size.x, -half_size.y, half_size.z),
		Vector3(half_size.x, -half_size.y, half_size.z),
		Vector3(half_size.x, half_size.y, half_size.z),
		Vector3(-half_size.x, half_size.y, half_size.z),
	};

	const int vertex_base = data->vertex_positions.size();
	for (const Vector3 &local_vertex : local_vertices) {
		data->vertex_positions.push_back(p_frame.xform(local_vertex));
		data->vertex_alive.push_back(1);
		data->vertex_generations.push_back((int32_t)data->_claim_vertex_generation());
	}

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
	for (const auto &edge : BOX_EDGES) {
		data->edge_vertices.push_back(vertex_base + edge[0]);
		data->edge_vertices.push_back(vertex_base + edge[1]);
		data->edge_alive.push_back(1);
		data->edge_generations.push_back((int32_t)data->_claim_edge_generation());
	}

	static constexpr int BOX_FACES[6][4] = {
		{ 0, 3, 2, 1 }, // -Z
		{ 4, 5, 6, 7 }, // +Z
		{ 0, 4, 7, 3 }, // -X
		{ 1, 2, 6, 5 }, // +X
		{ 0, 1, 5, 4 }, // -Y
		{ 3, 7, 6, 2 }, // +Y
	};
	const int first_face = data->face_loop_starts.size();
	// Each box face is its own polygroup: polygroups partition contiguous coplanar
	// regions, and a shared id across the six non-coplanar faces would make every
	// polygroup-tier face pick a closed region that region ops (extrude) must reject.
	int polygroup_id = _next_polygroup_id();
	for (const auto &face : BOX_FACES) {
		int face_vertices[4];
		for (int corner = 0; corner < 4; corner++) {
			face_vertices[corner] = vertex_base + face[corner];
		}
		_append_quad_face(face_vertices, p_material_index, polygroup_id++, LevelMeshData::FACE_FLAG_TEXTURE_LOCK);
	}

	for (int face_id = first_face; face_id < first_face + 6; face_id++) {
		Vector3 centroid;
		Vector3 normal;
		real_t area_x2 = 0.0;
		real_t longest_edge_squared = 0.0;
		if (!_compute_face_geometry(**data, face_id, centroid, normal, area_x2, longest_edge_squared)) {
			data->_copy_from(**transaction_before, true);
			transaction_changed = false;
			return false;
		}
		// LE0 serialized boxes used the positive seed convention. Preserve that
		// existing creation behavior so old content and smoke baselines do not
		// rotate, while the additive Align-to-Grid operation below implements the
		// LE2/Cyclops negative-axis convention.
		data->face_uv_origins.set(face_id, Vector3());
		data->face_uv_tangents.set(face_id, _dominant_axis_tangent(normal));
		LevelMeshData::_write_uv_transform(**data, face_id, Transform2D());
		if (!_reconcile_face_uv(face_id)) {
			// Inputs are validated and box faces are fixed quads, so this is only a
			// defensive guard. Restore the transaction's exact starting state.
			data->_copy_from(**transaction_before, true);
			transaction_changed = false;
			return false;
		}
	}

	transaction_changed = true;
	data->emit_changed();
	return true;
}

bool LevelMesh::reconcile_face_uv(int p_face_id) {
	const bool reconciled = _reconcile_face_uv(p_face_id);
	if (reconciled) {
		data->emit_changed();
		if (transaction_active) {
			transaction_changed = true;
		}
	}
	return reconciled;
}

Ref<LevelMeshDiff> LevelMesh::align_faces_to_grid(const PackedInt32Array &p_face_ids) {
	if (transaction_active || transform_preview_active || p_face_ids.is_empty()) {
		return Ref<LevelMeshDiff>();
	}
	PackedInt32Array face_ids;
	for (const int face_id : p_face_ids) {
		Vector3 centroid;
		Vector3 normal;
		real_t area_x2 = 0.0;
		real_t longest_edge_squared = 0.0;
		if (face_ids.has(face_id)) {
			continue;
		}
		if (!_compute_face_geometry(**data, face_id, centroid, normal, area_x2, longest_edge_squared)) {
			return Ref<LevelMeshDiff>();
		}
		face_ids.push_back(face_id);
	}
	begin_transaction();
	for (const int face_id : face_ids) {
		if (!_set_grid_frame(face_id, true)) {
			rollback();
			return Ref<LevelMeshDiff>();
		}
	}
	transaction_changed = true;
	geometry_change_notification = true;
	data->emit_changed();
	geometry_change_notification = false;
	return commit();
}

Ref<LevelMeshDiff> LevelMesh::align_faces_to_face(const PackedInt32Array &p_face_ids) {
	if (transaction_active || transform_preview_active || p_face_ids.is_empty()) {
		return Ref<LevelMeshDiff>();
	}
	PackedInt32Array face_ids;
	for (const int face_id : p_face_ids) {
		Vector3 centroid;
		Vector3 normal;
		real_t area_x2 = 0.0;
		real_t longest_edge_squared = 0.0;
		if (face_ids.has(face_id)) {
			continue;
		}
		if (!_compute_face_geometry(**data, face_id, centroid, normal, area_x2, longest_edge_squared)) {
			return Ref<LevelMeshDiff>();
		}
		face_ids.push_back(face_id);
	}
	begin_transaction();
	for (const int face_id : face_ids) {
		const bool reset_transform = data->face_uv_modes[face_id] != LevelMeshData::UV_MODE_PROJECTED;
		if (!_set_face_frame(face_id, reset_transform)) {
			rollback();
			return Ref<LevelMeshDiff>();
		}
	}
	transaction_changed = true;
	geometry_change_notification = true;
	data->emit_changed();
	geometry_change_notification = false;
	return commit();
}

Dictionary LevelMesh::solve_edge_hinge_similarity(const Vector2 &p0, const Vector2 &p1,
		const Vector2 &q0, const Vector2 &q1) {
	Dictionary result;
	result["valid"] = false;
	result["transform"] = Transform2D();
	if (!p0.is_finite() || !p1.is_finite() || !q0.is_finite() || !q1.is_finite()) {
		result["reason"] = "non_finite_correspondence";
		return result;
	}
	const Vector2 source_delta = p1 - p0;
	const Vector2 target_delta = q1 - q0;
	const real_t denominator = source_delta.length_squared();
	if (denominator <= CMP_EPSILON2) {
		result["reason"] = "degenerate_source_edge";
		return result;
	}
	const real_t real_part = target_delta.dot(source_delta) / denominator;
	const real_t imaginary_part = target_delta.cross(source_delta) / -denominator;
	const Vector2 transformed_p0(
			real_part * p0.x - imaginary_part * p0.y,
			imaginary_part * p0.x + real_part * p0.y);
	const Vector2 translation = q0 - transformed_p0;
	const Transform2D transform(real_part, imaginary_part, -imaginary_part, real_part,
			translation.x, translation.y);
	if (!transform.is_finite()) {
		result["reason"] = "non_finite_solution";
		return result;
	}
	result["valid"] = true;
	result["transform"] = transform;
	result["reason"] = "";
	return result;
}

bool LevelMesh::_restore_diff_state(const Ref<LevelMeshDiff> &p_diff, bool p_reverted) {
	const Ref<LevelMeshData> target_data = p_diff.is_valid() ? (p_reverted ? p_diff->before_data : p_diff->after_data) : Ref<LevelMeshData>();
	if (transaction_active || p_diff.is_null() || p_diff->empty || target_data.is_null()) {
		return false;
	}
	data->_copy_from(**target_data, true);
	if (p_diff->topology_changed) {
		_invalidate_topology();
	} else if (p_diff->geometry_changed) {
		_invalidate_geometry();
	}
	data->_emit_mesh_diff_applied(p_diff, p_reverted);
	return true;
}

bool LevelMesh::apply_diff(const Ref<LevelMeshDiff> &p_diff) {
	return _restore_diff_state(p_diff, false);
}

bool LevelMesh::revert_diff(const Ref<LevelMeshDiff> &p_diff) {
	return _restore_diff_state(p_diff, true);
}

Ref<LevelMeshAdjacency> LevelMesh::get_adjacency() const {
	return adjacency;
}

Ref<LevelMeshElementBVH> LevelMesh::get_element_bvh() const {
	return element_bvh;
}

Dictionary LevelMesh::ray_closest(const Vector3 &p_local_origin, const Vector3 &p_local_direction) const {
	return element_bvh->ray_closest(p_local_origin, p_local_direction);
}

PackedInt32Array LevelMesh::get_face_corner_vertex_ids(int p_face_id) const {
	PackedInt32Array result;
	if (p_face_id < 0 || p_face_id >= data->face_alive.size() || data->face_alive[p_face_id] == 0 ||
			p_face_id >= data->face_loop_starts.size() || p_face_id >= data->face_loop_counts.size()) {
		return result;
	}
	const int loop_start = data->face_loop_starts[p_face_id];
	const int loop_count = data->face_loop_counts[p_face_id];
	if (loop_count < 3 || loop_start < 0 || loop_start > data->loop_vertex_indices.size() - loop_count ||
			loop_start > data->loop_alive.size() - loop_count) {
		return result;
	}
	for (int corner = 0; corner < loop_count; corner++) {
		const int loop_id = loop_start + corner;
		const int vertex_id = data->loop_vertex_indices[loop_id];
		if (data->loop_alive[loop_id] == 0 || vertex_id < 0 || vertex_id >= data->vertex_alive.size() ||
				vertex_id >= data->vertex_positions.size() || data->vertex_alive[vertex_id] == 0) {
			result.clear();
			return result;
		}
		result.push_back(vertex_id);
	}
	return result;
}

PackedVector3Array LevelMesh::get_face_corner_positions(int p_face_id) const {
	PackedVector3Array result;
	const PackedInt32Array vertex_ids = get_face_corner_vertex_ids(p_face_id);
	result.resize(vertex_ids.size());
	for (int i = 0; i < vertex_ids.size(); i++) {
		result.set(i, data->vertex_positions[vertex_ids[i]]);
	}
	return result;
}

int LevelMesh::get_face_triangle_count(int p_face_id) const {
	const PackedInt32Array vertex_ids = get_face_corner_vertex_ids(p_face_id);
	return vertex_ids.size() >= 3 ? vertex_ids.size() - 2 : 0;
}

PackedInt32Array LevelMesh::get_face_triangle_vertex_ids(int p_face_id, int p_local_tri) const {
	PackedInt32Array result;
	const PackedInt32Array vertex_ids = get_face_corner_vertex_ids(p_face_id);
	if (p_local_tri < 0 || p_local_tri >= vertex_ids.size() - 2) {
		return result;
	}
	result.push_back(vertex_ids[0]);
	result.push_back(vertex_ids[p_local_tri + 1]);
	result.push_back(vertex_ids[p_local_tri + 2]);
	return result;
}

Vector3 LevelMesh::get_face_normal(int p_face_id) const {
	Vector3 normal;
	real_t distance = 0.0;
	return adjacency->_get_face_plane(p_face_id, normal, distance) ? normal : Vector3();
}

PackedInt32Array LevelMesh::get_face_boundary_edge_ids(int p_face_id, bool p_polygroup_tier) const {
	if (p_polygroup_tier) {
		return adjacency->get_polygroup_boundary_edges(p_face_id);
	}
	const PackedInt32Array edges = adjacency->get_face_edges(p_face_id);
	for (int i = 0; i < edges.size(); i++) {
		if (edges[i] < 0) {
			return PackedInt32Array();
		}
	}
	return edges;
}

PackedVector3Array LevelMesh::get_face_boundary_edge_positions(int p_face_id, bool p_polygroup_tier) const {
	PackedVector3Array result;
	const PackedInt32Array edge_ids = get_face_boundary_edge_ids(p_face_id, p_polygroup_tier);
	result.resize(edge_ids.size() * 2);
	for (int i = 0; i < edge_ids.size(); i++) {
		const PackedInt32Array vertices = adjacency->get_edge_vertices(edge_ids[i]);
		if (vertices.size() != 2 || vertices[0] < 0 || vertices[1] < 0 ||
				vertices[0] >= data->vertex_positions.size() || vertices[1] >= data->vertex_positions.size()) {
			result.clear();
			return result;
		}
		result.set(i * 2, data->vertex_positions[vertices[0]]);
		result.set(i * 2 + 1, data->vertex_positions[vertices[1]]);
	}
	return result;
}

int64_t LevelMesh::make_vertex_handle(int p_vertex_id) const {
	if (p_vertex_id < 0 || p_vertex_id >= data->vertex_alive.size() || p_vertex_id >= data->vertex_generations.size() || data->vertex_alive[p_vertex_id] == 0) {
		return -1;
	}
	return LevelMeshData::_pack_handle(p_vertex_id, (uint32_t)data->vertex_generations[p_vertex_id]);
}

int64_t LevelMesh::make_edge_handle(int p_edge_id) const {
	if (p_edge_id < 0 || p_edge_id >= data->edge_alive.size() || p_edge_id >= data->edge_generations.size() || data->edge_alive[p_edge_id] == 0) {
		return -1;
	}
	return LevelMeshData::_pack_handle(p_edge_id, (uint32_t)data->edge_generations[p_edge_id]);
}

int64_t LevelMesh::make_face_handle(int p_face_id) const {
	if (p_face_id < 0 || p_face_id >= data->face_alive.size() || p_face_id >= data->face_generations.size() || data->face_alive[p_face_id] == 0) {
		return -1;
	}
	return LevelMeshData::_pack_handle(p_face_id, (uint32_t)data->face_generations[p_face_id]);
}

int LevelMesh::resolve_vertex(int64_t p_handle) const {
	return LevelMeshData::_resolve_handle(p_handle, data->vertex_alive, data->vertex_generations);
}

int LevelMesh::resolve_edge(int64_t p_handle) const {
	return LevelMeshData::_resolve_handle(p_handle, data->edge_alive, data->edge_generations);
}

int LevelMesh::resolve_face(int64_t p_handle) const {
	return LevelMeshData::_resolve_handle(p_handle, data->face_alive, data->face_generations);
}

LevelMesh::LevelMesh() {
	data.instantiate();
	adjacency.instantiate();
	element_bvh.instantiate();
	data->connect_changed(callable_mp(this, &LevelMesh::_on_data_changed));
	adjacency->_set_data(data);
	element_bvh->_set_data(data);
}

LevelMesh::~LevelMesh() {
	if (data.is_valid()) {
		data->disconnect_changed(callable_mp(this, &LevelMesh::_on_data_changed));
	}
}

void LevelMesh::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_data", "data"), &LevelMesh::set_data);
	ClassDB::bind_method(D_METHOD("get_data"), &LevelMesh::get_data);
	ClassDB::bind_method(D_METHOD("begin_transaction"), &LevelMesh::begin_transaction);
	ClassDB::bind_method(D_METHOD("commit"), &LevelMesh::commit);
	ClassDB::bind_method(D_METHOD("rollback"), &LevelMesh::rollback);
	ClassDB::bind_method(D_METHOD("is_transaction_active"), &LevelMesh::is_transaction_active);
	ClassDB::bind_method(D_METHOD("create_box", "frame", "size", "material_index"), &LevelMesh::create_box);
	ClassDB::bind_method(D_METHOD("reconcile_face_uv", "face_id"), &LevelMesh::reconcile_face_uv);
	ClassDB::bind_method(D_METHOD("set_face_texture_lock", "face_id", "enabled"), &LevelMesh::set_face_texture_lock);
	ClassDB::bind_method(D_METHOD("is_face_texture_locked", "face_id"), &LevelMesh::is_face_texture_locked);
	ClassDB::bind_method(D_METHOD("project_native", "face_id", "point"), &LevelMesh::project_native);
	ClassDB::bind_method(D_METHOD("get_uv", "face_id", "point", "loop_id"), &LevelMesh::get_uv, DEFVAL(-1));
	ClassDB::bind_method(D_METHOD("get_face_uv_mode", "face_id"), &LevelMesh::get_face_uv_mode);
	ClassDB::bind_method(D_METHOD("get_face_uv_origin", "face_id"), &LevelMesh::get_face_uv_origin);
	ClassDB::bind_method(D_METHOD("get_face_uv_tangent", "face_id"), &LevelMesh::get_face_uv_tangent);
	ClassDB::bind_method(D_METHOD("get_face_uv_transform", "face_id"), &LevelMesh::get_face_uv_transform);
	ClassDB::bind_method(D_METHOD("capture_face_texture", "face_id"), &LevelMesh::capture_face_texture);
	ClassDB::bind_method(D_METHOD("apply_face_texture", "face_ids", "material_path", "captured_mapping"), &LevelMesh::apply_face_texture, DEFVAL(Dictionary()));
	ClassDB::bind_method(D_METHOD("calculate_wrap_transform", "source_face_id", "destination_face_id"), &LevelMesh::calculate_wrap_transform);
	ClassDB::bind_method(D_METHOD("wrap_faces", "source_face_id", "destination_face_ids"), &LevelMesh::wrap_faces);
	ClassDB::bind_method(D_METHOD("flow_faces", "ordered_face_ids"), &LevelMesh::flow_faces);
	ClassDB::bind_method(D_METHOD("modify_face_uv", "face_ids", "operation", "value"), &LevelMesh::modify_face_uv, DEFVAL(Vector2(1, 1)));
	ClassDB::bind_method(D_METHOD("apply_hotspot_fit", "face_results"), &LevelMesh::apply_hotspot_fit);
	ClassDB::bind_method(D_METHOD("get_last_hotspot_fit_diagnostics"), &LevelMesh::get_last_hotspot_fit_diagnostics);
	ClassDB::bind_method(D_METHOD("align_faces_to_grid", "face_ids"), &LevelMesh::align_faces_to_grid);
	ClassDB::bind_method(D_METHOD("align_faces_to_face", "face_ids"), &LevelMesh::align_faces_to_face);
	ClassDB::bind_static_method("LevelMesh", D_METHOD("grid_uv_tangent_for_normal", "normal"), &LevelMesh::grid_uv_tangent_for_normal);
	ClassDB::bind_static_method("LevelMesh", D_METHOD("solve_edge_hinge_similarity", "p0", "p1", "q0", "q1"), &LevelMesh::solve_edge_hinge_similarity);
	ClassDB::bind_static_method("LevelMesh", D_METHOD("unfold_face_across_edge", "data", "face_a", "edge_id", "face_b"), &LevelMesh::unfold_face_across_edge);
	ClassDB::bind_method(D_METHOD("unwrap_square", "face_ids"), &LevelMesh::unwrap_square);
	ClassDB::bind_method(D_METHOD("unwrap_planar", "face_ids"), &LevelMesh::unwrap_planar);
	ClassDB::bind_method(D_METHOD("unwrap_conforming", "face_ids", "distortion_threshold"), &LevelMesh::unwrap_conforming,
			DEFVAL(DEFAULT_CONFORMING_DISTORTION_THRESHOLD));
	ClassDB::bind_method(D_METHOD("unwrap_follow_quads", "face_ids", "spacing_mode"), &LevelMesh::unwrap_follow_quads);
	ClassDB::bind_method(D_METHOD("get_last_unwrap_error"), &LevelMesh::get_last_unwrap_error);
	ClassDB::bind_method(D_METHOD("get_last_unwrap_seam_edge_ids"), &LevelMesh::get_last_unwrap_seam_edge_ids);
	ClassDB::bind_method(D_METHOD("begin_transform_preview", "vertex_ids"), &LevelMesh::begin_transform_preview);
	ClassDB::bind_method(D_METHOD("preview_transform_vertices", "new_positions"), &LevelMesh::preview_transform_vertices);
	ClassDB::bind_method(D_METHOD("commit_transform_preview"), &LevelMesh::commit_transform_preview);
	ClassDB::bind_method(D_METHOD("cancel_transform_preview"), &LevelMesh::cancel_transform_preview);
	ClassDB::bind_method(D_METHOD("is_transform_preview_active"), &LevelMesh::is_transform_preview_active);
	ClassDB::bind_method(D_METHOD("extrude_faces", "face_ids"), &LevelMesh::extrude_faces);
	ClassDB::bind_method(D_METHOD("calculate_push_pull", "face_ids", "distance"), &LevelMesh::calculate_push_pull);
	ClassDB::bind_method(D_METHOD("push_pull_faces", "face_ids", "distance"), &LevelMesh::push_pull_faces);
	ClassDB::bind_method(D_METHOD("extrude_boundary_edges", "edge_ids"), &LevelMesh::extrude_boundary_edges);
	ClassDB::bind_method(D_METHOD("apply_diff", "diff"), &LevelMesh::apply_diff);
	ClassDB::bind_method(D_METHOD("revert_diff", "diff"), &LevelMesh::revert_diff);
	ClassDB::bind_method(D_METHOD("get_adjacency"), &LevelMesh::get_adjacency);
	ClassDB::bind_method(D_METHOD("get_element_bvh"), &LevelMesh::get_element_bvh);
	ClassDB::bind_method(D_METHOD("ray_closest", "local_origin", "local_direction"), &LevelMesh::ray_closest);
	ClassDB::bind_method(D_METHOD("get_face_corner_vertex_ids", "face_id"), &LevelMesh::get_face_corner_vertex_ids);
	ClassDB::bind_method(D_METHOD("get_face_corner_positions", "face_id"), &LevelMesh::get_face_corner_positions);
	ClassDB::bind_method(D_METHOD("get_face_triangle_count", "face_id"), &LevelMesh::get_face_triangle_count);
	ClassDB::bind_method(D_METHOD("get_face_triangle_vertex_ids", "face_id", "local_tri"), &LevelMesh::get_face_triangle_vertex_ids);
	ClassDB::bind_method(D_METHOD("get_face_normal", "face_id"), &LevelMesh::get_face_normal);
	ClassDB::bind_method(D_METHOD("get_face_boundary_edge_ids", "face_id", "polygroup_tier"), &LevelMesh::get_face_boundary_edge_ids, DEFVAL(false));
	ClassDB::bind_method(D_METHOD("get_face_boundary_edge_positions", "face_id", "polygroup_tier"), &LevelMesh::get_face_boundary_edge_positions, DEFVAL(false));
	ClassDB::bind_method(D_METHOD("make_vertex_handle", "vertex_id"), &LevelMesh::make_vertex_handle);
	ClassDB::bind_method(D_METHOD("make_edge_handle", "edge_id"), &LevelMesh::make_edge_handle);
	ClassDB::bind_method(D_METHOD("make_face_handle", "face_id"), &LevelMesh::make_face_handle);
	ClassDB::bind_method(D_METHOD("resolve_vertex", "handle"), &LevelMesh::resolve_vertex);
	ClassDB::bind_method(D_METHOD("resolve_edge", "handle"), &LevelMesh::resolve_edge);
	ClassDB::bind_method(D_METHOD("resolve_face", "handle"), &LevelMesh::resolve_face);

	BIND_ENUM_CONSTANT(TEXTURE_MODIFY_SHIFT);
	BIND_ENUM_CONSTANT(TEXTURE_MODIFY_SCALE);
	BIND_ENUM_CONSTANT(TEXTURE_MODIFY_ROTATE);
	BIND_ENUM_CONSTANT(TEXTURE_MODIFY_FIT);
	BIND_ENUM_CONSTANT(TEXTURE_MODIFY_JUSTIFY_LEFT);
	BIND_ENUM_CONSTANT(TEXTURE_MODIFY_JUSTIFY_RIGHT);
	BIND_ENUM_CONSTANT(TEXTURE_MODIFY_JUSTIFY_TOP);
	BIND_ENUM_CONSTANT(TEXTURE_MODIFY_JUSTIFY_BOTTOM);
	BIND_ENUM_CONSTANT(TEXTURE_MODIFY_JUSTIFY_CENTER);
	BIND_ENUM_CONSTANT(TEXTURE_MODIFY_FLIP_HORIZONTAL);
	BIND_ENUM_CONSTANT(TEXTURE_MODIFY_FLIP_VERTICAL);
	BIND_ENUM_CONSTANT(UNWRAP_SPACING_LENGTH);
	BIND_ENUM_CONSTANT(UNWRAP_SPACING_EVEN);
	BIND_ENUM_CONSTANT(UNWRAP_SPACING_LENGTH_AVERAGE);
	BIND_ENUM_CONSTANT(UNWRAP_ERROR_NONE);
	BIND_ENUM_CONSTANT(UNWRAP_ERROR_BUSY);
	BIND_ENUM_CONSTANT(UNWRAP_ERROR_EMPTY_SELECTION);
	BIND_ENUM_CONSTANT(UNWRAP_ERROR_INVALID_FACE);
	BIND_ENUM_CONSTANT(UNWRAP_ERROR_INVALID_TOPOLOGY);
	BIND_ENUM_CONSTANT(UNWRAP_ERROR_NON_MANIFOLD_EDGE);
	BIND_ENUM_CONSTANT(UNWRAP_ERROR_INVALID_SEED);
	BIND_ENUM_CONSTANT(UNWRAP_ERROR_INVALID_SPACING_MODE);
	BIND_ENUM_CONSTANT(UNWRAP_ERROR_INVALID_THRESHOLD);
	BIND_ENUM_CONSTANT(UNWRAP_ERROR_UNFOLD_FAILED);

	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "data", PROPERTY_HINT_RESOURCE_TYPE, "LevelMeshData"), "set_data", "get_data");
	// LevelMesh always starts with a private empty data resource; it is not a
	// shared instantiated property default.
	ADD_PROPERTY_DEFAULT("data", Ref<LevelMeshData>());
}
