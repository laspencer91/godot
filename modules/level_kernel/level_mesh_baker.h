/**************************************************************************/
/*  level_mesh_baker.h                                                    */
/**************************************************************************/

#pragma once

#include "core/object/ref_counted.h"

class ArrayMesh;
class LevelMeshData;
class Material;

class LevelMeshBaker : public RefCounted {
	GDCLASS(LevelMeshBaker, RefCounted);

	static Vector3 _face_normal(const LevelMeshData &p_data, int p_face_id);

protected:
	static void _bind_methods();

public:
	static String get_builtin_blockout_material_path(int p_slot);
	static Ref<Material> resolve_material_path(const String &p_path);
	Ref<ArrayMesh> bake(const Ref<LevelMeshData> &p_data) const;
	PackedVector3Array bake_collision_faces(const Ref<LevelMeshData> &p_data) const;
};
