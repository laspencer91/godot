/**************************************************************************/
/*  register_types.cpp                                                    */
/**************************************************************************/

#include "register_types.h"

#include "horde_agents.h"
#include "horde_flow_field.h"
#include "horde_fsm_config.h"
#include "horde_nav_grid.h"

#include "core/object/class_db.h"

void initialize_horde_sim_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}
	GDREGISTER_CLASS(HordeNavGrid);
	GDREGISTER_CLASS(HordeFlowField);
	GDREGISTER_CLASS(HordeFSMConfig);
	GDREGISTER_CLASS(HordeAgents);
}

void uninitialize_horde_sim_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}
}
