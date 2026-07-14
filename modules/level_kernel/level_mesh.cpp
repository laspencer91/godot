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
	Vector3 world_tangent;
	if (absolute_normal.x >= absolute_normal.y && absolute_normal.x >= absolute_normal.z) {
		world_tangent = Vector3(0, 0, 1);
	} else {
		world_tangent = Vector3(1, 0, 0);
	}

	// Project the chosen world axis onto the actual face plane. For axis-aligned
	// faces this remains an exact cardinal axis; sloped frames stay orthonormal.
	Vector3 tangent = world_tangent - p_normal * p_normal.dot(world_tangent);
	if (tangent.length_squared() <= CMP_EPSILON2) {
		world_tangent = Vector3(0, 1, 0);
		tangent = world_tangent - p_normal * p_normal.dot(world_tangent);
	}
	return tangent.normalized();
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

bool LevelMesh::_reconcile_face_uv(int p_face_id) {
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

	Vector3 frame_origin;
	Vector3 tangent;
	Vector3 bitangent;
	Vector3 normal;
	if (!_compute_face_basis(**data, p_face_id, false, frame_origin, tangent, bitangent, normal)) {
		return false;
	}
	data->face_uv_tangents.set(p_face_id, tangent);

	const bool projected = data->face_uv_modes[p_face_id] == LevelMeshData::UV_MODE_PROJECTED;
	const Vector3 origin = data->face_uv_origins[p_face_id];
	const Transform2D uv_transform = LevelMeshData::_read_uv_transform(**data, p_face_id);
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
			data->loop_uv0.set(loop_id, uv_transform.xform(projected_uv));
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

	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "data", PROPERTY_HINT_RESOURCE_TYPE, "LevelMeshData"), "set_data", "get_data");
	// LevelMesh always starts with a private empty data resource; it is not a
	// shared instantiated property default.
	ADD_PROPERTY_DEFAULT("data", Ref<LevelMeshData>());
}
