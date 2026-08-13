/**************************************************************************/
/*  register_types.cpp                                                    */
/**************************************************************************/

#include "register_types.h"

#include "drag_out_spike.h"
#include "file_system_watcher.h"
#include "progress_dialog_spike.h"

#include "core/object/class_db.h"

void initialize_material_library_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}
	GDREGISTER_CLASS(FileSystemWatcher);
	GDREGISTER_CLASS(DragOutSpike);
	GDREGISTER_CLASS(ProgressDialogSpike);
}

void uninitialize_material_library_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}
}
