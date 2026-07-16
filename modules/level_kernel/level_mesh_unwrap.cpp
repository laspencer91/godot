/**************************************************************************/
/*  level_mesh_unwrap.cpp                                                 */
/**************************************************************************/

#include "level_mesh.h"

#include "level_mesh_adjacency.h"
#include "level_mesh_data.h"
#include "level_mesh_diff.h"

#include "core/math/basis.h"
#include "core/math/math_funcs.h"
#include "core/templates/hash_set.h"

namespace {

struct FollowGridFace {
	int face_id = -1;
	Vector<Vector2i> loop_coordinates;
};

struct FollowGridPoint {
	Vector2i coordinate;
};

struct FollowGridSegment {
	int axis = 0;
	int line = 0;
	int lower_index = 0;
	int edge_id = -1;
	real_t length = 0.0;
};

struct FollowGridParameter {
	int line = 0;
	int index = 0;
	real_t value = 0.0;
};

struct FollowGridSegmentSort {
	_FORCE_INLINE_ bool operator()(const FollowGridSegment &p_left, const FollowGridSegment &p_right) const {
		if (p_left.axis != p_right.axis) {
			return p_left.axis < p_right.axis;
		}
		if (p_left.line != p_right.line) {
			return p_left.line < p_right.line;
		}
		if (p_left.lower_index != p_right.lower_index) {
			return p_left.lower_index < p_right.lower_index;
		}
		return p_left.edge_id < p_right.edge_id;
	}
};

int find_follow_face(const Vector<FollowGridFace> &p_faces, int p_face_id) {
	for (int i = 0; i < p_faces.size(); i++) {
		if (p_faces[i].face_id == p_face_id) {
			return i;
		}
	}
	return -1;
}

void append_unique_sorted_value(Vector<int> &r_values, int p_value) {
	if (r_values.find(p_value) < 0) {
		r_values.push_back(p_value);
	}
}

void append_unwrap_seam(PackedInt32Array &r_seams, int p_edge_id) {
	if (p_edge_id >= 0 && !r_seams.has(p_edge_id)) {
		r_seams.push_back(p_edge_id);
	}
}

bool get_segment_length(const Vector<FollowGridSegment> &p_segments, int p_axis,
		int p_line, int p_lower_index, real_t &r_length) {
	r_length = 0.0;
	int count = 0;
	for (const FollowGridSegment &segment : p_segments) {
		if (segment.axis == p_axis && segment.line == p_line && segment.lower_index == p_lower_index) {
			r_length += segment.length;
			count++;
		}
	}
	if (count == 0) {
		return false;
	}
	r_length /= (real_t)count;
	return Math::is_finite(r_length) && r_length > CMP_EPSILON;
}

bool build_grid_parameters(const Vector<FollowGridPoint> &p_points,
		const Vector<FollowGridSegment> &p_segments, int p_axis, int p_spacing_mode,
		Vector<FollowGridParameter> &r_parameters) {
	Vector<int> lines;
	for (const FollowGridPoint &point : p_points) {
		append_unique_sorted_value(lines, p_axis == 0 ? point.coordinate.y : point.coordinate.x);
	}
	lines.sort();

	for (const int line : lines) {
		Vector<int> indices;
		for (const FollowGridPoint &point : p_points) {
			const int point_line = p_axis == 0 ? point.coordinate.y : point.coordinate.x;
			if (point_line == line) {
				append_unique_sorted_value(indices, p_axis == 0 ? point.coordinate.x : point.coordinate.y);
			}
		}
		indices.sort();
		for (int component_start = 0; component_start < indices.size();) {
			int component_end = component_start;
			Vector<real_t> step_lengths;
			while (component_end + 1 < indices.size() && indices[component_end + 1] == indices[component_end] + 1) {
				real_t step_length = 0.0;
				if (!get_segment_length(p_segments, p_axis, line, indices[component_end], step_length)) {
					break;
				}
				step_lengths.push_back(step_length);
				component_end++;
			}

			real_t total_length = 0.0;
			for (const real_t step_length : step_lengths) {
				total_length += step_length;
			}
			const int step_count = component_end - component_start;
			real_t cumulative_length = 0.0;
			for (int cursor = component_start; cursor <= component_end; cursor++) {
				FollowGridParameter parameter;
				parameter.line = line;
				parameter.index = indices[cursor];
				switch (LevelMesh::UnwrapSpacingMode(p_spacing_mode)) {
					case LevelMesh::UNWRAP_SPACING_LENGTH: {
						parameter.value = cumulative_length;
					} break;
					case LevelMesh::UNWRAP_SPACING_EVEN: {
						parameter.value = step_count > 0 ? (real_t)(cursor - component_start) / (real_t)step_count : (real_t)0.0;
					} break;
					case LevelMesh::UNWRAP_SPACING_LENGTH_AVERAGE: {
						parameter.value = step_count > 0 ?
								(real_t)(cursor - component_start) * total_length / (real_t)step_count : (real_t)0.0;
					} break;
				}
				if (!Math::is_finite(parameter.value)) {
					return false;
				}
				r_parameters.push_back(parameter);
				if (cursor < component_end) {
					cumulative_length += step_lengths[cursor - component_start];
				}
			}
			component_start = component_end + 1;
		}
	}
	return true;
}

bool find_grid_parameter(const Vector<FollowGridParameter> &p_parameters,
		int p_line, int p_index, real_t &r_value) {
	for (const FollowGridParameter &parameter : p_parameters) {
		if (parameter.line == p_line && parameter.index == p_index) {
			r_value = parameter.value;
			return true;
		}
	}
	return false;
}

} // namespace

