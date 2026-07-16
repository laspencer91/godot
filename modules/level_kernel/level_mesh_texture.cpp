/**************************************************************************/
/*  level_mesh_texture.cpp                                               */
/**************************************************************************/

#include "level_mesh.h"

#include "level_mesh_adjacency.h"
#include "level_mesh_data.h"
#include "level_mesh_diff.h"

#include "core/math/math_funcs.h"
#include "core/templates/hash_set.h"

namespace {

bool face_row_is_alive(const Ref<LevelMeshData> &p_data, int p_face_id) {
	return p_data.is_valid() && p_face_id >= 0 && p_face_id < p_data->get_face_alive().size() &&
			p_data->get_face_alive()[p_face_id] != 0;
}

Transform2D uv_scale_about(const Vector2 &p_scale, const Vector2 &p_pivot) {
	Transform2D result(p_scale.x, 0.0, 0.0, p_scale.y, 0.0, 0.0);
	result[2] = p_pivot - result.basis_xform(p_pivot);
	return result;
}

} // namespace

Dictionary LevelMesh::capture_face_texture(int p_face_id) const {
	Dictionary capture;
	capture["valid"] = false;
	capture["has_mapping"] = false;
	capture["material_index"] = -1;
	capture["material_path"] = String();
	capture["uv_mode"] = LevelMeshData::UV_MODE_PROJECTED;
	capture["uv_origin"] = Vector3();
	capture["uv_tangent"] = Vector3();
	capture["uv_transform"] = Transform2D();

	if (!face_row_is_alive(data, p_face_id) || p_face_id >= data->face_material_indices.size() ||
			p_face_id >= data->face_uv_modes.size()) {
		return capture;
	}
	const int material_index = data->face_material_indices[p_face_id];
	const String material_path = data->get_material_path(material_index);
	if (material_path.is_empty()) {
		return capture;
	}

	const int uv_mode = data->face_uv_modes[p_face_id];
	capture["valid"] = true;
	capture["material_index"] = material_index;
	capture["material_path"] = material_path;
	capture["uv_mode"] = uv_mode;
	if (uv_mode != LevelMeshData::UV_MODE_PROJECTED || p_face_id >= data->face_uv_origins.size() ||
			p_face_id >= data->face_uv_tangents.size()) {
		// EXPLICIT sources deliberately degrade to a material-only capture. The
		// identity values make that degradation unambiguous to script callers.
		return capture;
	}

	const Vector3 origin = data->face_uv_origins[p_face_id];
	const Vector3 tangent = data->face_uv_tangents[p_face_id];
	const Transform2D transform = LevelMeshData::_read_uv_transform(**data, p_face_id);
	if (!origin.is_finite() || !tangent.is_finite() || tangent.length_squared() <= CMP_EPSILON2 ||
			!transform.is_finite()) {
		return capture;
	}
	capture["has_mapping"] = true;
	capture["uv_origin"] = origin;
	capture["uv_tangent"] = tangent;
	capture["uv_transform"] = transform;
	return capture;
}

