/**************************************************************************/
/*  box3d_ragdoll_editor_plugin.h                                         */
/**************************************************************************/

#pragma once

#include "../box3d_ragdoll.h"

#include "editor/plugins/editor_plugin.h"
#include "editor/scene/3d/node_3d_editor_gizmos.h"

class Box3DRagdollProfileGenerationDialog;

class Box3DRagdollGizmoPlugin : public EditorNode3DGizmoPlugin {
	GDCLASS(Box3DRagdollGizmoPlugin, EditorNode3DGizmoPlugin);

	Ref<Box3DRagdollProfileGenerator> line_generator;
	LocalVector<String> chain_material_names;

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
	Box3DRagdollProfileGenerationDialog *profile_generation_dialog = nullptr;

public:
	String get_plugin_name() const override { return "Box3DRagdoll"; }
	void popup_profile_generator(Box3DRagdoll *p_ragdoll);

	Box3DRagdollEditorPlugin();
};
