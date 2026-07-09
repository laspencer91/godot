/**************************************************************************/
/*  box3d_ragdoll_editor_plugin.h                                         */
/**************************************************************************/

#pragma once

#include "editor/plugins/editor_plugin.h"
#include "editor/scene/3d/node_3d_editor_gizmos.h"

class Box3DRagdollGizmoPlugin : public EditorNode3DGizmoPlugin {
	GDCLASS(Box3DRagdollGizmoPlugin, EditorNode3DGizmoPlugin);

public:
	bool has_gizmo(Node3D *p_spatial) override;
	String get_gizmo_name() const override;
	int get_priority() const override;
	void redraw(EditorNode3DGizmo *p_gizmo) override;

	Box3DRagdollGizmoPlugin();
};

class Box3DRagdollEditorPlugin : public EditorPlugin {
	GDCLASS(Box3DRagdollEditorPlugin, EditorPlugin);

	Ref<Box3DRagdollGizmoPlugin> gizmo_plugin;

public:
	String get_plugin_name() const override { return "Box3DRagdoll"; }

	Box3DRagdollEditorPlugin();
};
