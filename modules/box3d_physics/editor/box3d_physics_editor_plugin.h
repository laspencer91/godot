/**************************************************************************/
/*  box3d_physics_editor_plugin.h                                         */
/**************************************************************************/

#pragma once

#include "editor/plugins/editor_plugin.h"

class Box3DPhysicsEditorPlugin : public EditorPlugin {
	GDCLASS(Box3DPhysicsEditorPlugin, EditorPlugin);

	ObjectID settings_tab_id;

public:
	String get_plugin_name() const override { return "Box3DPhysics"; }

	Box3DPhysicsEditorPlugin();
	~Box3DPhysicsEditorPlugin();
};
