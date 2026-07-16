/**************************************************************************/
/*  editor_document.h                                                     */
/**************************************************************************/
/*  Part of the workspace-editor effort (feature/workspace-editor).       */
/*  The open-document model, split into three by kind of state (see       */
/*  workspace-editor-planning/ARCHITECTURE.md):                           */
/*                                                                        */
/*    EditorDocument      — DOCUMENT STATE: the slim, world-agnostic      */
/*                          identity/model of one open document.          */
/*    SceneDocument       — a scene document: owns the isolated           */
/*                          render/physics world (SubViewport+World3D/2D). */
/*    EditorDocumentView  — VIEW STATE: per-pane presentation of a        */
/*                          document (mirrors Node3DEditorView at the doc  */
/*                          layer). Minted per pane; not the model.        */
/*                                                                        */
/*  This replaces the earlier single EditorDocumentContext, which         */
/*  unconditionally owned a World3D even for script/resource documents    */
/*  and carried per-view state (editor_states/active) on the model.       */
/**************************************************************************/

#pragma once

#include "core/io/resource.h" // Ref<Resource> by-value getter on ScriptDocument.
#include "core/object/object_id.h" // Park-holder handle on ScreenHostDocument.
#include "core/string/ustring.h"
#include "core/templates/hash_map.h"
#include "core/templates/rid.h"
#include "core/variant/array.h"
#include "core/variant/dictionary.h"
#include "editor/editor_data.h" // EditorSelectionHistory (value member) + EditorSelection (fwd).
#include "scene/resources/3d/world_3d.h" // Ref<World3D> by-value getters need the complete type.
#include "scene/resources/material.h" // LevelDocument owns its active material interaction state.
#include "scene/resources/world_2d.h" // Ref<World2D> by-value getters need the complete type.

class Control;
class Node;
class SelectionModel;
class SubViewport;
class WorkspacePane;

enum class DocumentViewKind {
	DEFAULT,
	SCENE_2D,
	SCENE_3D,
	SCRIPT,
	HELP,
}; // G2 D14: view-kind, not main-screen identity.

// DOCUMENT STATE — the model of one open document, independent of how many panes
// (including zero) are looking at it. Deliberately world-agnostic: a script or
// resource document has no render world. World accessors default to "none" and
// are overridden by SceneDocument, so generic "active document" code stays
// cast-free while ownership of a world lives only where a world exists.
// Not an Object: pure C++ infrastructure, consumed only from the editor in C++.
class EditorDocument {
public:
	enum Type {
		TYPE_UNKNOWN,
		TYPE_SCENE_2D,
		TYPE_SCENE_3D,
		TYPE_SCENE_MIXED,
		TYPE_SCRIPT,
		TYPE_RESOURCE,
		TYPE_HELP, // G2 S3: a class-reference (help) document. Append-only — do not reorder.
		TYPE_SCREEN_HOST, // G2 S5.5: THE one screen-host document (seam #5). Append-only.
		TYPE_SHADER, // G-Shader: a Shader / ShaderInclude / VisualShader document. Append-only.
		TYPE_LEVEL, // G-Level LE0: a scene opened in the level-editor workspace. Append-only.
		TYPE_HOTSPOT_ATLAS, // G-Level WP21: HotspotAtlas resource patch-editor tab. Append-only.
	};

protected:
	Type type = TYPE_UNKNOWN;

	// The edited content's root node; its lifetime is owned by EditorData/the
	// scene tree (NOT freed here).
	Node *root = nullptr;

	// Stable per-document undo/redo history id (matches EditedScene::history_id).
	int history_id = 0;
	uint64_t time_opened = 0;
	String path;
	bool dirty = false; // RESERVED: unsaved-changes flag (not yet wired).
	// Document-owned state for contextual editor surfaces (for example the Animation drawer).
	// Editor plugins mirror this through their normal per-scene get_state()/set_state() contract,
	// while DocumentView can consume it even when the plugin state restores before the view exists.
	HashMap<StringName, Dictionary> contextual_editor_states;

public:
	Type get_type() const { return type; }
	void set_type(Type p_type) { type = p_type; }