Vector3 LevelMesh::_world_aligned_uv_tangent(const Vector3 &p_normal) {
	if (!p_normal.is_finite() || p_normal.length_squared() <= CMP_EPSILON2) {
		return Vector3();
	}
	const Vector3 normal = p_normal.normalized();
	Vector3 tangent = Vector3(0, 1, 0).cross(normal);
	if (tangent.length_squared() <= CMP_EPSILON2) {
		tangent = Vector3(1, 0, 0) - normal * normal.x;
	}
	return tangent.length_squared() > CMP_EPSILON2 ? tangent.normalized() : Vector3();
}

bool LevelMesh::_validate_unwrap_selection(const PackedInt32Array &p_face_ids, Vector<int> &r_face_ids) {
	last_unwrap_error = UNWRAP_ERROR_NONE;
	last_unwrap_seam_edge_ids.clear();
	r_face_ids.clear();
	if (transaction_active || transform_preview_active) {
		last_unwrap_error = UNWRAP_ERROR_BUSY;
		return false;
	}
	if (p_face_ids.is_empty()) {
		last_unwrap_error = UNWRAP_ERROR_EMPTY_SELECTION;
		return false;
	}
	if (data.is_null() || adjacency.is_null() || !adjacency->is_valid()) {
		last_unwrap_error = UNWRAP_ERROR_INVALID_TOPOLOGY;
		return false;
	}

	for (const int face_id : p_face_ids) {
		if (r_face_ids.find(face_id) >= 0) {
			continue;
		}
		Vector3 centroid;
		Vector3 normal;
		real_t area_x2 = 0.0;
		real_t longest_edge_squared = 0.0;
		if (!_compute_face_geometry(**data, face_id, centroid, normal, area_x2, longest_edge_squared) ||
				face_id < 0 || face_id >= data->face_uv_modes.size() ||
				face_id >= data->face_uv_origins.size() || face_id >= data->face_uv_tangents.size() ||
				(face_id + 1) * 6 > data->face_uv_transforms.size() ||
				face_id >= data->face_loop_starts.size() || face_id >= data->face_loop_counts.size()) {
			last_unwrap_error = UNWRAP_ERROR_INVALID_FACE;
			return false;
		}
		const int loop_start = data->face_loop_starts[face_id];
		const int loop_count = data->face_loop_counts[face_id];
		if (loop_start < 0 || loop_count < 3 || loop_start > data->loop_uv0.size() - loop_count ||
				loop_start > data->loop_normals.size() - loop_count ||
				adjacency->get_face_edges(face_id).size() != loop_count) {
			last_unwrap_error = UNWRAP_ERROR_INVALID_FACE;
			return false;
		}
		r_face_ids.push_back(face_id);
	}
	r_face_ids.sort();

	HashSet<int> selected_faces;
	for (const int face_id : r_face_ids) {
		selected_faces.insert(face_id);
	}
	for (const int face_id : r_face_ids) {
		for (const int edge_id : adjacency->get_face_edges(face_id)) {
			if (edge_id < 0) {
				last_unwrap_error = UNWRAP_ERROR_INVALID_TOPOLOGY;
				return false;
			}
			const PackedInt32Array radial_faces = adjacency->get_edge_faces(edge_id);
			int selected_count = 0;
			for (const int radial_face : radial_faces) {
				if (selected_faces.has(radial_face)) {
					selected_count++;
				}
			}
			if (selected_count >= 2 && radial_faces.size() > 2) {
				last_unwrap_error = UNWRAP_ERROR_NON_MANIFOLD_EDGE;
				return false;
			}
		}
	}
	return true;
}

LevelMesh::UnwrapError LevelMesh::get_last_unwrap_error() const {
	return last_unwrap_error;
}

PackedInt32Array LevelMesh::get_last_unwrap_seam_edge_ids() const {
	return last_unwrap_seam_edge_ids;
}

