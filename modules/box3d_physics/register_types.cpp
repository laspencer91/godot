/**************************************************************************/
/*  register_types.cpp                                                    */
/**************************************************************************/

#include "register_types.h"

#include "box3d_physics_server_3d.h"

#include "core/config/project_settings.h"
#include "core/object/callable_mp.h"
#include "servers/physics_3d/physics_server_3d_wrap_mt.h"

static PhysicsServer3D *create_box3d_physics_server() {
#ifdef THREADS_ENABLED
	bool run_on_separate_thread = GLOBAL_GET("physics/3d/run_on_separate_thread");
#else
	bool run_on_separate_thread = false;
#endif
	Box3DPhysicsServer3D *server = memnew(Box3DPhysicsServer3D);
	return memnew(PhysicsServer3DWrapMT(server, run_on_separate_thread));
}

void initialize_box3d_physics_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SERVERS) {
		return;
	}
	PhysicsServer3DManager::get_singleton()->register_server("Box3D", callable_mp_static(&create_box3d_physics_server));
}

void uninitialize_box3d_physics_module(ModuleInitializationLevel p_level) {
}
