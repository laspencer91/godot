/**************************************************************************/
/*  register_types.cpp                                                    */
/**************************************************************************/

#include "register_types.h"

#include "horde_flow_field.h"
#include "horde_nav_grid.h"

#include "core/object/class_db.h"

void initialize_horde_sim_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}
	GDREGISTER_CLASS(HordeNavGrid);
	GDREGISTER_CLASS(HordeFlowField);
}

void uninitialize_horde_sim_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}
}