	// Classify a scene by its root node kind (Node3D -> 3D, Node2D/Control -> 2D, else MIXED),
	// matching how the editor picks a default view. Used to route a DocumentView to the right
	// per-pane editor surface. Null root -> UNKNOWN.
	static Type classify_scene_type(Node *p_root);

	Node *get_root() const { return root; }
	void set_root(Node *p_root) { root = p_root; }

	int get_history_id() const { return history_id; }
	void set_history_id(int p_history_id) { history_id = p_history_id; }

	uint64_t get_time_opened() const { return time_opened; }
	void set_time_opened(uint64_t p_time) { time_opened = p_time; }

	// Virtual so a scene can derive its persist/lookup path from its root's scene_file_path (the base
	// `path` field is never populated for scenes — see SceneDocument). M6 keys tab persistence on this.
	virtual String get_path() const { return path; }
	void set_path(const String &p_path) { path = p_path; }

	// Display label for workspace tabs. Path-derived by default; subclasses with a better
	// source (e.g. HelpDocument's class name) override.
	virtual String get_title() const {
		const String file = path.get_file();
		return file.is_empty() ? "Document" : file;
	}

	// Whether reveal() opens this document as a workspace tab (vs the legacy main-screen
	// switch). Scenes flip to true in M7.1.
	virtual bool opens_as_workspace_tab() const { return false; }

	bool is_dirty() const { return dirty; }
	void set_dirty(bool p_dirty) { dirty = p_dirty; }

	void set_contextual_editor_state(const StringName &p_editor, const Dictionary &p_state) { contextual_editor_states[p_editor] = p_state; }
	Dictionary get_contextual_editor_state(const StringName &p_editor) const {
		const Dictionary *state = contextual_editor_states.getptr(p_editor);
		return state ? *state : Dictionary();
	}

	// World accessors — default to "no world" (a script/resource document has
	// none). SceneDocument overrides these; consumers never cast.
	virtual SubViewport *get_scene_root() const { return nullptr; }
	virtual Ref<World3D> get_world_3d() const { return Ref<World3D>(); }
	virtual Ref<World2D> get_world_2d() const { return Ref<World2D>(); }
	virtual RID get_scenario() const { return RID(); }
	virtual RID get_space() const { return RID(); }

	// Per-document selection + history — same convention as the world accessors: default to "none"
	// (script/resource documents have neither), SceneDocument overrides. Consumers null-coalesce
	// with the global (EditorNode::get_editor_selection[_history]) instead of downcasting.
	virtual EditorSelection *get_selection() const { return nullptr; }
	virtual EditorSelectionHistory *get_selection_history() { return nullptr; }

	// The scene whose editor context should remain active while this document is focused. Scene
	// documents return themselves; embedded resource documents return their owning scene.
	virtual EditorDocument *get_scene_context_document() { return nullptr; }

	EditorDocument() {}
	virtual ~EditorDocument() {}
};

// A scene document: owns the isolated render/physics world its scene renders
// into, so multiple open scenes are live at once without bleeding into each
// other. scene_root is the per-document SubViewport the scene's nodes parent
// under; it carries an explicit World3D so child Node3Ds register into THIS
// document's scenario, and its own World2D isolates 2D.
class SceneDocument : public EditorDocument {
	SubViewport *scene_root = nullptr;
	Ref<World3D> world_3d;
	Ref<World2D> world_2d;

	// This document's own selection + history (G2 D1–D3). The selection is authoritative: the global
	// EditorNode::editor_selection is an EditorActiveSelectionProxy that retargets at the active
	// document's selection, and per-pane docks (D7) bind these directly.
	EditorSelection *selection = nullptr;
	EditorSelectionHistory selection_history;

public:
	virtual SubViewport *get_scene_root() const override { return scene_root; }
	virtual Ref<World3D> get_world_3d() const override { return world_3d; }
	virtual Ref<World2D> get_world_2d() const override { return world_2d; }
	virtual RID get_scenario() const override;
	virtual RID get_space() const override;

	virtual EditorSelection *get_selection() const override { return selection; }
	virtual EditorSelectionHistory *get_selection_history() override { return &selection_history; }
	virtual EditorDocument *get_scene_context_document() override { return this; }

	// G2 M7.1: scenes now open as workspace tabs (pane-hosted DocumentView with its own viewport),
	// not the single legacy main screen. This is the routing flip that unblocks two-scenes-in-two-panes.
	virtual bool opens_as_workspace_tab() const override { return true; }

