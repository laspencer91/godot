/**************************************************************************/
/*  level_mesh_diff.h                                                     */
/**************************************************************************/

#pragma once

#include "core/object/ref_counted.h"

class LevelMesh;
class LevelMeshData;

class LevelMeshDiff : public RefCounted {
	GDCLASS(LevelMeshDiff, RefCounted);

	// LE0 records the changed range of every column as a single full span. The
	// representation is exact; later operators can narrow these spans without
	// changing apply/revert semantics or the public transaction API.
	Ref<LevelMeshData> before_data;
	Ref<LevelMeshData> after_data;
	PackedInt64Array removed_vertex_handles;
	PackedInt64Array removed_edge_handles;
	PackedInt64Array removed_face_handles;
	PackedInt64Array revert_removed_vertex_handles;
	PackedInt64Array revert_removed_edge_handles;
	PackedInt64Array revert_removed_face_handles;
	bool topology_changed = false;
	bool geometry_changed = false;
	bool empty = true;

	static void _compute_removed_handles(const PackedByteArray &p_from_alive, const PackedInt32Array &p_from_generations,
			const PackedByteArray &p_to_alive, const PackedInt32Array &p_to_generations, PackedInt64Array &r_removed);
	void _set_states(const Ref<LevelMeshData> &p_before, const Ref<LevelMeshData> &p_after, bool p_empty);

	friend class LevelMesh;

protected:
	static void _bind_methods();

public:
	Ref<LevelMeshData> get_before_data() const;
	Ref<LevelMeshData> get_after_data() const;
	PackedInt64Array get_removed_vertex_handles() const;
	PackedInt64Array get_removed_edge_handles() const;
	PackedInt64Array get_removed_face_handles() const;
	PackedInt64Array get_revert_removed_vertex_handles() const;
	PackedInt64Array get_revert_removed_edge_handles() const;
	PackedInt64Array get_revert_removed_face_handles() const;
	PackedInt64Array get_created_vertex_handles() const;
	PackedInt64Array get_created_edge_handles() const;
	PackedInt64Array get_created_face_handles() const;
	bool touches_topology() const;
	bool touches_geometry() const;
	bool is_empty() const;
};
