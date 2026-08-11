/**************************************************************************/
/*  register_types.cpp                                                    */
/**************************************************************************/

#include "register_types.h"

#include "box3d_character_mover.h"
#include "box3d_direct_space_state_3d.h"
#include "box3d_physics_server_3d.h"
#include "box3d_ragdoll.h"
#include "box3d_surface_materials.h"

#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "servers/physics_3d/physics_server_3d_wrap_mt.h"

#ifdef TOOLS_ENABLED
#include "editor/box3d_physics_editor_plugin.h"
#include "editor/box3d_ragdoll_editor_plugin.h"
#endif

static Box3DPhysics *box3d_physics_singleton = nullptr;

static PhysicsServer3D *create_box3d_physics_server() {
#ifdef THREADS_ENABLED
	bool run_on_separate_thread = GLOBAL_GET("physics/3d/run_on_separate_thread");
#else
	bool run_on_separate_thread = false;
#endif
	Box3DPhysicsServer3D *server = memnew(Box3DPhysicsServer3D(run_on_separate_thread));
	return memnew(PhysicsServer3DWrapMT(server, run_on_separate_thread));
}

void initialize_box3d_physics_module(ModuleInitializationLevel p_level) {
#ifdef TOOLS_ENABLED
	if (p_level == MODULE_INITIALIZATION_LEVEL_EDITOR) {
		EditorPlugins::add_by_type<Box3DRagdollEditorPlugin>();
		EditorPlugins::add_by_type<Box3DPhysicsEditorPlugin>();
		return;
	}
#endif
	if (p_level == MODULE_INITIALIZATION_LEVEL_SCENE) {
		GDREGISTER_CLASS(Box3DRagdollProfile);
		GDREGISTER_CLASS(Box3DRagdollProfileGenerator);
		GDREGISTER_CLASS(Box3DRagdoll);
		GDREGISTER_CLASS(Box3DSurfaceOverride3D);
		return;
	}

	if (p_level != MODULE_INITIALIZATION_LEVEL_SERVERS) {
		return;
	}
	GDREGISTER_CLASS(Box3DCharacterMover);
	GDREGISTER_ABSTRACT_CLASS(Box3DDirectSpaceState3D);
	GDREGISTER_CLASS(Box3DSurfaceMaterial);
	GDREGISTER_CLASS(Box3DSurfaceMaterialLibrary);
	GDREGISTER_CLASS(Box3DSurfaceMap);
	GDREGISTER_CLASS(Box3DPhysics);

	Box3DPhysics::register_project_settings();
	box3d_physics_singleton = memnew(Box3DPhysics);
	Engine::get_singleton()->add_singleton(Engine::Singleton("Box3DPhysics", box3d_physics_singleton));

	PhysicsServer3DManager::get_singleton()->register_server("Box3D", callable_mp_static(&create_box3d_physics_server));
}

void uninitialize_box3d_physics_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SERVERS) {
		return;
	}
	if (box3d_physics_singleton) {
		memdelete(box3d_physics_singleton);
		box3d_physics_singleton = nullptr;
	}
}