	// G2 styling: the tab shows the scene's filename (root->get_scene_file_path), not the generic
	// "Document" fallback. Defined out-of-line — needs Node's full type.
	virtual String get_title() const override;

	// M6: the persist/lookup path is the scene's file path (same source as get_title). The base `path`
	// field is never set for scenes, so without this override get_path() returns "" and the scene is
	// dropped from the saved workspace tabs. Empty for an unsaved scene. Out-of-line — needs Node.
	virtual String get_path() const override;

	// Document-level activation side effects (only the focused document drives the
	// audio listener in v1). The per-pane "is this view active" bit lives on
	// EditorDocumentView, not here.
	void activate();
	void deactivate();

	SceneDocument();
	virtual ~SceneDocument();
};

// G-Level LE0: the edited thing is still a scene. Inheriting SceneDocument is the
// document seam: isolated World3D/World2D, per-document selection, undo history,
// scene-root ownership, workspace-tab routing, and scene docks all stay identical.
class LevelDocument : public SceneDocument {
	SelectionModel *selection_model = nullptr;
	// WP22: semantic material interaction state belongs to the Level document,
	// never to the project-wide LevelEditor service. This lets two visible Level
	// documents keep independent material/UV contexts even while one is focused.
	Ref<Material> active_material;
	String active_material_path;
	String active_material_binding_path;
	Dictionary captured_mapping;
	int hotspot_mapping_mode_override = -1;
	// WP21: decision diagnostics from the last committed hotspot fit. This is
	// editor-session/document state only; it is never serialized into the scene.
	Array last_hotspot_fit_diagnostics;

	friend class LevelEditor;

public:
	SelectionModel *get_selection_model() const { return selection_model; }
	void set_last_hotspot_fit_diagnostics(const Array &p_diagnostics) { last_hotspot_fit_diagnostics = p_diagnostics; }
	Array get_last_hotspot_fit_diagnostics() const { return last_hotspot_fit_diagnostics; }
	void clear_last_hotspot_fit_diagnostics() { last_hotspot_fit_diagnostics.clear(); }
	virtual String get_title() const override;

	LevelDocument();
	virtual ~LevelDocument();
};

// A generic Resource edited as a first-class workspace tab. It keeps the exact Ref passed to
// EditorInterface::edit_resource(), so embedded resources are edited live rather than reloaded.
class ResourceDocument : public EditorDocument {
	Ref<Resource> resource;
	EditorDocument *scene_context_document = nullptr; // Not owned; closed before this document.
	EditorSelectionHistory selection_history;

public:
	Ref<Resource> get_resource() const { return resource; }
	void set_resource(const Ref<Resource> &p_resource) { resource = p_resource; }

	void set_scene_context_document(EditorDocument *p_document) { scene_context_document = p_document; }
	virtual EditorDocument *get_scene_context_document() override { return scene_context_document; }
	virtual EditorSelectionHistory *get_selection_history() override { return &selection_history; }

	virtual String get_path() const override;
	virtual String get_title() const override;
	virtual bool opens_as_workspace_tab() const override { return true; }

	ResourceDocument() { type = TYPE_RESOURCE; }
	virtual ~ResourceDocument() {}
};

// G-Level WP21: a HotspotAtlas is still resource document state, but its
// append-only type discriminator lets DocumentView route it through the
// LevelEditor factory instead of the generic Inspector surface. This mirrors
// ShaderDocument while retaining ResourceDocument's exact Ref/path/history.
class HotspotAtlasDocument : public ResourceDocument {
public:
	virtual String get_title() const override { return ResourceDocument::get_title() + " [Hotspot]"; }

	HotspotAtlasDocument() { type = TYPE_HOTSPOT_ATLAS; }
	virtual ~HotspotAtlasDocument() {}
};

// G2 S3: a script (or text) document opened as a workspace tab. No render world — the view
// is a ScriptTextEditor/TextEditor hosted by a DocumentView, and the scripting SERVICES stay on
// the ScriptEditor singleton. `path` == the edited resource's path.
class ScriptDocument : public EditorDocument {
	Ref<Resource> script;

public:
	Ref<Resource> get_script_resource() const { return script; }
	void set_script_resource(const Ref<Resource> &p_script) { script = p_script; }

