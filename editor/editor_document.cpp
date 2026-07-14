/**************************************************************************/
/*  editor_document.cpp                                                   */
/**************************************************************************/
/*  Part of the workspace-editor effort (feature/workspace-editor).       */
/*  See editor_document.h, workspace-editor-planning/ARCHITECTURE.md and   */
/*  workspace-editor-planning/G1-multiple-scenes.md.                      */
/**************************************************************************/

#include "editor/editor_document.h"

#include "scene/2d/node_2d.h"
#include "scene/3d/node_3d.h"
#include "scene/gui/control.h"
#include "scene/main/viewport.h"
#include "scene/resources/3d/world_3d.h"
#include "scene/resources/world_2d.h"

EditorDocument::Type EditorDocument::classify_scene_type(Node *p_root) {
	if (!p_root) {
		return TYPE_UNKNOWN;
	}
	if (Object::cast_to<Node3D>(p_root)) {
		return TYPE_SCENE_3D;
	}
	if (Object::cast_to<Node2D>(p_root) || Object::cast_to<Control>(p_root)) {
		return TYPE_SCENE_2D;
	}
	// A plain Node root (or other) could hold either 2D or 3D children; treat as mixed.
	return TYPE_SCENE_MIXED;
}

SceneDocument::SceneDocument() {
	type = TYPE_SCENE_MIXED;

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

SceneDocument::~SceneDocument() {
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

RID SceneDocument::get_scenario() const {
	return world_3d.is_valid() ? world_3d->get_scenario() : RID();
}

RID SceneDocument::get_space() const {
	return world_3d.is_valid() ? world_3d->get_space() : RID();
}

String SceneDocument::get_path() const {
	// M6: derive from the root's scene_file_path (the base `path` is never populated for scenes), so the
	// workspace save records scene tabs by their real path. Empty (base fallback) for an unsaved scene.
	return root ? root->get_scene_file_path() : EditorDocument::get_path();
}

String SceneDocument::get_title() const {
	// G2 styling: the tab title is the scene's filename (e.g. "player.tscn"); "[unsaved]" for a scene
	// with no file yet. Falls back to the base path-derived title if the root isn't set.
	if (root) {
		const String scene_path = root->get_scene_file_path();
		if (!scene_path.is_empty()) {
			return scene_path.get_file();
		}
		return TTRC("[unsaved]");
	}
	return EditorDocument::get_title();
}

String LevelDocument::get_title() const {
	return SceneDocument::get_title() + " [Level]";
}

String ResourceDocument::get_path() const {
	return resource.is_valid() ? resource->get_path() : String();
}

String ResourceDocument::get_title() const {
	if (resource.is_null()) {
		return "Resource";
	}
	if (resource->has_method(SNAME("_get_editor_name"))) {
		const String editor_name = resource->call(SNAME("_get_editor_name"));
		if (!editor_name.is_empty()) {
			return editor_name;
		}
	}
	if (!resource->get_name().is_empty()) {
		return resource->get_name();
	}
	const String resource_path = resource->get_path();
	if (resource_path.is_resource_file()) {
		return resource_path.get_file();
	}
	return resource->get_class();
}

void SceneDocument::activate() {
	if (scene_root) {
		// Only the focused document drives the audio listener in v1.
		scene_root->set_as_audio_listener_2d(true);
	}
}

void SceneDocument::deactivate() {
	if (scene_root) {
		scene_root->set_as_audio_listener_2d(false);
	}
}
