/**************************************************************************/
/*  editor_document_context.cpp                                           */
/**************************************************************************/
/*  Part of the workspace-editor effort (feature/workspace-editor).       */
/*  See editor_document_context.h and                                     */
/*  workspace-editor-planning/G1-multiple-scenes.md.                      */
/**************************************************************************/

#include "editor/editor_document_context.h"

#include "scene/main/viewport.h"
#include "scene/resources/3d/world_3d.h"
#include "scene/resources/world_2d.h"

EditorDocumentContext::EditorDocumentContext() {
	// Mirror EditorNode's stock scene_root configuration (editor_node.cpp
	// ~8820), but give this document its OWN render/physics world so it is
	// isolated from every other live document. disable_3d stays true: the
	// scene's Node3Ds still register into world_3d's scenario (ENTER_WORLD is
	// gated on world validity, not on disable_3d), and a Node3DEditorViewport
	// bound to world_3d performs the actual 3D rendering for the focused pane.
	scene_root = memnew(SubViewport);
	scene_root->set_auto_translate_mode(Node::AUTO_TRANSLATE_MODE_ALWAYS);
	scene_root->set_translation_domain(StringName());
	scene_root->set_embedding_subwindows(true);
	scene_root->set_disable_3d(true);
	scene_root->set_disable_input(true);
	scene_root->set_as_audio_listener_2d(true);

	// Explicit World3D handle so consumers (Node3DEditorViewport, gizmo/grid/
	// origin instancing, physics picking) can be pointed at this document's
	// scenario/space. World2D is per-SubViewport already, so 2D is isolated for
	// free; we just keep a handle to it.
	world_3d.instantiate();
	scene_root->set_world_3d(world_3d);
	world_2d = scene_root->get_world_2d();

	selection = memnew(EditorSelection);
}

EditorDocumentContext::~EditorDocumentContext() {
	if (selection) {
		memdelete(selection);
		selection = nullptr;
	}
	if (scene_root) {
		// The edited scene (root) is owned by EditorData/the scene tree and is
		// detached elsewhere; here we only tear down the SubViewport we created.
		if (scene_root->get_parent()) {
			scene_root->get_parent()->remove_child(scene_root);
		}
		memdelete(scene_root);
		scene_root = nullptr;
	}
	// world_3d / world_2d are released via Ref<> refcounting.
}

RID EditorDocumentContext::get_scenario() const {
	return world_3d.is_valid() ? world_3d->get_scenario() : RID();
}

RID EditorDocumentContext::get_space() const {
	return world_3d.is_valid() ? world_3d->get_space() : RID();
}

void EditorDocumentContext::activate() {
	active = true;
	if (scene_root) {
		// Only the focused document drives the audio listener in v1.
		scene_root->set_as_audio_listener_2d(true);
	}
}

void EditorDocumentContext::deactivate() {
	active = false;
	if (scene_root) {
		scene_root->set_as_audio_listener_2d(false);
	}
}
