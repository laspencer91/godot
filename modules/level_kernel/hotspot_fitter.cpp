/**************************************************************************/
/*  hotspot_fitter.cpp                                                    */
/**************************************************************************/
/*  G-Level LE3: deterministic, editor-independent hotspot fitting.       */
/**************************************************************************/

#include "hotspot_fitter.h"

#include "hotspot_atlas.h"
#include "level_mesh.h"
#include "level_mesh_adjacency.h"
#include "level_mesh_data.h"

#include "core/math/math_funcs.h"
#include "core/object/class_db.h"
#include "core/templates/hash_set.h"

namespace {

struct FaceGeometry {
	Vector3 centroid;
	Vector3 normal;
	real_t area_x2 = 0.0;
};

struct HotspotIsland {
	Vector<int> faces;
	Vector<int> vertex_ids;
	bool has_fold = false;
};

struct IslandLayout {
	PackedVector2Array loop_coordinates;
	PackedByteArray loop_coordinate_set;
	Vector2 axis_u;
	Vector2 axis_v;
	real_t min_u = 0.0;
	real_t max_u = 0.0;
	real_t min_v = 0.0;
	real_t max_v = 0.0;
	real_t world_w = 0.0;
	real_t world_h = 0.0;
	real_t distortion = 0.0;
};

struct PatchCandidate {
	Ref<HotspotPatch> patch;
	int patch_index = -1;
	String name;
	real_t width_px = 0.0;
	real_t height_px = 0.0;
	real_t aspect = 0.0;
	real_t density_error = 0.0;
	real_t aspect_error = 0.0;
	bool needs_swap = false;
};

struct PatchCandidateNameSort {
	_FORCE_INLINE_ bool operator()(const PatchCandidate &p_left, const PatchCandidate &p_right) const {
		if (p_left.name != p_right.name) {
			return p_left.name < p_right.name;
		}
		return p_left.patch_index < p_right.patch_index;
	}
};

class StableRNG {
	uint64_t state = 0;

public:
	explicit StableRNG(uint64_t p_seed) {
		state = p_seed != 0 ? p_seed : UINT64_C(0x9e3779b97f4a7c15);
	}