Ref<LevelMeshDiff> LevelMesh::apply_face_texture(const PackedInt32Array &p_face_ids,
		const String &p_material_path, const Dictionary &p_captured_mapping) {
	if (transaction_active || transform_preview_active || p_material_path.is_empty() || p_face_ids.is_empty()) {
		return Ref<LevelMeshDiff>();
	}

	Vector<int> face_ids;
	for (const int face_id : p_face_ids) {
		if (!face_row_is_alive(data, face_id) || face_id >= data->face_material_indices.size() ||
				face_id >= data->face_uv_modes.size() || face_id >= data->face_uv_tangents.size()) {
			return Ref<LevelMeshDiff>();
		}
		if (face_ids.find(face_id) < 0) {
			face_ids.push_back(face_id);
		}
	}
	face_ids.sort();

	const String captured_material_path = p_captured_mapping.get("material_path", String());
	const bool stamp_mapping = bool(p_captured_mapping.get("valid", false)) &&
			bool(p_captured_mapping.get("has_mapping", false)) &&
			int(p_captured_mapping.get("uv_mode", LevelMeshData::UV_MODE_PROJECTED)) == LevelMeshData::UV_MODE_PROJECTED &&
			captured_material_path == p_material_path;
	Vector3 captured_origin;
	Vector3 captured_tangent;
	Transform2D captured_transform;
	if (stamp_mapping) {
		captured_origin = p_captured_mapping.get("uv_origin", Vector3());
		captured_tangent = p_captured_mapping.get("uv_tangent", Vector3());
		captured_transform = p_captured_mapping.get("uv_transform", Transform2D());
		if (!captured_origin.is_finite() || !captured_tangent.is_finite() ||
				captured_tangent.length_squared() <= CMP_EPSILON2 || !captured_transform.is_finite()) {
			return Ref<LevelMeshDiff>();
		}
	}

	int material_index = data->material_paths.find(p_material_path);
	const int target_material_index = material_index >= 0 ? material_index : data->material_paths.size();
	bool changes = material_index < 0;
	for (const int face_id : face_ids) {
		changes = changes || data->face_material_indices[face_id] != target_material_index;
		if (stamp_mapping) {
			changes = changes || data->face_uv_modes[face_id] != LevelMeshData::UV_MODE_PROJECTED ||
					data->face_uv_origins[face_id] != captured_origin ||
					data->face_uv_tangents[face_id] != captured_tangent ||
					LevelMeshData::_read_uv_transform(**data, face_id) != captured_transform;
		} else {
			const Vector3 tangent = data->face_uv_tangents[face_id];
			changes = changes || !tangent.is_finite() || tangent.length_squared() <= CMP_EPSILON2;
		}
	}
	if (!changes) {
		return Ref<LevelMeshDiff>();
	}

	begin_transaction();
	if (material_index < 0) {
		data->material_paths.push_back(p_material_path);
		material_index = data->material_paths.size() - 1;
	}
	for (const int face_id : face_ids) {
		data->face_material_indices.set(face_id, material_index);
		if (stamp_mapping) {
			data->face_uv_modes.set(face_id, LevelMeshData::UV_MODE_PROJECTED);
			data->face_uv_origins.set(face_id, captured_origin);
			data->face_uv_tangents.set(face_id, captured_tangent);
			LevelMeshData::_write_uv_transform(**data, face_id, captured_transform);
			if (!_reconcile_face_uv(face_id)) {
				transaction_changed = true;
				rollback();
				return Ref<LevelMeshDiff>();
			}
		} else {
			const Vector3 tangent = data->face_uv_tangents[face_id];
			if (!tangent.is_finite() || tangent.length_squared() <= CMP_EPSILON2) {
				if (!_set_grid_frame(face_id, true)) {
					transaction_changed = true;
					rollback();
					return Ref<LevelMeshDiff>();
				}
			}
		}
	}
	transaction_changed = true;
	geometry_change_notification = true;
	data->emit_changed();
	geometry_change_notification = false;
	return commit();
}

bool LevelMesh::_get_shared_edge_vertices(int p_face_a, int p_face_b, int &r_edge_id,
		int &r_vertex_a, int &r_vertex_b) const {
	r_edge_id = -1;
	r_vertex_a = -1;
	r_vertex_b = -1;
	if (!face_row_is_alive(data, p_face_a) || !face_row_is_alive(data, p_face_b) || p_face_a == p_face_b) {
		return false;
	}
	for (const int edge_id : adjacency->get_face_edges(p_face_a)) {
		if (edge_id < 0 || !adjacency->get_edge_faces(edge_id).has(p_face_b)) {
			continue;
		}
		if (r_edge_id >= 0) {
			// Two faces sharing multiple physical edges are ambiguous for a
			// two-point hinge transfer and are rejected deterministically.
			return false;
		}
		r_edge_id = edge_id;
	}
	if (r_edge_id < 0) {
		return false;
	}
	const PackedInt32Array vertices = adjacency->get_edge_vertices(r_edge_id);
	if (vertices.size() != 2) {
		return false;
	}
	r_vertex_a = vertices[0];
	r_vertex_b = vertices[1];
	return r_vertex_a >= 0 && r_vertex_b >= 0 && r_vertex_a < data->vertex_positions.size() &&
			r_vertex_b < data->vertex_positions.size();
}