Ref<LevelMeshDiff> LevelMesh::unwrap_square(const PackedInt32Array &p_face_ids) {
	Vector<int> face_ids;
	if (!_validate_unwrap_selection(p_face_ids, face_ids)) {
		return Ref<LevelMeshDiff>();
	}

	begin_transaction();
	for (const int face_id : face_ids) {
		Vector3 centroid;
		Vector3 normal;
		real_t area_x2 = 0.0;
		real_t longest_edge_squared = 0.0;
		if (!_compute_face_geometry(**data, face_id, centroid, normal, area_x2, longest_edge_squared)) {
			transaction_changed = true;
			rollback();
			last_unwrap_error = UNWRAP_ERROR_INVALID_FACE;
			return Ref<LevelMeshDiff>();
		}
		const Vector3 tangent = _dominant_axis_tangent(normal);
		if (tangent.length_squared() <= CMP_EPSILON2) {
			transaction_changed = true;
			rollback();
			last_unwrap_error = UNWRAP_ERROR_INVALID_FACE;
			return Ref<LevelMeshDiff>();
		}
		data->face_uv_modes.set(face_id, LevelMeshData::UV_MODE_PROJECTED);
		data->face_uv_origins.set(face_id, centroid);
		data->face_uv_tangents.set(face_id, tangent);
		// V1 keeps one UV unit per world unit. The per-face centroid frame makes
		// Square independent per face while preserving a consistent density scale.
		LevelMeshData::_write_uv_transform(**data, face_id, Transform2D());
		if (!_reconcile_face_uv(face_id)) {
			transaction_changed = true;
			rollback();
			last_unwrap_error = UNWRAP_ERROR_INVALID_FACE;
			return Ref<LevelMeshDiff>();
		}
	}
	transaction_changed = true;
	geometry_change_notification = true;
	data->emit_changed();
	geometry_change_notification = false;
	return commit();
}

Ref<LevelMeshDiff> LevelMesh::unwrap_planar(const PackedInt32Array &p_face_ids) {
	Vector<int> face_ids;
	if (!_validate_unwrap_selection(p_face_ids, face_ids)) {
		return Ref<LevelMeshDiff>();
	}

	int dominant_face_id = -1;
	real_t dominant_area_x2 = -1.0;
	Vector3 dominant_normal;
	for (const int face_id : face_ids) {
		Vector3 centroid;
		Vector3 normal;
		real_t area_x2 = 0.0;
		real_t longest_edge_squared = 0.0;
		if (!_compute_face_geometry(**data, face_id, centroid, normal, area_x2, longest_edge_squared)) {
			last_unwrap_error = UNWRAP_ERROR_INVALID_FACE;
			return Ref<LevelMeshDiff>();
		}
		// face_ids is sorted, and equality deliberately does not replace the
		// winner, so an exact area tie resolves to the lowest stable face id.
		if (area_x2 > dominant_area_x2) {
			dominant_area_x2 = area_x2;
			dominant_face_id = face_id;
			dominant_normal = normal;
		}
	}
	const Vector3 tangent = _world_aligned_uv_tangent(dominant_normal);
	if (dominant_face_id < 0 || tangent.length_squared() <= CMP_EPSILON2) {
		last_unwrap_error = UNWRAP_ERROR_INVALID_FACE;
		return Ref<LevelMeshDiff>();
	}

	begin_transaction();
	for (const int face_id : face_ids) {
		data->face_uv_modes.set(face_id, LevelMeshData::UV_MODE_PROJECTED);
		// The world origin and area-dominant tangent are written verbatim to
		// every face. This shared recipe is the continuity mechanism.
		data->face_uv_origins.set(face_id, Vector3());
		data->face_uv_tangents.set(face_id, tangent);
		LevelMeshData::_write_uv_transform(**data, face_id, Transform2D());
		if (!_reconcile_face_uv(face_id)) {
			transaction_changed = true;
			rollback();
			last_unwrap_error = UNWRAP_ERROR_INVALID_FACE;
			return Ref<LevelMeshDiff>();
		}
	}
	transaction_changed = true;
	geometry_change_notification = true;
	data->emit_changed();
	geometry_change_notification = false;
	return commit();
}