	uint64_t next() {
		// SplitMix64 is fixed-width and has no platform or engine-global state.
		state += UINT64_C(0x9e3779b97f4a7c15);
		uint64_t value = state;
		value = (value ^ (value >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
		value = (value ^ (value >> 27)) * UINT64_C(0x94d049bb133111eb);
		return value ^ (value >> 31);
	}

	int range(int p_size) {
		return p_size > 0 ? int(next() % uint64_t(p_size)) : 0;
	}

	bool bit() {
		return (next() & 1) != 0;
	}
};

uint64_t fnv1a_append_u32(uint64_t p_hash, uint32_t p_value) {
	for (int byte = 0; byte < 4; byte++) {
		p_hash ^= uint8_t(p_value >> (byte * 8));
		p_hash *= UINT64_C(1099511628211);
	}
	return p_hash;
}

uint64_t stable_string_hash(const String &p_value) {
	uint64_t hash = UINT64_C(1469598103934665603);
	const CharString utf8 = p_value.utf8();
	for (int i = 0; i < utf8.length(); i++) {
		hash ^= uint8_t(utf8[i]);
		hash *= UINT64_C(1099511628211);
	}
	return hash;
}

uint64_t island_vertex_hash(const Vector<int> &p_vertex_ids) {
	uint64_t hash = UINT64_C(1469598103934665603);
	for (const int vertex_id : p_vertex_ids) {
		hash = fnv1a_append_u32(hash, uint32_t(vertex_id));
	}
	return hash;
}

real_t log2_positive(real_t p_value) {
	return Math::log(p_value) / Math::log((real_t)2.0);
}

Dictionary fit_error(const String &p_reason) {
	Dictionary result;
	result["ok"] = false;
	result["error"] = p_reason;
	result["faces"] = Array();
	result["diagnostics"] = Array();
	result["island_count"] = 0;
	return result;
}

bool compute_face_geometry(const PackedVector3Array &p_positions,
		const PackedInt32Array &p_loop_vertices, const PackedInt32Array &p_face_starts,
		const PackedInt32Array &p_face_counts, const PackedByteArray &p_face_alive,
		int p_face_id, FaceGeometry &r_geometry) {
	if (p_face_id < 0 || p_face_id >= p_face_alive.size() || p_face_alive[p_face_id] == 0 ||
			p_face_id >= p_face_starts.size() || p_face_id >= p_face_counts.size()) {
		return false;
	}
	const int loop_start = p_face_starts[p_face_id];
	const int loop_count = p_face_counts[p_face_id];
	if (loop_start < 0 || loop_count < 3 || loop_start > p_loop_vertices.size() - loop_count) {
		return false;
	}
	Vector3 centroid;
	for (int corner = 0; corner < loop_count; corner++) {
		const int vertex_id = p_loop_vertices[loop_start + corner];
		if (vertex_id < 0 || vertex_id >= p_positions.size() || !p_positions[vertex_id].is_finite()) {
			return false;
		}
		centroid += p_positions[vertex_id];
	}
	centroid /= (real_t)loop_count;
	const Vector3 anchor = p_positions[p_loop_vertices[loop_start]];
	Vector3 normal_sum;
	for (int corner = 1; corner + 1 < loop_count; corner++) {
		normal_sum += (p_positions[p_loop_vertices[loop_start + corner]] - anchor).cross(
				p_positions[p_loop_vertices[loop_start + corner + 1]] - anchor);
	}
	const real_t area_x2 = normal_sum.length();
	if (!centroid.is_finite() || !Math::is_finite(area_x2) || area_x2 <= CMP_EPSILON) {
		return false;
	}
	r_geometry.centroid = centroid;
	r_geometry.normal = normal_sum / area_x2;
	r_geometry.area_x2 = area_x2;
	return r_geometry.normal.is_finite();
}

Vector3 projected_world_up(const Vector3 &p_normal) {
	const Vector3 projected = Vector3(0, 1, 0) - p_normal * p_normal.y;
	return projected.length_squared() > CMP_EPSILON2 ? projected.normalized() : Vector3();
}

bool can_merge_faces(int p_face_a, int p_face_b, int p_edge_id,
		const Vector<FaceGeometry> &p_geometry, const PackedVector3Array &p_positions,
		const Ref<LevelMeshAdjacency> &p_adjacency, real_t p_cos_coplanar,
		real_t p_cos_collinear) {
	if (p_face_a < 0 || p_face_b < 0 || p_face_a >= p_geometry.size() || p_face_b >= p_geometry.size()) {
		return false;
	}
	const real_t normal_dot = p_geometry[p_face_a].normal.dot(p_geometry[p_face_b].normal);
	if (normal_dot >= p_cos_coplanar) {
		return true;
	}
	// A strip fold must be a genuine bend, and its hinge must preserve the
	// common extrusion/run direction on both faces. Projecting gravity into
	// each plane identifies that direction without depending on face winding;
	// it also makes a wall/floor junction fail while a cornering wall ribbon
	// remains one island.
	if (Math::abs(normal_dot) >= p_cos_collinear) {
		return false;
	}
	const PackedInt32Array edge_vertices = p_adjacency->get_edge_vertices(p_edge_id);
	if (edge_vertices.size() != 2 || edge_vertices[0] < 0 || edge_vertices[1] < 0 ||
			edge_vertices[0] >= p_positions.size() || edge_vertices[1] >= p_positions.size()) {
		return false;
	}
	const Vector3 edge_delta = p_positions[edge_vertices[1]] - p_positions[edge_vertices[0]];
	if (edge_delta.length_squared() <= CMP_EPSILON2) {
		return false;
	}
	const Vector3 edge_direction = edge_delta.normalized();
	const Vector3 run_a = projected_world_up(p_geometry[p_face_a].normal);
	const Vector3 run_b = projected_world_up(p_geometry[p_face_b].normal);
	return run_a.length_squared() > CMP_EPSILON2 && run_b.length_squared() > CMP_EPSILON2 &&
			Math::abs(edge_direction.dot(run_a)) >= p_cos_collinear &&
			Math::abs(edge_direction.dot(run_b)) >= p_cos_collinear;
}

bool collect_islands(const Vector<int> &p_faces, int p_mode, const Vector<FaceGeometry> &p_geometry,
		const PackedVector3Array &p_positions, const PackedInt32Array &p_loop_vertices,
		const PackedInt32Array &p_face_starts, const PackedInt32Array &p_face_counts,
		const Ref<LevelMeshAdjacency> &p_adjacency, real_t p_cos_coplanar,
		real_t p_cos_collinear, Vector<HotspotIsland> &r_islands, Vector<int> &r_face_island) {
	r_islands.clear();
	HashSet<int> selected;
	for (const int face_id : p_faces) {
		selected.insert(face_id);
	}

	if (p_mode == HotspotFitter::ISLAND_INDIVIDUAL) {
		for (const int face_id : p_faces) {
			HotspotIsland island;
			island.faces.push_back(face_id);
			const int loop_start = p_face_starts[face_id];
			for (int corner = 0; corner < p_face_counts[face_id]; corner++) {
				const int vertex_id = p_loop_vertices[loop_start + corner];
				if (island.vertex_ids.find(vertex_id) < 0) {
					island.vertex_ids.push_back(vertex_id);
				}
			}
			island.vertex_ids.sort();
			r_face_island.write[face_id] = r_islands.size();
			r_islands.push_back(island);
		}
		return true;
	}

	HashSet<int> assigned;
	for (const int root_face : p_faces) {
		if (assigned.has(root_face)) {
			continue;
		}
		HotspotIsland island;
		Vector<int> queue;
		queue.push_back(root_face);
		assigned.insert(root_face);
		for (int cursor = 0; cursor < queue.size(); cursor++) {
			const int face_a = queue[cursor];
			island.faces.push_back(face_a);
			Vector<int> edges;
			for (const int edge_id : p_adjacency->get_face_edges(face_a)) {
				if (edge_id >= 0) {
					edges.push_back(edge_id);
				}
			}
			edges.sort();
			for (const int edge_id : edges) {
				Vector<int> neighbors;
				for (const int radial_face : p_adjacency->get_edge_faces(edge_id)) {
					if (radial_face != face_a && selected.has(radial_face)) {
						neighbors.push_back(radial_face);
					}
				}
				neighbors.sort();
				for (const int face_b : neighbors) {
					if (!assigned.has(face_b) && can_merge_faces(face_a, face_b, edge_id, p_geometry,
							p_positions, p_adjacency, p_cos_coplanar, p_cos_collinear)) {
						assigned.insert(face_b);
						queue.push_back(face_b);
					}
				}
			}
		}
		island.faces.sort();
		for (const int face_id : island.faces) {
			r_face_island.write[face_id] = r_islands.size();
			const int loop_start = p_face_starts[face_id];
			for (int corner = 0; corner < p_face_counts[face_id]; corner++) {
				const int vertex_id = p_loop_vertices[loop_start + corner];
				if (island.vertex_ids.find(vertex_id) < 0) {
					island.vertex_ids.push_back(vertex_id);
				}
			}
		}
		island.vertex_ids.sort();
		for (const int face_a : island.faces) {
			for (const int edge_id : p_adjacency->get_face_edges(face_a)) {
				for (const int face_b : p_adjacency->get_edge_faces(edge_id)) {
					if (face_b > face_a && island.faces.find(face_b) >= 0 &&
							p_geometry[face_a].normal.dot(p_geometry[face_b].normal) < p_cos_coplanar) {
						island.has_fold = true;
					}
				}
			}
		}
		r_islands.push_back(island);
	}
	return true;
}

bool build_island_layout(const HotspotIsland &p_island, const Vector<FaceGeometry> &p_geometry,
		const Ref<LevelMesh> &p_mesh, const Ref<LevelMeshData> &p_data,
		const PackedVector3Array &p_world_positions, const PackedInt32Array &p_loop_vertices,
		const PackedInt32Array &p_face_starts, const PackedInt32Array &p_face_counts,
		const Vector<int> &p_face_island, int p_island_index, real_t p_horizontal_bias_degrees,
		IslandLayout &r_layout) {
	const Ref<LevelMeshAdjacency> adjacency = p_mesh->get_adjacency();
	r_layout.loop_coordinates.resize(p_data->get_loop_vertex_indices().size());
	r_layout.loop_coordinate_set.resize(r_layout.loop_coordinates.size());
	for (int i = 0; i < r_layout.loop_coordinate_set.size(); i++) {
		r_layout.loop_coordinate_set.set(i, 0);
	}

	Vector3 average_normal;
	for (const int face_id : p_island.faces) {
		average_normal += p_geometry[face_id].normal * p_geometry[face_id].area_x2;
	}
	if (average_normal.length_squared() <= CMP_EPSILON2) {
		return false;
	}
	average_normal.normalize();

	if (!p_island.has_fold) {
		const Vector3 tangent = LevelMesh::grid_uv_tangent_for_normal(average_normal);
		const Vector3 bitangent = average_normal.cross(tangent).normalized();
		if (tangent.length_squared() <= CMP_EPSILON2 || bitangent.length_squared() <= CMP_EPSILON2) {
			return false;
		}
		for (const int face_id : p_island.faces) {
			const int loop_start = p_face_starts[face_id];
			for (int corner = 0; corner < p_face_counts[face_id]; corner++) {
				const int loop_id = loop_start + corner;
				const Vector3 position = p_world_positions[p_loop_vertices[loop_id]];
				r_layout.loop_coordinates.set(loop_id, Vector2(position.dot(tangent), position.dot(bitangent)));
				r_layout.loop_coordinate_set.set(loop_id, 1);
			}
		}
	} else {
		// Reuse WP16's single-hinge unfold verbatim. The only setup here is an
		// isometric dominant-axis seed; every subsequent face is flattened by
		// LevelMesh::unfold_face_across_edge along a lowest-id BFS tree.
		Ref<LevelMeshData> working = p_data->duplicate_data();
		working->set_vertex_positions(p_world_positions);
		PackedVector2Array working_uvs = working->get_loop_uv0();
		const int root_face = p_island.faces[0];
		const Vector3 root_normal = p_geometry[root_face].normal;
		const Vector3 root_tangent = LevelMesh::grid_uv_tangent_for_normal(root_normal);
		const Vector3 root_bitangent = root_normal.cross(root_tangent).normalized();
		if (root_tangent.length_squared() <= CMP_EPSILON2 || root_bitangent.length_squared() <= CMP_EPSILON2) {
			return false;
		}
		const int root_start = p_face_starts[root_face];
		for (int corner = 0; corner < p_face_counts[root_face]; corner++) {
			const int loop_id = root_start + corner;
			const Vector3 position = p_world_positions[p_loop_vertices[loop_id]];
			working_uvs.set(loop_id, Vector2(position.dot(root_tangent), position.dot(root_bitangent)));
			r_layout.loop_coordinates.set(loop_id, working_uvs[loop_id]);
			r_layout.loop_coordinate_set.set(loop_id, 1);
		}
		working->set_loop_uv0(working_uvs);

		Vector<uint8_t> visited;
		Vector<real_t> accumulated_distortion;
		visited.resize(p_face_island.size());
		accumulated_distortion.resize(p_face_island.size());
		for (int i = 0; i < visited.size(); i++) {
			visited.write[i] = 0;
			accumulated_distortion.write[i] = 0.0;
		}
		visited.write[root_face] = 1;
		Vector<int> queue;
		queue.push_back(root_face);
		for (int cursor = 0; cursor < queue.size(); cursor++) {
			const int face_a = queue[cursor];
			Vector<int> edges;
			for (const int edge_id : adjacency->get_face_edges(face_a)) {
				if (edge_id >= 0) {
					edges.push_back(edge_id);
				}
			}
			edges.sort();
			for (const int edge_id : edges) {
				Vector<int> neighbors;
				for (const int radial_face : adjacency->get_edge_faces(edge_id)) {
					if (radial_face >= 0 && radial_face < p_face_island.size() && radial_face != face_a &&
							p_face_island[radial_face] == p_island_index) {
						neighbors.push_back(radial_face);
					}
				}
				neighbors.sort();
				for (const int face_b : neighbors) {
					if (visited[face_b] != 0) {
						continue;
					}
					const Dictionary unfolded = LevelMesh::unfold_face_across_edge(working, face_a, edge_id, face_b);
					if (!bool(unfolded.get("valid", false))) {
						return false;
					}
					const PackedInt32Array loop_ids = unfolded.get("loop_ids", PackedInt32Array());
					const PackedVector2Array uvs = unfolded.get("uvs", PackedVector2Array());
					if (loop_ids.size() != p_face_counts[face_b] || loop_ids.size() != uvs.size()) {
						return false;
					}
					for (int i = 0; i < loop_ids.size(); i++) {
						working_uvs.set(loop_ids[i], uvs[i]);
						r_layout.loop_coordinates.set(loop_ids[i], uvs[i]);
						r_layout.loop_coordinate_set.set(loop_ids[i], 1);
					}
					working->set_loop_uv0(working_uvs);
					accumulated_distortion.write[face_b] = accumulated_distortion[face_a] +
							(real_t)unfolded.get("dihedral_angle", (real_t)0.0);
					r_layout.distortion = MAX(r_layout.distortion, accumulated_distortion[face_b]);
					visited.write[face_b] = 1;
					queue.push_back(face_b);
				}
			}
		}
		for (const int face_id : p_island.faces) {
			if (visited[face_id] == 0) {
				return false;
			}
		}
	}

	bool found_axis = false;
	bool found_horizontal = false;
	real_t chosen_length_squared = -1.0;
	uint64_t chosen_key = UINT64_MAX;
	Vector2 chosen_delta;
	const real_t horizontal_sine = Math::sin(Math::deg_to_rad(p_horizontal_bias_degrees));
	for (const int face_id : p_island.faces) {
		const PackedInt32Array face_edges = adjacency->get_face_edges(face_id);
		const int loop_start = p_face_starts[face_id];
		const int loop_count = p_face_counts[face_id];
		for (int corner = 0; corner < loop_count; corner++) {
			const int edge_id = corner < face_edges.size() ? face_edges[corner] : -1;
			int island_radial_count = 0;
			if (edge_id >= 0) {
				for (const int radial_face : adjacency->get_edge_faces(edge_id)) {
					if (radial_face >= 0 && radial_face < p_face_island.size() &&
							p_face_island[radial_face] == p_island_index) {
						island_radial_count++;
					}
				}
			}
			if (island_radial_count > 1) {
				continue;
			}
			const int loop_a = loop_start + corner;
			const int loop_b = loop_start + ((corner + 1) % loop_count);
			if (r_layout.loop_coordinate_set[loop_a] == 0 || r_layout.loop_coordinate_set[loop_b] == 0) {
				continue;
			}
			const int vertex_a = p_loop_vertices[loop_a];
			const int vertex_b = p_loop_vertices[loop_b];
			const Vector3 world_delta = p_world_positions[vertex_b] - p_world_positions[vertex_a];
			const Vector2 planar_delta = r_layout.loop_coordinates[loop_b] - r_layout.loop_coordinates[loop_a];
			const real_t length_squared = world_delta.length_squared();
			if (length_squared <= CMP_EPSILON2 || planar_delta.length_squared() <= CMP_EPSILON2) {
				continue;
			}
			const bool horizontal = Math::abs(world_delta.y) <= Math::sqrt(length_squared) * horizontal_sine;
			const uint32_t lower = uint32_t(MIN(vertex_a, vertex_b));
			const uint32_t upper = uint32_t(MAX(vertex_a, vertex_b));
			const uint64_t key = (uint64_t(lower) << 32) | upper;
			const bool preferred_class = horizontal && !found_horizontal;
			const bool eligible_class = horizontal == found_horizontal || preferred_class;
			const bool longer = length_squared > chosen_length_squared + CMP_EPSILON;
			const bool exact_tie = Math::is_equal_approx(length_squared, chosen_length_squared) && key < chosen_key;
			if (!found_axis || preferred_class || (eligible_class && (longer || exact_tie))) {
				found_axis = true;
				found_horizontal = found_horizontal || horizontal;
				chosen_length_squared = length_squared;
				chosen_key = key;
				chosen_delta = vertex_a <= vertex_b ? planar_delta : -planar_delta;
			}
		}
	}
	if (!found_axis || chosen_delta.length_squared() <= CMP_EPSILON2) {
		return false;
	}
	r_layout.axis_u = chosen_delta.normalized();
	r_layout.axis_v = Vector2(-r_layout.axis_u.y, r_layout.axis_u.x);
	r_layout.min_u = Math::INF;
	r_layout.max_u = -Math::INF;
	r_layout.min_v = Math::INF;
	r_layout.max_v = -Math::INF;
	for (const int face_id : p_island.faces) {
		const int loop_start = p_face_starts[face_id];
		for (int corner = 0; corner < p_face_counts[face_id]; corner++) {
			const int loop_id = loop_start + corner;
			const Vector2 point = r_layout.loop_coordinates[loop_id];
			const real_t u = point.dot(r_layout.axis_u);
			const real_t v = point.dot(r_layout.axis_v);
			r_layout.min_u = MIN(r_layout.min_u, u);
			r_layout.max_u = MAX(r_layout.max_u, u);
			r_layout.min_v = MIN(r_layout.min_v, v);
			r_layout.max_v = MAX(r_layout.max_v, v);
		}
	}
	r_layout.world_w = r_layout.max_u - r_layout.min_u;
	r_layout.world_h = r_layout.max_v - r_layout.min_v;
	return Math::is_finite(r_layout.world_w) && Math::is_finite(r_layout.world_h) &&
			r_layout.world_w > CMP_EPSILON && r_layout.world_h > CMP_EPSILON;
}

bool orientation_needs_swap(real_t p_patch_aspect, real_t p_want_aspect) {
	if (Math::is_equal_approx(p_patch_aspect, (real_t)1.0)) {
		return false;
	}
	return (p_patch_aspect >= (real_t)1.0) != (p_want_aspect >= (real_t)1.0);
}

Vector2 rotate_quarter(const Vector2 &p_point, int p_quarter_turns) {
	switch (p_quarter_turns & 3) {
		case 1: return Vector2(p_point.y, (real_t)1.0 - p_point.x);
		case 2: return Vector2((real_t)1.0 - p_point.x, (real_t)1.0 - p_point.y);
		case 3: return Vector2((real_t)1.0 - p_point.y, p_point.x);
		default: return p_point;
	}
}

real_t repeat_coordinate(real_t p_coordinate, real_t p_face_center, int p_repetitions) {
	if (p_repetitions <= 1) {
		return CLAMP(p_coordinate, (real_t)0.0, (real_t)1.0);
	}
	if (Math::is_zero_approx(p_coordinate)) {
		return 0.0;
	}
	if (Math::is_equal_approx(p_coordinate, (real_t)1.0)) {
		return 1.0;
	}
	const real_t scaled = p_coordinate * p_repetitions;
	const real_t nearest = Math::round(scaled);
	if (Math::is_equal_approx(scaled, nearest)) {
		return p_coordinate <= p_face_center ? (real_t)1.0 : (real_t)0.0;
	}
	return Math::fposmod(scaled, (real_t)1.0);
}

bool solve_affine_transform(const Vector<Vector2> &p_source, const Vector<Vector2> &p_target,
		Transform2D &r_transform) {
	if (p_source.size() != p_target.size() || p_source.size() < 3) {
		return false;
	}
	const Vector2 source_0 = p_source[0];
	const Vector2 target_0 = p_target[0];
	for (int i = 1; i + 1 < p_source.size(); i++) {
		for (int j = i + 1; j < p_source.size(); j++) {
			const Vector2 source_x = p_source[i] - source_0;
			const Vector2 source_y = p_source[j] - source_0;
			const real_t determinant = source_x.cross(source_y);
			if (Math::abs(determinant) <= CMP_EPSILON) {
				continue;
			}
			const Vector2 target_x = p_target[i] - target_0;
			const Vector2 target_y = p_target[j] - target_0;
			const Vector2 column_x = (target_x * source_y.y - target_y * source_x.y) / determinant;
			const Vector2 column_y = (-target_x * source_y.x + target_y * source_x.x) / determinant;
			const Vector2 origin = target_0 - column_x * source_0.x - column_y * source_0.y;
			const Transform2D candidate(column_x, column_y, origin);
			if (!candidate.is_finite()) {
				continue;
			}
			bool matches = true;
			for (int point = 0; point < p_source.size(); point++) {
				if (candidate.xform(p_source[point]).distance_squared_to(p_target[point]) > (real_t)1e-8) {
					matches = false;
					break;
				}
			}
			if (matches) {
				r_transform = candidate;
				return true;
			}
		}
	}
	return false;
}

real_t option_real(const Dictionary &p_options, const StringName &p_name, real_t p_default,
		real_t p_minimum, real_t p_maximum) {
	const Variant value = p_options.get(p_name, p_default);
	if (value.get_type() != Variant::FLOAT && value.get_type() != Variant::INT) {
		return p_default;
	}
	const real_t converted = value;
	return Math::is_finite(converted) ? CLAMP(converted, p_minimum, p_maximum) : p_default;
}

} // namespace

Dictionary HotspotFitter::fit(const PackedInt32Array &p_face_ids, const Ref<LevelMesh> &p_mesh,
		const Ref<HotspotAtlas> &p_atlas, int p_island_mode, int64_t p_seed,
		const Dictionary &p_options) const {
	if (p_mesh.is_null() || p_mesh->get_data().is_null() || p_atlas.is_null()) {
		return fit_error("invalid_input");
	}
	if (p_island_mode < ISLAND_GROUPED || p_island_mode > ISLAND_INDIVIDUAL) {
		return fit_error("invalid_island_mode");
	}
	if (p_face_ids.is_empty()) {
		return fit_error("empty_selection");
	}
	const Ref<LevelMeshData> data = p_mesh->get_data();
	const Ref<LevelMeshAdjacency> adjacency = p_mesh->get_adjacency();
	if (adjacency.is_null() || !adjacency->is_valid()) {
		return fit_error("invalid_topology");
	}

	const real_t density_margin = option_real(p_options, "density_margin", DEFAULT_DENSITY_MARGIN, 0.0, 8.0);
	const real_t aspect_margin = option_real(p_options, "aspect_margin", DEFAULT_ASPECT_MARGIN, 0.0, 8.0);
	const real_t cos_coplanar = option_real(p_options, "cos_coplanar", DEFAULT_COS_COPLANAR, -1.0, 1.0);
	const real_t cos_collinear = option_real(p_options, "cos_collinear", DEFAULT_COS_COLLINEAR, -1.0, 1.0);
	const real_t horizontal_bias_degrees = option_real(p_options, "horizontal_bias_degrees",
			DEFAULT_HORIZONTAL_BIAS_DEGREES, 0.0, 90.0);
	const real_t inset_mip_bleed = option_real(p_options, "inset_mip_bleed", DEFAULT_INSET_MIP_BLEED, 0.0, 64.0);
	const real_t automatic_distortion_threshold = option_real(p_options, "automatic_distortion_threshold",
			LevelMesh::DEFAULT_CONFORMING_DISTORTION_THRESHOLD, 0.0, Math::TAU);
	const real_t texel_density = option_real(p_options, "texel_density", p_atlas->get_texel_density_target(),
			(real_t)0.001, (real_t)1048576.0);

	Transform3D mesh_to_world;
	const Variant transform_option = p_options.get("mesh_to_world", Transform3D());
	if (transform_option.get_type() == Variant::TRANSFORM3D) {
		mesh_to_world = transform_option;
	}
	if (!mesh_to_world.is_finite()) {
		return fit_error("invalid_mesh_to_world");
	}

	Size2i texture_size = p_atlas->get_reference_texture_size();
	const Variant texture_size_option = p_options.get("texture_size", texture_size);
	if (texture_size_option.get_type() == Variant::VECTOR2I) {
		texture_size = texture_size_option;
	}
	if (texture_size.x <= 0 || texture_size.y <= 0) {
		return fit_error("invalid_texture_size");
	}

	int requested_mapping_mode = p_atlas->get_default_mapping_mode();
	const Variant mapping_option = p_options.get("mapping_mode", requested_mapping_mode);
	if (mapping_option.get_type() == Variant::INT) {
		requested_mapping_mode = mapping_option;
	}
	if (requested_mapping_mode < HotspotAtlas::MAPPING_AUTOMATIC ||
			requested_mapping_mode > HotspotAtlas::MAPPING_FOLLOW_ACTIVE_QUADS) {
		return fit_error("invalid_mapping_mode");
	}

	const PackedVector3Array local_positions = data->get_vertex_positions();
	PackedVector3Array world_positions;
	world_positions.resize(local_positions.size());
	for (int vertex_id = 0; vertex_id < local_positions.size(); vertex_id++) {
		world_positions.set(vertex_id, mesh_to_world.xform(local_positions[vertex_id]));
	}
	const PackedInt32Array loop_vertices = data->get_loop_vertex_indices();
	const PackedInt32Array face_starts = data->get_face_loop_starts();
	const PackedInt32Array face_counts = data->get_face_loop_counts();
	const PackedByteArray face_alive = data->get_face_alive();

	Vector<int> faces;
	for (const int face_id : p_face_ids) {
		if (faces.find(face_id) < 0) {
			faces.push_back(face_id);
		}
	}
	faces.sort();
	Vector<FaceGeometry> geometry;
	geometry.resize(face_alive.size());
	for (const int face_id : faces) {
		if (!compute_face_geometry(world_positions, loop_vertices, face_starts, face_counts,
				face_alive, face_id, geometry.write[face_id])) {
			return fit_error("invalid_face_" + itos(face_id));
		}
	}

	Vector<int> face_island;
	face_island.resize(face_alive.size());
	for (int i = 0; i < face_island.size(); i++) {
		face_island.write[i] = -1;
	}
	Vector<HotspotIsland> islands;
	if (!collect_islands(faces, p_island_mode, geometry, world_positions, loop_vertices,
			face_starts, face_counts, adjacency, cos_coplanar, cos_collinear, islands, face_island)) {
		return fit_error("island_partition_failed");
	}

	const TypedArray<HotspotPatch> atlas_patches = p_atlas->get_patches();
	if (atlas_patches.is_empty()) {
		return fit_error("atlas_has_no_patches");
	}
	Array face_results;
	Array diagnostics;
	Vector<StringName> chosen_by_island;
	chosen_by_island.resize(islands.size());

	String atlas_identity = String(p_atlas->get_atlas_id());
	if (atlas_identity.is_empty()) {
		atlas_identity = p_atlas->get_path();
	}
	if (atlas_identity.is_empty() && p_atlas->get_reference_texture().is_valid()) {
		atlas_identity = p_atlas->get_reference_texture()->get_path();
	}
	const uint64_t atlas_hash = stable_string_hash(atlas_identity);

	for (int island_index = 0; island_index < islands.size(); island_index++) {
		const HotspotIsland &island = islands[island_index];
		IslandLayout layout;
		if (!build_island_layout(island, geometry, p_mesh, data, world_positions, loop_vertices,
				face_starts, face_counts, face_island, island_index, horizontal_bias_degrees, layout)) {
			return fit_error("island_layout_failed_" + itos(island_index));
		}
		const real_t want_area = layout.world_w * layout.world_h * texel_density * texel_density;
		const real_t want_aspect = layout.world_w / layout.world_h;
		if (!Math::is_finite(want_area) || !Math::is_finite(want_aspect) ||
				want_area <= CMP_EPSILON || want_aspect <= CMP_EPSILON) {
			return fit_error("invalid_island_metrics_" + itos(island_index));
		}

		Vector<PatchCandidate> candidates;
		for (int patch_index = 0; patch_index < atlas_patches.size(); patch_index++) {
			Ref<HotspotPatch> patch = atlas_patches[patch_index];
			if (patch.is_null()) {
				continue;
			}
			const bool tiling = patch->is_tiling_allowed();
			if ((p_atlas->get_tiling_policy() == HotspotAtlas::TILING_NO && tiling) ||
					(p_atlas->get_tiling_policy() == HotspotAtlas::TILING_ONLY && !tiling)) {
				continue;
			}
			const Rect2 rect = patch->get_rect_uv();
			const real_t width_px = rect.size.x * texture_size.x;
			const real_t height_px = rect.size.y * texture_size.y;
			const real_t area_texels = width_px * height_px;
			if (!Math::is_finite(width_px) || !Math::is_finite(height_px) || width_px <= CMP_EPSILON ||
					height_px <= CMP_EPSILON || area_texels <= CMP_EPSILON) {
				continue;
			}
			const real_t raw_aspect = width_px / height_px;
			const bool needs_swap = orientation_needs_swap(raw_aspect, want_aspect);
			if (needs_swap && !patch->is_rotation_allowed()) {
				continue;
			}
			const real_t effective_aspect = needs_swap ? (real_t)1.0 / raw_aspect : raw_aspect;
			PatchCandidate candidate;
			candidate.patch = patch;
			candidate.patch_index = patch_index;
			candidate.name = String(patch->get_patch_name());
			candidate.width_px = width_px;
			candidate.height_px = height_px;
			candidate.aspect = raw_aspect;
			candidate.needs_swap = needs_swap;
			candidate.density_error = Math::abs(log2_positive(area_texels) - log2_positive(want_area));
			candidate.aspect_error = Math::abs(log2_positive(effective_aspect) - log2_positive(want_aspect));
			candidates.push_back(candidate);
		}
		if (candidates.is_empty()) {
			return fit_error("no_patch_candidates_" + itos(island_index));
		}
		candidates.sort_custom<PatchCandidateNameSort>();

		real_t best_density_error = Math::INF;
		for (const PatchCandidate &candidate : candidates) {
			best_density_error = MIN(best_density_error, candidate.density_error);
		}
		Vector<int> density_bucket;
		for (int i = 0; i < candidates.size(); i++) {
			if (candidates[i].density_error <= best_density_error + density_margin + CMP_EPSILON) {
				density_bucket.push_back(i);
			}
		}
		real_t best_aspect_error = Math::INF;
		for (const int index : density_bucket) {
			best_aspect_error = MIN(best_aspect_error, candidates[index].aspect_error);
		}
		Vector<int> finalists;
		for (const int index : density_bucket) {
			if (candidates[index].aspect_error <= best_aspect_error + aspect_margin + CMP_EPSILON) {
				finalists.push_back(index);
			}
		}
		if (finalists.is_empty()) {
			return fit_error("no_patch_finalists_" + itos(island_index));
		}

		int neighboring_island = -1;
		for (const int face_id : island.faces) {
			for (const int edge_id : adjacency->get_face_edges(face_id)) {
				for (const int radial_face : adjacency->get_edge_faces(edge_id)) {
					if (radial_face < 0 || radial_face >= face_island.size()) {
						continue;
					}
					const int candidate_island = face_island[radial_face];
					if (candidate_island >= 0 && candidate_island < island_index &&
							(neighboring_island < 0 || candidate_island < neighboring_island)) {
						neighboring_island = candidate_island;
					}
				}
			}
		}

		const uint64_t fit_seed = uint64_t(p_seed) ^ island_vertex_hash(island.vertex_ids) ^ atlas_hash;
		StableRNG rng(fit_seed);
		int rolled_finalist = 0;
		if (!p_atlas->is_random_disallowed()) {
			rolled_finalist = rng.range(finalists.size());
			if (neighboring_island >= 0 && finalists.size() > 1 &&
					StringName(candidates[finalists[rolled_finalist]].name) == chosen_by_island[neighboring_island]) {
				Vector<int> non_repeating;
				for (int i = 0; i < finalists.size(); i++) {
					if (i != rolled_finalist) {
						non_repeating.push_back(i);
					}
				}
				rolled_finalist = non_repeating[rng.range(non_repeating.size())];
			}
		}
		int chosen_candidate = finalists[p_atlas->is_random_disallowed() ? 0 : rolled_finalist];

		String sticky_name;
		for (const int face_id : island.faces) {
			const String stored = data->get_face_hotspot_patch_name(face_id);
			if (!stored.is_empty()) {
				sticky_name = stored;
				break;
			}
		}
		if (!sticky_name.is_empty()) {
			for (const int finalist : finalists) {
				if (candidates[finalist].name == sticky_name) {
					chosen_candidate = finalist;
					break;
				}
			}
		}
		const PatchCandidate &chosen = candidates[chosen_candidate];
		chosen_by_island.write[island_index] = StringName(chosen.name);

		int quarter_turns = 0;
		if (chosen.needs_swap) {
			quarter_turns = 1;
		} else if (chosen.patch->is_rotation_allowed() && Math::is_equal_approx(chosen.aspect, (real_t)1.0)) {
			quarter_turns = rng.range(4);
		}
		const bool mirror_x = chosen.patch->is_mirror_x_allowed() && rng.bit();
		const bool mirror_y = chosen.patch->is_mirror_y_allowed() && rng.bit();
		const bool axes_swapped = (quarter_turns & 1) != 0;
		const Vector2 oriented_world_size = axes_swapped ?
				Vector2(layout.world_h, layout.world_w) : Vector2(layout.world_w, layout.world_h);
		int repetitions = 1;
		if (chosen.patch->is_tiling_allowed()) {
			const bool tile_u = chosen.patch->get_tiling_axis() == HotspotPatch::TILING_AXIS_U;
			const real_t island_length = tile_u ? oriented_world_size.x : oriented_world_size.y;
			const real_t patch_length_px = tile_u ? chosen.width_px : chosen.height_px;
			repetitions = MAX(1, int(Math::round(island_length * texel_density / patch_length_px)));
		}

		const real_t minimum_patch_dimension = MIN(chosen.width_px, chosen.height_px);
		const int mip_levels = MAX(0, int(Math::floor(log2_positive(minimum_patch_dimension))));
		const real_t inset_px = chosen.patch->get_inset_px() + inset_mip_bleed * mip_levels;
		const Vector2 inset_uv(inset_px / texture_size.x, inset_px / texture_size.y);
		const Rect2 patch_rect = chosen.patch->get_rect_uv();
		const Rect2 fit_rect(patch_rect.position + inset_uv, patch_rect.size - inset_uv * (real_t)2.0);
		if (!fit_rect.has_area() || !fit_rect.position.is_finite() || !fit_rect.size.is_finite()) {
			return fit_error("patch_inset_exhausted_" + chosen.name);
		}

		int effective_mapping_mode = requested_mapping_mode;
		if (effective_mapping_mode == HotspotAtlas::MAPPING_AUTOMATIC) {
			effective_mapping_mode = layout.distortion > automatic_distortion_threshold ?
					HotspotAtlas::MAPPING_CONFORMING : HotspotAtlas::MAPPING_SQUARE;
		}
		const bool island_requires_explicit = island.has_fold || repetitions > 1 ||
				effective_mapping_mode == HotspotAtlas::MAPPING_CONFORMING ||
				effective_mapping_mode == HotspotAtlas::MAPPING_FOLLOW_ACTIVE_QUADS;

		const real_t atlas_u_px = chosen.width_px *
				(chosen.patch->get_tiling_axis() == HotspotPatch::TILING_AXIS_U ? repetitions : 1);
		const real_t atlas_v_px = chosen.height_px *
				(chosen.patch->get_tiling_axis() == HotspotPatch::TILING_AXIS_V ? repetitions : 1);
		const real_t realized_u = axes_swapped ? atlas_v_px / layout.world_w : atlas_u_px / layout.world_w;
		const real_t realized_v = axes_swapped ? atlas_u_px / layout.world_h : atlas_v_px / layout.world_h;

		PackedFloat32Array seam_parameters;
		for (int seam = 1; seam < repetitions; seam++) {
			seam_parameters.push_back((float)seam / (float)repetitions);
		}

		for (const int face_id : island.faces) {
			const int loop_start = face_starts[face_id];
			const int loop_count = face_counts[face_id];
			PackedInt32Array result_loop_ids;
			PackedVector2Array result_uvs;
			Vector<Vector2> normalized_points;
			normalized_points.resize(loop_count);
			Vector2 oriented_center;
			real_t face_arc_min = Math::INF;
			real_t face_arc_max = -Math::INF;
			for (int corner = 0; corner < loop_count; corner++) {
				const int loop_id = loop_start + corner;
				const Vector2 planar = layout.loop_coordinates[loop_id];
				const real_t frame_u = planar.dot(layout.axis_u);
				const real_t frame_v = planar.dot(layout.axis_v);
				const Vector2 normalized((frame_u - layout.min_u) / layout.world_w,
						(frame_v - layout.min_v) / layout.world_h);
				normalized_points.write[corner] = normalized;
				Vector2 oriented = rotate_quarter(normalized, quarter_turns);
				if (mirror_x) {
					oriented.x = (real_t)1.0 - oriented.x;
				}
				if (mirror_y) {
					oriented.y = (real_t)1.0 - oriented.y;
				}
				oriented_center += oriented;
				face_arc_min = MIN(face_arc_min, normalized.x);
				face_arc_max = MAX(face_arc_max, normalized.x);
			}
			oriented_center /= (real_t)loop_count;
			for (int corner = 0; corner < loop_count; corner++) {
				Vector2 oriented = rotate_quarter(normalized_points[corner], quarter_turns);
				if (mirror_x) {
					oriented.x = (real_t)1.0 - oriented.x;
				}
				if (mirror_y) {
					oriented.y = (real_t)1.0 - oriented.y;
				}
				if (chosen.patch->is_tiling_allowed() && repetitions > 1) {
					if (chosen.patch->get_tiling_axis() == HotspotPatch::TILING_AXIS_U) {
						oriented.x = repeat_coordinate(oriented.x, oriented_center.x, repetitions);
					} else {
						oriented.y = repeat_coordinate(oriented.y, oriented_center.y, repetitions);
					}
				}
				const Vector2 uv = fit_rect.position + oriented * fit_rect.size;
				if (!uv.is_finite()) {
					return fit_error("non_finite_uv_" + itos(face_id));
				}
				result_loop_ids.push_back(loop_start + corner);
				result_uvs.push_back(uv);
			}

			Dictionary face_result;
			face_result["face_index"] = face_id;
			face_result["face_id"] = face_id;
			face_result["island_index"] = island_index;
			face_result["patch_name"] = StringName(chosen.name);
			face_result["loop_ids"] = result_loop_ids;
			face_result["loop_uvs"] = result_uvs;
			face_result["repetitions"] = repetitions;
			face_result["tiling_seams"] = seam_parameters;
			face_result["arc_u_range"] = Vector2(face_arc_min, face_arc_max);
			face_result["realized_u_texels_per_meter"] = realized_u;
			face_result["realized_v_texels_per_meter"] = realized_v;
			face_result["mapping_mode"] = effective_mapping_mode;

			bool write_explicit = island_requires_explicit;
			Transform2D uv_transform;
			Vector3 uv_tangent;
			if (!write_explicit) {
				FaceGeometry local_geometry;
				if (!compute_face_geometry(local_positions, loop_vertices, face_starts, face_counts,
						face_alive, face_id, local_geometry)) {
					write_explicit = true;
				} else {
					uv_tangent = LevelMesh::grid_uv_tangent_for_normal(local_geometry.normal);
					const Vector3 bitangent = local_geometry.normal.cross(uv_tangent).normalized();
					Vector<Vector2> source_points;
					Vector<Vector2> target_points;
					source_points.resize(loop_count);
					target_points.resize(loop_count);
					for (int corner = 0; corner < loop_count; corner++) {
						const Vector3 position = local_positions[loop_vertices[loop_start + corner]];
						source_points.write[corner] = Vector2(position.dot(uv_tangent), position.dot(bitangent));
						target_points.write[corner] = result_uvs[corner];
					}
					if (uv_tangent.length_squared() <= CMP_EPSILON2 ||
							!solve_affine_transform(source_points, target_points, uv_transform)) {
						write_explicit = true;
					}
				}
			}
			face_result["uv_mode"] = write_explicit ? LevelMeshData::UV_MODE_EXPLICIT : LevelMeshData::UV_MODE_PROJECTED;
			face_result["uv_origin"] = Vector3();
			face_result["uv_tangent"] = write_explicit ? Vector3() : uv_tangent;
			face_result["uv_transform"] = write_explicit ? Transform2D() : uv_transform;
			face_results.push_back(face_result);
		}

		PackedStringArray finalist_names;
		for (const int finalist : finalists) {
			finalist_names.push_back(candidates[finalist].name);
		}
		PackedStringArray density_bucket_names;
		for (const int bucket_index : density_bucket) {
			density_bucket_names.push_back(candidates[bucket_index].name);
		}
		String decided_by = "random";
		if (density_bucket.size() == 1) {
			decided_by = "density-unique";
		} else if (finalists.size() == 1) {
			decided_by = "aspect";
		}
		PackedInt32Array diagnostic_faces;
		for (const int face_id : island.faces) {
			diagnostic_faces.push_back(face_id);
		}
		PackedInt32Array diagnostic_vertices;
		for (const int vertex_id : island.vertex_ids) {
			diagnostic_vertices.push_back(vertex_id);
		}
		Dictionary diagnostic;
		diagnostic["island_index"] = island_index;
		diagnostic["face_ids"] = diagnostic_faces;
		diagnostic["vertex_ids"] = diagnostic_vertices;
		diagnostic["decided_by"] = decided_by;
		diagnostic["want_area"] = want_area;
		diagnostic["want_aspect"] = want_aspect;
		diagnostic["density_bucket_names"] = density_bucket_names;
		diagnostic["finalist_names"] = finalist_names;
		diagnostic["chosen"] = chosen.name;
		diagnostic["world_w"] = layout.world_w;
		diagnostic["world_h"] = layout.world_h;
		diagnostic["developable_strip"] = island.has_fold;
		diagnostic["mapping_mode"] = effective_mapping_mode;
		diagnostic["repetitions"] = repetitions;
		diagnostic["seed"] = (int64_t)fit_seed;
		diagnostic["atlas_path"] = p_atlas->get_path();
		diagnostics.push_back(diagnostic);
		// Carry the exact per-island decision alongside each face result so the
		// transactional apply seam can retain it ephemerally without widening
		// apply_hotspot_fit's established signature.
		for (int result_index = 0; result_index < face_results.size(); result_index++) {
			Dictionary face_result = face_results[result_index];
			if (int(face_result.get("island_index", -1)) == island_index && !face_result.has("fit_diagnostic")) {
				face_result["fit_diagnostic"] = diagnostic;
				face_results[result_index] = face_result;
			}
		}
	}

	Dictionary result;
	result["ok"] = true;
	result["error"] = String();
	result["faces"] = face_results;
	result["diagnostics"] = diagnostics;
	result["island_count"] = islands.size();
	return result;
}

void HotspotFitter::_bind_methods() {
	ClassDB::bind_method(D_METHOD("fit", "face_ids", "mesh", "atlas", "island_mode", "seed", "options"),
			&HotspotFitter::fit, DEFVAL(Dictionary()));

	BIND_ENUM_CONSTANT(ISLAND_GROUPED);
	BIND_ENUM_CONSTANT(ISLAND_INDIVIDUAL);
	BIND_CONSTANT(DEFAULT_DENSITY_MARGIN);
	BIND_CONSTANT(DEFAULT_ASPECT_MARGIN);
	BIND_CONSTANT(DEFAULT_COS_COPLANAR);
	BIND_CONSTANT(DEFAULT_COS_COLLINEAR);
	BIND_CONSTANT(DEFAULT_HORIZONTAL_BIAS_DEGREES);
	BIND_CONSTANT(DEFAULT_INSET_MIP_BLEED);
}