bool LevelMesh::_calculate_wrap_transform_internal(int p_source_face_id, int p_destination_face_id,
		Transform2D &r_transform, int &r_edge_id, String &r_reason) const {
	r_transform = Transform2D();
	r_edge_id = -1;
	r_reason = "invalid_faces";
	int vertex_a = -1;
	int vertex_b = -1;
	if (!_get_shared_edge_vertices(p_source_face_id, p_destination_face_id, r_edge_id, vertex_a, vertex_b)) {
		r_reason = "faces_do_not_share_one_edge";
		return false;
	}
	if (p_source_face_id >= data->face_uv_modes.size() || p_destination_face_id >= data->face_uv_modes.size()) {
		return false;
	}

	if (data->face_uv_modes[p_source_face_id] == LevelMeshData::UV_MODE_PROJECTED) {
		Vector3 source_tangent;
		Vector3 source_bitangent;
		Vector3 source_normal;
		if (!_get_projection_basis(**data, p_source_face_id, source_tangent, source_bitangent, source_normal)) {
			r_reason = "invalid_source_frame";
			return false;
		}
	} else if (data->face_uv_modes[p_source_face_id] != LevelMeshData::UV_MODE_EXPLICIT) {
		r_reason = "invalid_source_mode";
		return false;
	}

	Vector3 destination_tangent;
	Vector3 destination_bitangent;
	Vector3 destination_normal;
	Vector3 destination_origin;
	const bool destination_projected = data->face_uv_modes[p_destination_face_id] == LevelMeshData::UV_MODE_PROJECTED &&
			_get_projection_basis(**data, p_destination_face_id, destination_tangent, destination_bitangent, destination_normal);
	if (destination_projected) {
		destination_origin = data->face_uv_origins[p_destination_face_id];
	} else {
		Vector3 centroid;
		real_t area_x2 = 0.0;
		real_t longest_edge_squared = 0.0;
		if (!_compute_face_geometry(**data, p_destination_face_id, centroid, destination_normal, area_x2, longest_edge_squared)) {
			r_reason = "invalid_destination_geometry";
			return false;
		}
		destination_origin = Vector3();
		destination_tangent = grid_uv_tangent_for_normal(destination_normal);
		destination_bitangent = destination_normal.cross(destination_tangent);
		if (destination_tangent.length_squared() <= CMP_EPSILON2 ||
				destination_bitangent.length_squared() <= CMP_EPSILON2) {
			r_reason = "invalid_destination_frame";
			return false;
		}
		destination_tangent.normalize();
		destination_bitangent.normalize();
	}

	const Vector3 world_a = data->vertex_positions[vertex_a];
	const Vector3 world_b = data->vertex_positions[vertex_b];
	const Vector3 relative_a = world_a - destination_origin;
	const Vector3 relative_b = world_b - destination_origin;
	const Vector2 native_a(relative_a.dot(destination_tangent), relative_a.dot(destination_bitangent));
	const Vector2 native_b(relative_b.dot(destination_tangent), relative_b.dot(destination_bitangent));
	const Vector2 target_a = get_uv(p_source_face_id, world_a);
	const Vector2 target_b = get_uv(p_source_face_id, world_b);
	if (!target_a.is_finite() || !target_b.is_finite() ||
			target_a.distance_squared_to(target_b) <= CMP_EPSILON2) {
		r_reason = "degenerate_source_uv_edge";
		return false;
	}
	const Dictionary solution = solve_edge_hinge_similarity(native_a, native_b, target_a, target_b);
	if (!bool(solution.get("valid", false))) {
		r_reason = solution.get("reason", "hinge_solve_failed");
		return false;
	}
	r_transform = solution.get("transform", Transform2D());
	r_reason = String();
	return true;
}

Dictionary LevelMesh::calculate_wrap_transform(int p_source_face_id, int p_destination_face_id) const {
	Dictionary result;
	Transform2D transform;
	int edge_id = -1;
	String reason;
	const bool valid = _calculate_wrap_transform_internal(p_source_face_id, p_destination_face_id,
			transform, edge_id, reason);
	result["valid"] = valid;
	result["transform"] = transform;
	result["edge_id"] = edge_id;
	result["reason"] = reason;
	return result;
}

bool LevelMesh::_apply_wrap_pair(int p_source_face_id, int p_destination_face_id) {
	Vector3 tangent;
	Vector3 bitangent;
	Vector3 normal;
	if (p_destination_face_id >= data->face_uv_modes.size() ||
			data->face_uv_modes[p_destination_face_id] != LevelMeshData::UV_MODE_PROJECTED ||
			!_get_projection_basis(**data, p_destination_face_id, tangent, bitangent, normal)) {
		if (!_set_grid_frame(p_destination_face_id, true)) {
			return false;
		}
	}
	Transform2D transform;
	int edge_id = -1;
	String reason;
	if (!_calculate_wrap_transform_internal(p_source_face_id, p_destination_face_id,
			transform, edge_id, reason)) {
		return false;
	}
	data->face_uv_modes.set(p_destination_face_id, LevelMeshData::UV_MODE_PROJECTED);
	LevelMeshData::_write_uv_transform(**data, p_destination_face_id, transform);
	return _reconcile_face_uv(p_destination_face_id);
}