Dictionary LevelMesh::unfold_face_across_edge(const Ref<LevelMeshData> &p_data,
		int p_face_a, int p_edge_id, int p_face_b) {
	Dictionary result;
	result["valid"] = false;
	result["reason"] = String("invalid_input");
	result["loop_ids"] = PackedInt32Array();
	result["uvs"] = PackedVector2Array();
	result["dihedral_angle"] = (real_t)0.0;
	if (p_data.is_null() || p_face_a == p_face_b || p_edge_id < 0 ||
			p_edge_id >= p_data->edge_alive.size() || p_data->edge_alive[p_edge_id] == 0 ||
			p_edge_id > (p_data->edge_vertices.size() - 2) / 2) {
		return result;
	}

	auto get_face_layout = [&](int p_face_id, int &r_loop_start, int &r_loop_count) -> bool {
		if (p_face_id < 0 || p_face_id >= p_data->face_alive.size() || p_data->face_alive[p_face_id] == 0 ||
				p_face_id >= p_data->face_loop_starts.size() || p_face_id >= p_data->face_loop_counts.size()) {
			return false;
		}
		r_loop_start = p_data->face_loop_starts[p_face_id];
		r_loop_count = p_data->face_loop_counts[p_face_id];
		return r_loop_count >= 3 && r_loop_start >= 0 &&
				r_loop_start <= p_data->loop_vertex_indices.size() - r_loop_count &&
				r_loop_start <= p_data->loop_uv0.size() - r_loop_count &&
				r_loop_start <= p_data->loop_alive.size() - r_loop_count;
	};

	int loop_start_a = -1;
	int loop_count_a = 0;
	int loop_start_b = -1;
	int loop_count_b = 0;
	if (!get_face_layout(p_face_a, loop_start_a, loop_count_a) ||
			!get_face_layout(p_face_b, loop_start_b, loop_count_b)) {
		result["reason"] = "invalid_face";
		return result;
	}
	const int edge_vertex_a = p_data->edge_vertices[p_edge_id * 2];
	const int edge_vertex_b = p_data->edge_vertices[p_edge_id * 2 + 1];
	if (edge_vertex_a < 0 || edge_vertex_b < 0 || edge_vertex_a == edge_vertex_b ||
			edge_vertex_a >= p_data->vertex_positions.size() || edge_vertex_b >= p_data->vertex_positions.size()) {
		result["reason"] = "invalid_edge";
		return result;
	}

	auto find_edge_corner = [&](int p_loop_start, int p_loop_count) -> int {
		for (int corner = 0; corner < p_loop_count; corner++) {
			const int vertex_a = p_data->loop_vertex_indices[p_loop_start + corner];
			const int vertex_b = p_data->loop_vertex_indices[p_loop_start + ((corner + 1) % p_loop_count)];
			if ((vertex_a == edge_vertex_a && vertex_b == edge_vertex_b) ||
					(vertex_a == edge_vertex_b && vertex_b == edge_vertex_a)) {
				return corner;
			}
		}
		return -1;
	};
	if (find_edge_corner(loop_start_a, loop_count_a) < 0 || find_edge_corner(loop_start_b, loop_count_b) < 0) {
		result["reason"] = "edge_not_shared_by_faces";
		return result;
	}

	auto find_vertex_loop = [&](int p_loop_start, int p_loop_count, int p_vertex_id) -> int {
		for (int corner = 0; corner < p_loop_count; corner++) {
			const int loop_id = p_loop_start + corner;
			if (p_data->loop_alive[loop_id] != 0 && p_data->loop_vertex_indices[loop_id] == p_vertex_id) {
				return loop_id;
			}
		}
		return -1;
	};
	const int parent_loop_a = find_vertex_loop(loop_start_a, loop_count_a, edge_vertex_a);
	const int parent_loop_b = find_vertex_loop(loop_start_a, loop_count_a, edge_vertex_b);
	if (parent_loop_a < 0 || parent_loop_b < 0) {
		result["reason"] = "missing_parent_edge_uv";
		return result;
	}

	Vector3 centroid_a;
	Vector3 normal_a;
	Vector3 centroid_b;
	Vector3 normal_b;
	real_t area_a_x2 = 0.0;
	real_t area_b_x2 = 0.0;
	real_t longest_edge_squared = 0.0;
	if (!_compute_face_geometry(**p_data, p_face_a, centroid_a, normal_a, area_a_x2, longest_edge_squared) ||
			!_compute_face_geometry(**p_data, p_face_b, centroid_b, normal_b, area_b_x2, longest_edge_squared)) {
		result["reason"] = "degenerate_face";
		return result;
	}

	const Vector3 world_a = p_data->vertex_positions[edge_vertex_a];
	const Vector3 world_b = p_data->vertex_positions[edge_vertex_b];
	const Vector3 edge_delta = world_b - world_a;
	const real_t edge_length = edge_delta.length();
	const Vector2 uv_a = p_data->loop_uv0[parent_loop_a];
	const Vector2 uv_b = p_data->loop_uv0[parent_loop_b];
	if (edge_length <= CMP_EPSILON || !uv_a.is_finite() || !uv_b.is_finite() ||
			uv_a.distance_squared_to(uv_b) <= CMP_EPSILON2) {
		result["reason"] = "degenerate_parent_edge";
		return result;
	}
	const Vector3 edge_axis = edge_delta / edge_length;
	const Vector3 parent_perpendicular = normal_a.cross(edge_axis).normalized();
	const Vector2 uv_edge_axis = (uv_b - uv_a) / edge_length;
	if (parent_perpendicular.length_squared() <= CMP_EPSILON2 || !uv_edge_axis.is_finite()) {
		result["reason"] = "invalid_parent_basis";
		return result;
	}

	Vector2 uv_perpendicular;
	real_t best_perpendicular_distance = 0.0;
	for (int corner = 0; corner < loop_count_a; corner++) {
		const int loop_id = loop_start_a + corner;
		const int vertex_id = p_data->loop_vertex_indices[loop_id];
		if (vertex_id == edge_vertex_a || vertex_id == edge_vertex_b || vertex_id < 0 ||
				vertex_id >= p_data->vertex_positions.size() || p_data->loop_alive[loop_id] == 0) {
			continue;
		}
		const Vector3 relative = p_data->vertex_positions[vertex_id] - world_a;
		const real_t perpendicular_distance = relative.dot(parent_perpendicular);
		if (Math::abs(perpendicular_distance) <= best_perpendicular_distance + CMP_EPSILON) {
			continue;
		}
		const Vector2 residual = p_data->loop_uv0[loop_id] - uv_a - uv_edge_axis * relative.dot(edge_axis);
		const Vector2 candidate = residual / perpendicular_distance;
		if (!candidate.is_finite() || candidate.length_squared() <= CMP_EPSILON2) {
			continue;
		}
		uv_perpendicular = candidate;
		best_perpendicular_distance = Math::abs(perpendicular_distance);
	}
	if (best_perpendicular_distance <= CMP_EPSILON || !uv_perpendicular.is_finite()) {
		result["reason"] = "missing_parent_basis";
		return result;
	}

	const real_t signed_angle = Math::atan2(edge_axis.dot(normal_b.cross(normal_a)), normal_b.dot(normal_a));
	const Basis unfold_rotation(edge_axis, signed_angle);
	PackedInt32Array loop_ids;
	PackedVector2Array uvs;
	for (int corner = 0; corner < loop_count_b; corner++) {
		const int loop_id = loop_start_b + corner;
		const int vertex_id = p_data->loop_vertex_indices[loop_id];
		if (p_data->loop_alive[loop_id] == 0 || vertex_id < 0 || vertex_id >= p_data->vertex_positions.size()) {
			result["reason"] = "invalid_child_loop";
			return result;
		}
		Vector2 uv;
		if (vertex_id == edge_vertex_a) {
			uv = uv_a;
		} else if (vertex_id == edge_vertex_b) {
			uv = uv_b;
		} else {
			const Vector3 unfolded = world_a + unfold_rotation.xform(p_data->vertex_positions[vertex_id] - world_a);
			const Vector3 relative = unfolded - world_a;
			uv = uv_a + uv_edge_axis * relative.dot(edge_axis) +
					uv_perpendicular * relative.dot(parent_perpendicular);
		}
		if (!uv.is_finite()) {
			result["reason"] = "non_finite_child_uv";
			return result;
		}
		loop_ids.push_back(loop_id);
		uvs.push_back(uv);
	}

	result["valid"] = true;
	result["reason"] = String();
	result["loop_ids"] = loop_ids;
	result["uvs"] = uvs;
	result["dihedral_angle"] = Math::abs(signed_angle);
	return result;
}

