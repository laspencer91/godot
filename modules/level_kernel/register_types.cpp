/**************************************************************************/
/*  register_types.cpp                                                    */
/**************************************************************************/

#include "register_types.h"

#include "level_block.h"
#include "level_mesh.h"
#include "level_mesh_adjacency.h"
#include "level_mesh_baker.h"
#include "level_mesh_data.h"
#include "level_mesh_diff.h"
#include "level_mesh_element_bvh.h"

#include "core/object/class_db.h"

void initialize_level_kernel_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}

	GDREGISTER_CLASS(LevelMeshData);
	GDREGISTER_CLASS(LevelMeshDiff);
	GDREGISTER_CLASS(LevelMeshAdjacency);
	GDREGISTER_CLASS(LevelMeshElementBVH);
	GDREGISTER_CLASS(LevelMesh);
	GDREGISTER_CLASS(LevelMeshBaker);
	GDREGISTER_CLASS(LevelBlock);
}

void uninitialize_level_kernel_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}
}