Ref<LevelMeshDiff> LevelMesh::wrap_faces(int p_source_face_id, const PackedInt32Array &p_destination_face_ids) {
	if (transaction_active || transform_preview_active || !face_row_is_alive(data, p_source_face_id) ||
			p_destination_face_ids.is_empty()) {
		return Ref<LevelMeshDiff>();
	}
	Vector<int> destinations;
	for (const int face_id : p_destination_face_ids) {
		if (face_id == p_source_face_id) {
			continue;
		}
		if (!face_row_is_alive(data, face_id)) {
			return Ref<LevelMeshDiff>();
		}
		if (destinations.find(face_id) < 0) {
			destinations.push_back(face_id);
		}
	}
	if (destinations.is_empty()) {
		return Ref<LevelMeshDiff>();
	}
	destinations.sort();

	HashSet<int> remaining;
	for (const int face_id : destinations) {
		remaining.insert(face_id);
	}
	Vector<int> queue;
	queue.push_back(p_source_face_id);
	int queue_index = 0;
	begin_transaction();
	while (queue_index < queue.size() && !remaining.is_empty()) {
		const int parent = queue[queue_index++];
		Vector<int> children;
		for (const int edge_id : adjacency->get_face_edges(parent)) {
			if (edge_id < 0) {
				continue;
			}
			for (const int neighbor : adjacency->get_edge_faces(edge_id)) {
				if (neighbor != parent && remaining.has(neighbor) && children.find(neighbor) < 0) {
					children.push_back(neighbor);
				}
			}
		}
		children.sort();
		for (const int child : children) {
			if (!_apply_wrap_pair(parent, child)) {
				transaction_changed = true;
				rollback();
				return Ref<LevelMeshDiff>();
			}
			remaining.erase(child);
			queue.push_back(child);
		}
	}
	if (!remaining.is_empty()) {
		transaction_changed = true;
		rollback();
		return Ref<LevelMeshDiff>();
	}
	transaction_changed = true;
	geometry_change_notification = true;
	data->emit_changed();
	geometry_change_notification = false;
	return commit();
}