Ref<LevelMeshDiff> LevelMesh::unwrap_conforming(const PackedInt32Array &p_face_ids,
		real_t p_distortion_threshold) {
	Vector<int> face_ids;
	if (!_validate_unwrap_selection(p_face_ids, face_ids)) {
		return Ref<LevelMeshDiff>();
	}
	if (!Math::is_finite(p_distortion_threshold) || p_distortion_threshold < 0.0) {
		last_unwrap_error = UNWRAP_ERROR_INVALID_THRESHOLD;
		return Ref<LevelMeshDiff>();
	}

	HashSet<int> selected_faces;
	for (const int face_id : face_ids) {
		selected_faces.insert(face_id);
	}
	Ref<LevelMeshData> working = data->duplicate_data();
	Vector<uint8_t> visited;
	Vector<real_t> distortion;
	Vector<int> parent_edge;
	visited.resize(data->face_alive.size());
	distortion.resize(data->face_alive.size());
	parent_edge.resize(data->face_alive.size());
	for (int face_id = 0; face_id < visited.size(); face_id++) {
		visited.write[face_id] = 0;
		distortion.write[face_id] = 0.0;
		parent_edge.write[face_id] = -1;
	}

	auto seed_face = [&](int p_face_id) -> bool {
		Vector3 centroid;
		Vector3 normal;
		real_t area_x2 = 0.0;
		real_t longest_edge_squared = 0.0;
		if (!_compute_face_geometry(**working, p_face_id, centroid, normal, area_x2, longest_edge_squared)) {
			return false;
		}
		const Vector3 tangent = _world_aligned_uv_tangent(normal);
		const Vector3 bitangent = normal.cross(tangent).normalized();
		if (tangent.length_squared() <= CMP_EPSILON2 || bitangent.length_squared() <= CMP_EPSILON2) {
			return false;
		}
		const int loop_start = working->face_loop_starts[p_face_id];
		const int loop_count = working->face_loop_counts[p_face_id];
		for (int corner = 0; corner < loop_count; corner++) {
			const int loop_id = loop_start + corner;
			const int vertex_id = working->loop_vertex_indices[loop_id];
			const Vector3 relative = working->vertex_positions[vertex_id] - centroid;
			working->loop_uv0.set(loop_id, Vector2(relative.dot(tangent), relative.dot(bitangent)));
		}
		return true;
	};

	Vector<int> queue;
	int visited_count = 0;
	for (const int selection_root : face_ids) {
		if (visited[selection_root] != 0) {
			continue;
		}
		if (!seed_face(selection_root)) {
			last_unwrap_error = UNWRAP_ERROR_UNFOLD_FAILED;
			last_unwrap_seam_edge_ids.clear();
			return Ref<LevelMeshDiff>();
		}
		visited.write[selection_root] = 1;
		distortion.write[selection_root] = 0.0;
		parent_edge.write[selection_root] = -1;
		visited_count++;
		queue.clear();
		queue.push_back(selection_root);
		for (int queue_cursor = 0; queue_cursor < queue.size(); queue_cursor++) {
			const int face_a = queue[queue_cursor];
			for (const int edge_id : adjacency->get_face_edges(face_a)) {
				const PackedInt32Array radial_faces = adjacency->get_edge_faces(edge_id);
				int face_b = -1;
				for (const int radial_face : radial_faces) {
					if (radial_face != face_a && selected_faces.has(radial_face)) {
						face_b = radial_face;
						break;
					}
				}
				if (face_b < 0 || parent_edge[face_a] == edge_id || parent_edge[face_b] == edge_id) {
					continue;
				}

				if (visited[face_b] == 0) {
					Vector3 centroid_a;
					Vector3 normal_a;
					Vector3 centroid_b;
					Vector3 normal_b;
					real_t area_x2 = 0.0;
					real_t longest_edge_squared = 0.0;
					if (!_compute_face_geometry(**working, face_a, centroid_a, normal_a, area_x2, longest_edge_squared) ||
							!_compute_face_geometry(**working, face_b, centroid_b, normal_b, area_x2, longest_edge_squared)) {
						last_unwrap_error = UNWRAP_ERROR_UNFOLD_FAILED;
						last_unwrap_seam_edge_ids.clear();
						return Ref<LevelMeshDiff>();
					}
					const real_t dihedral = Math::acos(CLAMP(normal_a.dot(normal_b), (real_t)-1.0, (real_t)1.0));
					const real_t candidate_distortion = distortion[face_a] + dihedral;
					if (candidate_distortion > p_distortion_threshold + CYCLE_CLOSURE_EPSILON) {
						append_unwrap_seam(last_unwrap_seam_edge_ids, edge_id);
						if (!seed_face(face_b)) {
							last_unwrap_error = UNWRAP_ERROR_UNFOLD_FAILED;
							last_unwrap_seam_edge_ids.clear();
							return Ref<LevelMeshDiff>();
						}
						distortion.write[face_b] = 0.0;
						parent_edge.write[face_b] = -1;
					} else {
						const Dictionary candidate = unfold_face_across_edge(working, face_a, edge_id, face_b);
						if (!bool(candidate.get("valid", false))) {
							last_unwrap_error = UNWRAP_ERROR_UNFOLD_FAILED;
							last_unwrap_seam_edge_ids.clear();
							return Ref<LevelMeshDiff>();
						}
						const PackedInt32Array loop_ids = candidate.get("loop_ids", PackedInt32Array());
						const PackedVector2Array uvs = candidate.get("uvs", PackedVector2Array());
						if (loop_ids.size() != working->face_loop_counts[face_b] || loop_ids.size() != uvs.size()) {
							last_unwrap_error = UNWRAP_ERROR_UNFOLD_FAILED;
							last_unwrap_seam_edge_ids.clear();
							return Ref<LevelMeshDiff>();
						}
						for (int i = 0; i < loop_ids.size(); i++) {
							working->loop_uv0.set(loop_ids[i], uvs[i]);
						}
						distortion.write[face_b] = candidate_distortion;
						parent_edge.write[face_b] = edge_id;
					}
					visited.write[face_b] = 1;
					visited_count++;
					queue.push_back(face_b);
					continue;
				}

				const Dictionary candidate = unfold_face_across_edge(working, face_a, edge_id, face_b);
				if (!bool(candidate.get("valid", false))) {
					last_unwrap_error = UNWRAP_ERROR_UNFOLD_FAILED;
					last_unwrap_seam_edge_ids.clear();
					return Ref<LevelMeshDiff>();
				}
				const PackedInt32Array loop_ids = candidate.get("loop_ids", PackedInt32Array());
				const PackedVector2Array uvs = candidate.get("uvs", PackedVector2Array());
				bool conflict = loop_ids.size() != uvs.size();
				for (int i = 0; !conflict && i < loop_ids.size(); i++) {
					if (working->loop_uv0[loop_ids[i]].distance_squared_to(uvs[i]) >
							CYCLE_CLOSURE_EPSILON * CYCLE_CLOSURE_EPSILON) {
						conflict = true;
					}
				}
				if (conflict) {
					// First-visited wins: only record the second-arriving edge as a
					// seam. The established face loop values are never overwritten.
					append_unwrap_seam(last_unwrap_seam_edge_ids, edge_id);
				}
			}
		}
	}
	if (visited_count != face_ids.size()) {
		last_unwrap_error = UNWRAP_ERROR_UNFOLD_FAILED;
		last_unwrap_seam_edge_ids.clear();
		return Ref<LevelMeshDiff>();
	}

	begin_transaction();
	for (const int face_id : face_ids) {
		data->face_uv_modes.set(face_id, LevelMeshData::UV_MODE_EXPLICIT);
		data->face_uv_origins.set(face_id, Vector3());
		data->face_uv_tangents.set(face_id, Vector3());
		LevelMeshData::_write_uv_transform(**data, face_id, Transform2D());
		const int loop_start = data->face_loop_starts[face_id];
		const int loop_count = data->face_loop_counts[face_id];
		for (int corner = 0; corner < loop_count; corner++) {
			data->loop_uv0.set(loop_start + corner, working->loop_uv0[loop_start + corner]);
		}
		if (!_reconcile_face_uv(face_id)) {
			transaction_changed = true;
			rollback();
			last_unwrap_error = UNWRAP_ERROR_UNFOLD_FAILED;
			last_unwrap_seam_edge_ids.clear();
			return Ref<LevelMeshDiff>();
		}
	}
	transaction_changed = true;
	geometry_change_notification = true;
	data->emit_changed();
	geometry_change_notification = false;
	return commit();
}

