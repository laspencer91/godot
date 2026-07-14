/**************************************************************************/
/*  level_mesh_element_bvh.cpp                                            */
/**************************************************************************/

#include "level_mesh_element_bvh.h"

#include "level_mesh_data.h"

#include "core/math/geometry_3d.h"
#include "core/math/math_funcs.h"
#include "core/object/class_db.h"
#include "core/templates/sort_array.h"

int LevelMeshElementBVH::_create_bvh(BVHNode *p_nodes, BVHNode **p_leaf_ptrs, int p_from, int p_size, int &r_next_node) {
	if (p_size <= 0) {
		return -1;
	}
	if (p_size == 1) {
		return (int)(p_leaf_ptrs[p_from] - p_nodes);
	}

	AABB bounds = p_leaf_ptrs[p_from]->aabb;
	for (int i = 1; i < p_size; i++) {
		bounds.merge_with(p_leaf_ptrs[p_from + i]->aabb);
	}
	switch (bounds.get_longest_axis_index()) {
		case Vector3::AXIS_X: {
			SortArray<BVHNode *, BVHCmpX> sorter;
			sorter.nth_element(0, p_size, p_size / 2, &p_leaf_ptrs[p_from]);
		} break;
		case Vector3::AXIS_Y: {
			SortArray<BVHNode *, BVHCmpY> sorter;
			sorter.nth_element(0, p_size, p_size / 2, &p_leaf_ptrs[p_from]);
		} break;
		case Vector3::AXIS_Z: {
			SortArray<BVHNode *, BVHCmpZ> sorter;
			sorter.nth_element(0, p_size, p_size / 2, &p_leaf_ptrs[p_from]);
		} break;
	}

	const int left = _create_bvh(p_nodes, p_leaf_ptrs, p_from, p_size / 2, r_next_node);
	const int right = _create_bvh(p_nodes, p_leaf_ptrs, p_from + p_size / 2, p_size - p_size / 2, r_next_node);
	const int node_id = r_next_node++;
	BVHNode &node = p_nodes[node_id];
	node.aabb = bounds;
	node.center = bounds.get_center();
	node.left = left;
	node.right = right;
	node.triangle_index = -1;
	return node_id;
}

void LevelMeshElementBVH::_set_data(const Ref<LevelMeshData> &p_data) {
	data = p_data;
	_mark_dirty();
}

void LevelMeshElementBVH::_mark_dirty() {
	dirty = true;
}

void LevelMeshElementBVH::_ensure_built() const {
	if (dirty) {
		_rebuild();
	}
}

void LevelMeshElementBVH::_rebuild() const {
	triangles.clear();
	nodes.clear();
	root_node = -1;
	valid = data.is_valid();
	dirty = false;
	if (data.is_null()) {
		return;
	}

	int next_tri_id = 0;
	for (int face_id = 0; face_id < data->face_alive.size(); face_id++) {
		if (data->face_alive[face_id] == 0) {
			continue;
		}
		if (!data->face_is_bakeable(face_id)) {
			valid = false;
			continue;
		}
		const int loop_start = data->face_loop_starts[face_id];
		const int loop_count = data->face_loop_counts[face_id];
		const Vector3 p0 = data->vertex_positions[data->loop_vertex_indices[loop_start]];
		// Keep the kernel's CCW loop fan and identity mapping. Rendering reverses
		// this order later at the baker seam; picking must not inherit that flip.
		for (int corner = 1; corner < loop_count - 1; corner++) {
			Triangle triangle;
			triangle.vertices[0] = p0;
			triangle.vertices[1] = data->vertex_positions[data->loop_vertex_indices[loop_start + corner]];
			triangle.vertices[2] = data->vertex_positions[data->loop_vertex_indices[loop_start + corner + 1]];
			triangle.tri_id = next_tri_id++;
			triangle.face_id = face_id;
			triangle.local_tri = corner - 1;
			triangles.push_back(triangle);
		}
	}

	if (triangles.is_empty()) {
		return;
	}
	nodes.resize(triangles.size() * 2);
	BVHNode *node_write = nodes.ptrw();
	Vector<BVHNode *> leaf_ptrs;
	leaf_ptrs.resize(triangles.size());
	BVHNode **leaf_write = leaf_ptrs.ptrw();
	for (int triangle_index = 0; triangle_index < triangles.size(); triangle_index++) {
		const Triangle &triangle = triangles[triangle_index];
		BVHNode &leaf = node_write[triangle_index];
		leaf.aabb = AABB(triangle.vertices[0], Vector3());
		leaf.aabb.expand_to(triangle.vertices[1]);
		leaf.aabb.expand_to(triangle.vertices[2]);
		leaf.center = leaf.aabb.get_center();
		leaf.left = -1;
		leaf.right = -1;
		leaf.triangle_index = triangle_index;
		leaf_write[triangle_index] = &leaf;
	}
	int next_node = triangles.size();
	root_node = _create_bvh(node_write, leaf_write, 0, triangles.size(), next_node);
	nodes.resize(next_node);
}

