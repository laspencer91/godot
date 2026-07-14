/**************************************************************************/
/*  level_block.h                                                         */
/**************************************************************************/

#pragma once

#include "scene/3d/node_3d.h"

class CollisionShape3D;
class LevelMesh;
class LevelMeshData;
class MeshInstance3D;
class StaticBody3D;

class LevelBlock : public Node3D {
	GDCLASS(LevelBlock, Node3D);

	Ref<LevelMeshData> data;
	Ref<LevelMesh> level_mesh;
	MeshInstance3D *mesh_instance = nullptr;
	StaticBody3D *static_body = nullptr;
	CollisionShape3D *collision_shape = nullptr;

	void _data_changed();
	void _preview_changed();
	void _ensure_internal_nodes();

protected:
	static void _bind_methods();
	void _notification(int p_what);

public:
	void set_data(const Ref<LevelMeshData> &p_data);
	Ref<LevelMeshData> get_data() const;
	Ref<LevelMesh> get_level_mesh();
	void rebuild();

	LevelBlock();
	~LevelBlock();
};
