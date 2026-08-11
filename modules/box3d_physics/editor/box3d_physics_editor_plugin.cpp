/**************************************************************************/
/*  box3d_physics_editor_plugin.cpp                                       */
/**************************************************************************/

#include "box3d_physics_editor_plugin.h"

#include "box3d_physics_settings_tab.h"
#include "box3d_surface_material_inspector_plugin.h"

#include "editor/settings/project_settings_editor.h"

Box3DPhysicsEditorPlugin::Box3DPhysicsEditorPlugin() {
	Box3DPhysicsSettingsTab *settings_tab = memnew(Box3DPhysicsSettingsTab);
	settings_tab_id = settings_tab->get_instance_id();
	add_control_to_container(CONTAINER_PROJECT_SETTING_TAB_RIGHT, settings_tab);

	Ref<Box3DSurfaceMaterialInspectorPlugin> material_inspector_plugin;
	material_inspector_plugin.instantiate();
	add_inspector_plugin(material_inspector_plugin);
}

Box3DPhysicsEditorPlugin::~Box3DPhysicsEditorPlugin() {
	// The tab lives under the Project Settings dialog, so on editor teardown it can
	// already be gone by the time the plugin is destroyed.
	Box3DPhysicsSettingsTab *tab = Object::cast_to<Box3DPhysicsSettingsTab>(ObjectDB::get_instance(settings_tab_id));
	if (tab && ProjectSettingsEditor::get_singleton()) {
		remove_control_from_container(CONTAINER_PROJECT_SETTING_TAB_RIGHT, tab);
		memdelete(tab);
	}
}
