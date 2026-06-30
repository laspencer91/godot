/**************************************************************************/
/*  editor_document_context.h                                             */
/**************************************************************************/
/*  Part of the workspace-editor effort (feature/workspace-editor).       */
/*  G1: one live, independently-rendered document per open scene/script/  */
/*  resource. Each EditorDocumentContext owns an isolated render/physics  */
/*  world (own World3D scenario + space, own World2D) plus its own        */
/*  selection and selection history, so multiple scenes can be live at    */
/*  once without bleeding into each other. See                            */
/*  workspace-editor-planning/G1-multiple-scenes.md.                      */
/**************************************************************************/

#ifndef EDITOR_DOCUMENT_CONTEXT_H
#define EDITOR_DOCUMENT_CONTEXT_H

#include "core/string/ustring.h"
#include "core/templates/rid.h"
#include "core/variant/dictionary.h"
#include "editor/editor_data.h" // EditorSelectionHistory (value member) + EditorSelection (fwd).

class Node;
class SubViewport;
class World2D;
class World3D;

// Owns everything that, in stock Godot, is a single global on EditorNode:
// the scene-root SubViewport, the render/physics world, the selection and the
// selection history. EditorData holds one of these per open document; the
// EditorNode singletons (scene_root / editor_selection / editor_history) become
// delegating accessors onto the *active* document. Not an Object: it is pure
// C++ infrastructure, consumed only from the editor in C++.
class EditorDocumentContext {
public:
	enum Type {
		TYPE_UNKNOWN,
		TYPE_SCENE_2D,
		TYPE_SCENE_3D,
		TYPE_SCENE_MIXED,
		TYPE_SCRIPT,
		TYPE_RESOURCE,
	};

private:
	Type type = TYPE_UNKNOWN;

	// Render/physics isolation. scene_root is the per-document SubViewport that
	// the edited scene's nodes parent under; it carries an explicit World3D so
	// child Node3Ds register their instances into *this* document's scenario,
	// and its own World2D isolates 2D. 3D is rendered by a Node3DEditorViewport
	// bound to world_3d (v1), so scene_root keeps disable_3d.
	SubViewport *scene_root = nullptr;
	Ref<World3D> world_3d;
	Ref<World2D> world_2d;

	// Edited content. root is the scene's root node; its lifetime is owned by
	// EditorData/EditorNode (the scene tree), NOT freed here.
	Node *root = nullptr;

	// Per-document selection state (mirrors EditorNode::editor_selection /
	// editor_history, which become active-document delegates).
	EditorSelection *selection = nullptr;
	EditorSelectionHistory selection_history;

	// Stable per-document undo/redo history id (matches EditedScene::history_id).
	int history_id = 0;

	String path;
	Dictionary editor_states;
	uint64_t time_opened = 0;
	bool active = false;

public:
	// --- Render / physics world ------------------------------------------
	SubViewport *get_scene_root() const { return scene_root; }
	Ref<World3D> get_world_3d() const { return world_3d; }
	Ref<World2D> get_world_2d() const { return world_2d; }
	RID get_scenario() const;
	RID get_space() const;

	// --- Selection -------------------------------------------------------
	EditorSelection *get_selection() const { return selection; }
	EditorSelectionHistory *get_selection_history() { return &selection_history; }

	// --- Edited content / metadata ---------------------------------------
	Node *get_root() const { return root; }
	void set_root(Node *p_root) { root = p_root; }

	Type get_type() const { return type; }
	void set_type(Type p_type) { type = p_type; }

	int get_history_id() const { return history_id; }
	void set_history_id(int p_history_id) { history_id = p_history_id; }

	String get_path() const { return path; }
	void set_path(const String &p_path) { path = p_path; }

	Dictionary &get_editor_states() { return editor_states; }
	void set_editor_states(const Dictionary &p_states) { editor_states = p_states; }

	uint64_t get_time_opened() const { return time_opened; }
	void set_time_opened(uint64_t p_time) { time_opened = p_time; }

	// --- Activation (v1: no node reparenting; visibility + listener) ------
	bool is_active() const { return active; }
	void activate();
	void deactivate();

	EditorDocumentContext();
	~EditorDocumentContext();
};

#endif // EDITOR_DOCUMENT_CONTEXT_H