bool LevelMeshElementBVH::is_valid() const {
	_ensure_built();
	return valid;
}

int LevelMeshElementBVH::get_triangle_count() const {
	_ensure_built();
	return triangles.size();
}

Dictionary LevelMeshElementBVH::ray_closest(const Vector3 &p_local_origin, const Vector3 &p_local_direction) const {
	_ensure_built();
	Dictionary result;
	result["hit"] = false;
	result["t"] = Math::INF;
	result["face_id"] = -1;
	result["tri_id"] = -1;
	result["local_tri"] = -1;
	result["barycentric"] = Vector3();

	const real_t direction_length_squared = p_local_direction.length_squared();
	if (root_node < 0 || !p_local_origin.is_finite() || !p_local_direction.is_finite() || direction_length_squared <= CMP_EPSILON2) {
		return result;
	}

	real_t closest_t = Math::INF;
	int closest_triangle_index = -1;
	Vector3 closest_point;
	Vector<int> stack;
	stack.push_back(root_node);
	while (!stack.is_empty()) {
		const int node_id = stack[stack.size() - 1];
		stack.resize(stack.size() - 1);
		if (node_id < 0 || node_id >= nodes.size()) {
			continue;
		}
		const BVHNode &node = nodes[node_id];
		if (!node.aabb.intersects_ray(p_local_origin, p_local_direction)) {
			continue;
		}
		if (node.triangle_index >= 0) {
			if (node.triangle_index >= triangles.size()) {
				continue;
			}
			const Triangle &triangle = triangles[node.triangle_index];
			Vector3 intersection;
			if (!Geometry3D::ray_intersects_triangle(p_local_origin, p_local_direction,
						triangle.vertices[0], triangle.vertices[1], triangle.vertices[2], &intersection)) {
				continue;
			}
			const real_t t = (intersection - p_local_origin).dot(p_local_direction) / direction_length_squared;
			if (t < closest_t || (Math::is_equal_approx(t, closest_t) && (closest_triangle_index == -1 || triangle.tri_id < triangles[closest_triangle_index].tri_id))) {
				closest_t = t;
				closest_triangle_index = node.triangle_index;
				closest_point = intersection;
			}
		} else {
			if (node.right >= 0) {
				stack.push_back(node.right);
			}
			if (node.left >= 0) {
				stack.push_back(node.left);
			}
		}
	}

	if (closest_triangle_index == -1) {
		return result;
	}
	const Triangle &triangle = triangles[closest_triangle_index];
	result["hit"] = true;
	result["t"] = closest_t;
	result["face_id"] = triangle.face_id;
	result["tri_id"] = triangle.tri_id;
	result["local_tri"] = triangle.local_tri;
	result["barycentric"] = Geometry3D::triangle_get_barycentric_coords(
			triangle.vertices[0], triangle.vertices[1], triangle.vertices[2], closest_point);
	return result;
}

void LevelMeshElementBVH::_bind_methods() {
	ClassDB::bind_method(D_METHOD("is_valid"), &LevelMeshElementBVH::is_valid);
	ClassDB::bind_method(D_METHOD("get_triangle_count"), &LevelMeshElementBVH::get_triangle_count);
	ClassDB::bind_method(D_METHOD("ray_closest", "local_origin", "local_direction"), &LevelMeshElementBVH::ray_closest);
}
