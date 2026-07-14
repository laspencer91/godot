/**************************************************************************/
/*  level_mesh_element_bvh.h                                              */
/**************************************************************************/

#pragma once

#include "core/math/aabb.h"
#include "core/object/ref_counted.h"
#include "core/templates/vector.h"

class LevelMesh;
class LevelMeshData;

class LevelMeshElementBVH : public RefCounted {
	GDCLASS(LevelMeshElementBVH, RefCounted);

	struct Triangle {
		Vector3 vertices[3];
		int tri_id = -1;
		int face_id = -1;
		int local_tri = -1;
	};

	struct BVHNode {
		AABB aabb;
		Vector3 center;
		int left = -1;
		int right = -1;
		int triangle_index = -1;
	};

	struct BVHCmpX {
		bool operator()(const BVHNode *p_left, const BVHNode *p_right) const { return p_left->center.x < p_right->center.x; }
	};
	struct BVHCmpY {
		bool operator()(const BVHNode *p_left, const BVHNode *p_right) const { return p_left->center.y < p_right->center.y; }
	};
	struct BVHCmpZ {
		bool operator()(const BVHNode *p_left, const BVHNode *p_right) const { return p_left->center.z < p_right->center.z; }
	};

	Ref<LevelMeshData> data;
	mutable bool dirty = true;
	mutable bool valid = false;
	mutable Vector<Triangle> triangles;
	mutable Vector<BVHNode> nodes;
	mutable int root_node = -1;

	static int _create_bvh(BVHNode *p_nodes, BVHNode **p_leaf_ptrs, int p_from, int p_size, int &r_next_node);
	void _set_data(const Ref<LevelMeshData> &p_data);
	void _mark_dirty();
	void _ensure_built() const;
	void _rebuild() const;

	friend class LevelMesh;

protected:
	static void _bind_methods();

public:
	bool is_valid() const;
	int get_triangle_count() const;
	Dictionary ray_closest(const Vector3 &p_local_origin, const Vector3 &p_local_direction) const;
};