Ref<LevelMeshDiff> LevelMesh::flow_faces(const PackedInt32Array &p_ordered_face_ids) {
	if (transaction_active || transform_preview_active || p_ordered_face_ids.size() < 2) {
		return Ref<LevelMeshDiff>();
	}
	Vector<int> chain;
	HashSet<int> seen;
	for (const int face_id : p_ordered_face_ids) {
		if (!face_row_is_alive(data, face_id)) {
			return Ref<LevelMeshDiff>();
		}
		if (!chain.is_empty() && chain[chain.size() - 1] == face_id) {
			continue;
		}
		if (seen.has(face_id)) {
			return Ref<LevelMeshDiff>();
		}
		seen.insert(face_id);
		chain.push_back(face_id);
	}
	if (chain.size() < 2) {
		return Ref<LevelMeshDiff>();
	}
	for (int i = 1; i < chain.size(); i++) {
		int edge_id = -1;
		int vertex_a = -1;
		int vertex_b = -1;
		if (!_get_shared_edge_vertices(chain[i - 1], chain[i], edge_id, vertex_a, vertex_b)) {
			return Ref<LevelMeshDiff>();
		}
	}

	begin_transaction();
	for (int i = 1; i < chain.size(); i++) {
		if (!_apply_wrap_pair(chain[i - 1], chain[i])) {
			transaction_changed = true;
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

bool LevelMesh::_get_face_uv_bounds(int p_face_id, Rect2 &r_bounds) const {
	if (!face_row_is_alive(data, p_face_id) || p_face_id >= data->face_loop_starts.size() ||
			p_face_id >= data->face_loop_counts.size()) {
		return false;
	}
	const int loop_start = data->face_loop_starts[p_face_id];
	const int loop_count = data->face_loop_counts[p_face_id];
	if (loop_count < 3 || loop_start < 0 || loop_start > data->loop_vertex_indices.size() - loop_count) {
		return false;
	}
	Vector2 minimum;
	Vector2 maximum;
	for (int corner = 0; corner < loop_count; corner++) {
		const int loop_id = loop_start + corner;
		const int vertex_id = data->loop_vertex_indices[loop_id];
		if (vertex_id < 0 || vertex_id >= data->vertex_positions.size()) {
			return false;
		}
		const Vector2 uv = get_uv(p_face_id, data->vertex_positions[vertex_id], loop_id);
		if (!uv.is_finite()) {
			return false;
		}
		if (corner == 0) {
			minimum = uv;
			maximum = uv;
		} else {
			minimum.x = MIN(minimum.x, uv.x);
			minimum.y = MIN(minimum.y, uv.y);
			maximum.x = MAX(maximum.x, uv.x);
			maximum.y = MAX(maximum.y, uv.y);
		}
	}
	r_bounds = Rect2(minimum, maximum - minimum);
	return r_bounds.position.is_finite() && r_bounds.size.is_finite();
}

Ref<LevelMeshDiff> LevelMesh::modify_face_uv(const PackedInt32Array &p_face_ids, int p_operation,
		const Vector2 &p_value) {
	if (transaction_active || transform_preview_active || p_face_ids.is_empty() || !p_value.is_finite() ||
			p_operation < TEXTURE_MODIFY_SHIFT || p_operation > TEXTURE_MODIFY_FLIP_VERTICAL) {
		return Ref<LevelMeshDiff>();
	}
	Vector<int> face_ids;
	for (const int face_id : p_face_ids) {
		if (!face_row_is_alive(data, face_id) || face_id >= data->face_uv_modes.size()) {
			return Ref<LevelMeshDiff>();
		}
		if (face_ids.find(face_id) < 0) {
			face_ids.push_back(face_id);
		}
	}
	face_ids.sort();

	begin_transaction();
	for (const int face_id : face_ids) {
		Vector3 tangent;
		Vector3 bitangent;
		Vector3 normal;
		if (data->face_uv_modes[face_id] != LevelMeshData::UV_MODE_PROJECTED ||
				!_get_projection_basis(**data, face_id, tangent, bitangent, normal)) {
			if (!_set_grid_frame(face_id, true)) {
				transaction_changed = true;
				rollback();
				return Ref<LevelMeshDiff>();
			}
		}

		Rect2 bounds;
		if (!_get_face_uv_bounds(face_id, bounds)) {
			transaction_changed = true;
			rollback();
			return Ref<LevelMeshDiff>();
		}
		const Vector2 center = bounds.get_center();
		Transform2D transform = LevelMeshData::_read_uv_transform(**data, face_id);
		Transform2D uv_operation;
		switch (TextureModifyOperation(p_operation)) {
			case TEXTURE_MODIFY_SHIFT: {
				transform[2] += p_value;
			} break;
			case TEXTURE_MODIFY_SCALE: {
				if (Math::abs(p_value.x) <= CMP_EPSILON || Math::abs(p_value.y) <= CMP_EPSILON) {
					transaction_changed = true;
					rollback();
					return Ref<LevelMeshDiff>();
				}
				uv_operation = uv_scale_about(p_value, center);
				transform = uv_operation * transform;
			} break;
			case TEXTURE_MODIFY_ROTATE: {
				uv_operation = Transform2D(p_value.x, Vector2());
				uv_operation[2] = center - uv_operation.basis_xform(center);
				transform = uv_operation * transform;
			} break;
			case TEXTURE_MODIFY_FIT: {
				if (bounds.size.x <= CMP_EPSILON || bounds.size.y <= CMP_EPSILON ||
						p_value.x <= CMP_EPSILON || p_value.y <= CMP_EPSILON) {
					transaction_changed = true;
					rollback();
					return Ref<LevelMeshDiff>();
				}
				uv_operation = uv_scale_about(Vector2(p_value.x / bounds.size.x, p_value.y / bounds.size.y), center);
				uv_operation[2] += p_value * (real_t)0.5 - center;
				transform = uv_operation * transform;
			} break;
			case TEXTURE_MODIFY_JUSTIFY_LEFT: {
				transform[2].x -= bounds.position.x;
			} break;
			case TEXTURE_MODIFY_JUSTIFY_RIGHT: {
				transform[2].x += p_value.x - bounds.get_end().x;
			} break;
			case TEXTURE_MODIFY_JUSTIFY_TOP: {
				transform[2].y -= bounds.position.y;
			} break;
			case TEXTURE_MODIFY_JUSTIFY_BOTTOM: {
				transform[2].y += p_value.y - bounds.get_end().y;
			} break;
			case TEXTURE_MODIFY_JUSTIFY_CENTER: {
				transform[2] += p_value * (real_t)0.5 - center;
			} break;
			case TEXTURE_MODIFY_FLIP_HORIZONTAL: {
				uv_operation = uv_scale_about(Vector2(-1, 1), center);
				transform = uv_operation * transform;
			} break;
			case TEXTURE_MODIFY_FLIP_VERTICAL: {
				uv_operation = uv_scale_about(Vector2(1, -1), center);
				transform = uv_operation * transform;
			} break;
		}
		if (!transform.is_finite()) {
			transaction_changed = true;
			rollback();
			return Ref<LevelMeshDiff>();
		}
		LevelMeshData::_write_uv_transform(**data, face_id, transform);
		if (!_reconcile_face_uv(face_id)) {
			transaction_changed = true;
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