Ref<LevelMeshDiff> LevelMesh::unwrap_follow_quads(const PackedInt32Array &p_face_ids, int p_spacing_mode) {
	Vector<int> face_ids;
	if (!_validate_unwrap_selection(p_face_ids, face_ids)) {
		return Ref<LevelMeshDiff>();
	}
	if (p_spacing_mode < UNWRAP_SPACING_LENGTH || p_spacing_mode > UNWRAP_SPACING_LENGTH_AVERAGE) {
		last_unwrap_error = UNWRAP_ERROR_INVALID_SPACING_MODE;
		return Ref<LevelMeshDiff>();
	}
	const int seed_face = face_ids[0];
	if (data->face_loop_counts[seed_face] != 4 || adjacency->get_face_edges(seed_face).size() != 4) {
		last_unwrap_error = UNWRAP_ERROR_INVALID_SEED;
		return Ref<LevelMeshDiff>();
	}

	HashSet<int> selected_faces;
	for (const int face_id : face_ids) {
		selected_faces.insert(face_id);
	}
	Vector<FollowGridFace> grid_faces;
	FollowGridFace seed;
	seed.face_id = seed_face;
	seed.loop_coordinates.resize(4);
	seed.loop_coordinates.write[0] = Vector2i(0, 0);
	seed.loop_coordinates.write[1] = Vector2i(1, 0);
	seed.loop_coordinates.write[2] = Vector2i(1, 1);
	seed.loop_coordinates.write[3] = Vector2i(0, 1);
	grid_faces.push_back(seed);
	Vector<int> queue;
	queue.push_back(seed_face);

	const Vector2i unset_coordinate(0x3fffffff, 0x3fffffff);
	for (int queue_cursor = 0; queue_cursor < queue.size(); queue_cursor++) {
		const int current_index = find_follow_face(grid_faces, queue[queue_cursor]);
		if (current_index < 0) {
			last_unwrap_error = UNWRAP_ERROR_UNFOLD_FAILED;
			return Ref<LevelMeshDiff>();
		}
		const FollowGridFace current = grid_faces[current_index];
		const PackedInt32Array current_edges = adjacency->get_face_edges(current.face_id);
		const int current_loop_start = data->face_loop_starts[current.face_id];
		for (int edge_corner = 0; edge_corner < 4; edge_corner++) {
			const int edge_id = current_edges[edge_corner];
			const PackedInt32Array radial_faces = adjacency->get_edge_faces(edge_id);
			int neighbor_face = -1;
			for (const int radial_face : radial_faces) {
				if (radial_face != current.face_id && selected_faces.has(radial_face)) {
					neighbor_face = radial_face;
					break;
				}
			}
			if (neighbor_face < 0 || data->face_loop_counts[neighbor_face] != 4) {
				// Selection boundaries and non-quads are terminators, never fallback
				// targets. Their complete UV rows remain untouched.
				continue;
			}
			const PackedInt32Array neighbor_edges = adjacency->get_face_edges(neighbor_face);
			const int entry_corner = neighbor_edges.find(edge_id);
			if (neighbor_edges.size() != 4 || entry_corner < 0 ||
					neighbor_edges[(entry_corner + 2) % 4] < 0 ||
					neighbor_edges[(entry_corner + 2) % 4] == edge_id) {
				// loop.next.next is the only legal continuation. A missing or
				// collapsed opposite edge terminates this ribbon direction.
				continue;
			}

			Vector<Vector2i> candidate_coordinates;
			candidate_coordinates.resize(4);
			for (int corner = 0; corner < 4; corner++) {
				candidate_coordinates.write[corner] = unset_coordinate;
			}
			const int neighbor_loop_start = data->face_loop_starts[neighbor_face];
			bool candidate_valid = true;
			for (int endpoint = 0; endpoint < 2; endpoint++) {
				const int current_corner = (edge_corner + endpoint) % 4;
				const int vertex_id = data->loop_vertex_indices[current_loop_start + current_corner];
				int neighbor_corner = -1;
				for (int entry_endpoint = 0; entry_endpoint < 2; entry_endpoint++) {
					const int candidate_corner = (entry_corner + entry_endpoint) % 4;
					if (data->loop_vertex_indices[neighbor_loop_start + candidate_corner] == vertex_id) {
						neighbor_corner = candidate_corner;
						break;
					}
				}
				if (neighbor_corner < 0) {
					candidate_valid = false;
					break;
				}
				const int current_inside_corner = endpoint == 0 ? (edge_corner + 3) % 4 : (edge_corner + 2) % 4;
				const int neighbor_outside_corner = neighbor_corner == entry_corner ?
						(entry_corner + 3) % 4 : (entry_corner + 2) % 4;
				const Vector2i inside_delta = current.loop_coordinates[current_inside_corner] -
						current.loop_coordinates[current_corner];
				candidate_coordinates.write[neighbor_corner] = current.loop_coordinates[current_corner];
				candidate_coordinates.write[neighbor_outside_corner] =
						current.loop_coordinates[current_corner] - inside_delta;
			}
			for (int corner = 0; candidate_valid && corner < 4; corner++) {
				const Vector2i delta = candidate_coordinates[(corner + 1) % 4] - candidate_coordinates[corner];
				candidate_valid = candidate_coordinates[corner] != unset_coordinate &&
						Math::abs(delta.x) + Math::abs(delta.y) == 1;
			}
			if (!candidate_valid) {
				continue;
			}

			const int existing_index = find_follow_face(grid_faces, neighbor_face);
			if (existing_index >= 0) {
				bool agrees = true;
				for (int corner = 0; corner < 4; corner++) {
					agrees = agrees && grid_faces[existing_index].loop_coordinates[corner] == candidate_coordinates[corner];
				}
				if (!agrees) {
					append_unwrap_seam(last_unwrap_seam_edge_ids, edge_id);
				}
				continue;
			}

			FollowGridFace neighbor;
			neighbor.face_id = neighbor_face;
			neighbor.loop_coordinates = candidate_coordinates;
			grid_faces.push_back(neighbor);
			queue.push_back(neighbor_face);
		}
	}

	Vector<FollowGridPoint> grid_points;
	Vector<FollowGridSegment> grid_segments;
	HashSet<int> recorded_edges;
	for (const FollowGridFace &grid_face : grid_faces) {
		const int loop_start = data->face_loop_starts[grid_face.face_id];
		const PackedInt32Array face_edges = adjacency->get_face_edges(grid_face.face_id);
		for (int corner = 0; corner < 4; corner++) {
			FollowGridPoint point;
			point.coordinate = grid_face.loop_coordinates[corner];
			grid_points.push_back(point);
			const int edge_id = face_edges[corner];
			if (recorded_edges.has(edge_id)) {
				continue;
			}
			recorded_edges.insert(edge_id);
			const Vector2i coordinate_a = grid_face.loop_coordinates[corner];
			const Vector2i coordinate_b = grid_face.loop_coordinates[(corner + 1) % 4];
			const Vector2i coordinate_delta = coordinate_b - coordinate_a;
			if (Math::abs(coordinate_delta.x) + Math::abs(coordinate_delta.y) != 1) {
				continue;
			}
			const PackedInt32Array edge_vertices = adjacency->get_edge_vertices(edge_id);
			if (edge_vertices.size() != 2) {
				continue;
			}
			FollowGridSegment segment;
			segment.edge_id = edge_id;
			segment.length = data->vertex_positions[edge_vertices[0]].distance_to(data->vertex_positions[edge_vertices[1]]);
			if (coordinate_delta.y == 0) {
				segment.axis = 0;
				segment.line = coordinate_a.y;
				segment.lower_index = MIN(coordinate_a.x, coordinate_b.x);
			} else {
				segment.axis = 1;
				segment.line = coordinate_a.x;
				segment.lower_index = MIN(coordinate_a.y, coordinate_b.y);
			}
			if (Math::is_finite(segment.length) && segment.length > CMP_EPSILON) {
				grid_segments.push_back(segment);
			}
		}
	}
	grid_segments.sort_custom<FollowGridSegmentSort>();
	Vector<FollowGridParameter> u_parameters;
	Vector<FollowGridParameter> v_parameters;
	if (!build_grid_parameters(grid_points, grid_segments, 0, p_spacing_mode, u_parameters) ||
			!build_grid_parameters(grid_points, grid_segments, 1, p_spacing_mode, v_parameters)) {
		last_unwrap_error = UNWRAP_ERROR_UNFOLD_FAILED;
		last_unwrap_seam_edge_ids.clear();
		return Ref<LevelMeshDiff>();
	}

	Ref<LevelMeshData> working = data->duplicate_data();
	Vector<int> applied_faces;
	for (const FollowGridFace &grid_face : grid_faces) {
		applied_faces.push_back(grid_face.face_id);
		const int loop_start = data->face_loop_starts[grid_face.face_id];
		for (int corner = 0; corner < 4; corner++) {
			const Vector2i coordinate = grid_face.loop_coordinates[corner];
			real_t u = 0.0;
			real_t v = 0.0;
			if (!find_grid_parameter(u_parameters, coordinate.y, coordinate.x, u) ||
					!find_grid_parameter(v_parameters, coordinate.x, coordinate.y, v)) {
				last_unwrap_error = UNWRAP_ERROR_UNFOLD_FAILED;
				last_unwrap_seam_edge_ids.clear();
				return Ref<LevelMeshDiff>();
			}
			working->loop_uv0.set(loop_start + corner, Vector2(u, v));
		}
	}
	applied_faces.sort();

	begin_transaction();
	for (const int face_id : applied_faces) {
		data->face_uv_modes.set(face_id, LevelMeshData::UV_MODE_EXPLICIT);
		data->face_uv_origins.set(face_id, Vector3());
		data->face_uv_tangents.set(face_id, Vector3());
		LevelMeshData::_write_uv_transform(**data, face_id, Transform2D());
		const int loop_start = data->face_loop_starts[face_id];
		for (int corner = 0; corner < 4; corner++) {
			data->loop_uv0.set(loop_start + corner, working->loop_uv0[loop_start + corner]);
		}
		if (!_reconcile_face_uv(face_id)) {
			transaction_changed = true;
			rollback();
			last_unwrap_error = UNWRAP_ERROR_UNFOLD_FAILED;
			last_unwrap_seam_edge_ids.clear();
			return Ref<LevelMeshDiff>();
		}
	}
	transaction_changed = true;
	geometry_change_notification = true;
	data->emit_changed();
	geometry_change_notification = false;
	return commit();
}