	virtual bool opens_as_workspace_tab() const override { return true; }

	ScriptDocument() { type = TYPE_SCRIPT; }
	virtual ~ScriptDocument() {}
};

// G-Shader: a shader document opened as a workspace tab. Holds a Shader / ShaderInclude / VisualShader
// resource (kept as Ref<Resource> so this header stays light — the ShaderEditorPlugin view factory
// casts to the concrete type and its language plugins pick the widget: a code editor for text shaders,
// a node-graph editor for visual shaders). No render world; `path` == the edited resource's path.
class ShaderDocument : public EditorDocument {
	Ref<Resource> shader;

public:
	Ref<Resource> get_shader_resource() const { return shader; }
	void set_shader_resource(const Ref<Resource> &p_shader) { shader = p_shader; }

	virtual bool opens_as_workspace_tab() const override { return true; }

	ShaderDocument() { type = TYPE_SHADER; }
	virtual ~ShaderDocument() {}
};

// G2 S3: a class-reference (help) document opened as a workspace tab. The view is an EditorHelp.
// `path` == "help://" + class_name (a stable, unique key for lookup/dedup).
class HelpDocument : public EditorDocument {
	String class_name;

public:
	String get_class_name() const { return class_name; }
	void set_class_name(const String &p_class) {
		class_name = p_class;
		path = "help://" + p_class;
	}

	virtual String get_title() const override { return class_name; }
	virtual bool opens_as_workspace_tab() const override { return true; }

	HelpDocument() { type = TYPE_HELP; }
	virtual ~HelpDocument() {}
};

// G2 S5.5: THE one screen-host document (seam #5). Its DocumentView hosts the legacy
// main-screen stack (EditorMainScreen's main_screen_vbox) itself, so the singleton screens
// (2D/3D/Script/Game/AssetLib + third-party main screens) live inside one workspace tab and
// the root pane can be a tab host from startup — the first script/scene reveal then opens as
// a sibling tab in the SAME pane, never a split. The stack is NOT owned here: when the view
// dies, it parks back under the hidden holder identified by park_holder_id, so
// EditorMainScreen::get_control() keeps returning the live vbox at every instant (D11).
class ScreenHostDocument : public EditorDocument {
	Control *screen_stack = nullptr; // main_screen_vbox (not owned).
	ObjectID park_holder_id; // Hidden holder the stack returns to when no view hosts it.

public:
	Control *get_screen_stack() const { return screen_stack; }
	void set_screen_stack(Control *p_stack) { screen_stack = p_stack; }

	ObjectID get_park_holder_id() const { return park_holder_id; }
	void set_park_holder_id(ObjectID p_id) { park_holder_id = p_id; }

	virtual bool opens_as_workspace_tab() const override { return true; }

	ScreenHostDocument() {
		type = TYPE_SCREEN_HOST;
		path = "screens://Editor"; // Stable key; get_file() doubles as the v1 tab title.
	}
	virtual ~ScreenHostDocument() {}
};

// VIEW STATE — one per pane that presents a document. Holds the presentation
// state that must differ between two panes showing the same document (camera,
// pan/zoom, 2D/3D toggle, gizmo mode) plus which document/pane it binds. Mirrors
// Node3DEditorView at the document layer. Reserved scaffolding: minted per pane
// once the workspace hosts real per-pane documents (Step ④ wiring); not the model.
class EditorDocumentView {
	EditorDocument *document = nullptr; // Presented document (not owned).
	WorkspacePane *pane = nullptr; // Hosting pane (not owned).
	Dictionary editor_states; // 3D camera, 2D pan/zoom, 2D/3D toggle, gizmo mode.
	bool active = false; // Is this the active view (drives which doc is "current").

public:
	EditorDocument *get_document() const { return document; }
	void set_document(EditorDocument *p_document) { document = p_document; }

	WorkspacePane *get_pane() const { return pane; }
	void set_pane(WorkspacePane *p_pane) { pane = p_pane; }

	Dictionary &get_editor_states() { return editor_states; }
	void set_editor_states(const Dictionary &p_states) { editor_states = p_states; }

	bool is_active() const { return active; }
	void set_active(bool p_active) { active = p_active; }
};
