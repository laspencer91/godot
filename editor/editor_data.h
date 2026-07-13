/**************************************************************************/
/*  editor_data.h                                                         */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#pragma once

#include "core/templates/list.h"
#include "scene/main/node.h"
#include "scene/resources/texture.h"

class ConfigFile;
class EditorPlugin;
class EditorUndoRedoManager;
class PopupMenu;

/**
 * Stores the history of objects which have been selected for editing in the Editor & the Inspector.
 *
 * Used in the editor to set & access the currently edited object, as well as the history of objects which have been edited.
 */
class EditorSelectionHistory {
	// Stores the object & property (if relevant).
	struct _Object {
		Ref<RefCounted> ref;
		ObjectID object;
		String property;
		bool inspector_only = false;
	};

	// Represents the selection of an object for editing.
	struct HistoryElement {
		// The sub-resources of the parent object (first in the path) that have been edited.
		// For example, Node2D -> nested resource -> nested resource, if edited each individually in their own inspector.
		Vector<_Object> path;
		// The current point in the path. This is always equal to the last item in the path - it is never decremented.
		int level = 0;
	};
	friend class EditorData;

	Vector<HistoryElement> history;
	int current_elem_idx; // The current history element being edited.

public:
	void cleanup_history();

	bool is_at_beginning() const;
	bool is_at_end() const;

	// Adds an object to the selection history. A property name can be passed if the target is a subresource of the given object.
	// If the object should not change the main screen plugin, it can be set as inspector only.
	void add_object(ObjectID p_object, const String &p_property = String(), bool p_inspector_only = false);
	void replace_object(ObjectID p_old_object, ObjectID p_new_object);

	int get_history_len();
	int get_history_pos();

	// Gets an object from the history. The most recent object would be the object with p_obj = get_history_len() - 1.
	ObjectID get_history_obj(int p_obj) const;

	bool next();
	bool previous();
	ObjectID get_current();
	bool is_current_inspector_only() const;

	// Gets the size of the path of the current history item.
	int get_path_size() const;
	// Gets the object of the current history item, if valid.
	ObjectID get_path_object(int p_index) const;
	// Gets the property of the current history item.
	String get_path_property(int p_index) const;

	void clear();

	EditorSelectionHistory();
};

class EditorSelection;
class EditorDocument;
class ResourceDocument;
class ScriptDocument;
class HelpDocument;
class ShaderDocument;

class EditorData {
public:
	struct CustomType {
		String name;
		Ref<Script> script;
		Ref<Texture2D> icon;
	};

	struct EditedScene {
		Node *root = nullptr;
		String path;
		uint64_t file_modified_time = 0;
		Dictionary editor_states;
		List<Node *> selection;
		Vector<EditorSelectionHistory::HistoryElement> history_stored;
		int history_current = 0;
		Dictionary custom_state;
		NodePath live_edit_root;
		int history_id = 0;
		uint64_t last_checked_version = 0;
		uint64_t time_opened = 0;

		// Per-document container (own render/physics world + scene_root SubViewport).
		// Owned raw pointer: EditedScene has no destructor, so shallow copies of the
		// struct (get_edited_scenes, Vector reallocation) never free it; the single
		// canonical owner in `edited_scene` is freed in remove_scene/clear_edited_scenes,
		// mirroring how `root` is managed. See editor/editor_document.h. Holds a
		// SceneDocument in v1 (every open document is a scene); typed as the base
		// EditorDocument so script/resource documents can slot in later.
		EditorDocument *document = nullptr;
	};

private:
	Vector<EditorPlugin *> editor_plugins;
	HashMap<StringName, EditorPlugin *> extension_editor_plugins;

	struct PropertyData {
		String name;
		Variant value;
	};
	HashMap<String, Vector<CustomType>> custom_types;

	List<PropertyData> clipboard;
	EditorUndoRedoManager *undo_redo_manager;
	Vector<Callable> undo_redo_callbacks;
	HashMap<StringName, Callable> move_element_functions;

	Vector<EditedScene> edited_scene;
	int current_edited_scene = -1;

	// G2 S3: auxiliary (non-scene) documents — scripts and help pages opened as workspace tabs.
	// Scene documents live on EditedScene::document; these have no edited-scene slot. Owned here
	// (memnew'd by get_or_create_*), freed by close_aux_document and in the EditorData teardown.
	Vector<EditorDocument *> aux_documents;
	int last_created_scene = 1;

	bool _find_updated_instances(Node *p_root, Node *p_node, HashSet<String> &checked_paths);

	HashMap<StringName, String> _script_class_icon_paths;
	HashMap<String, StringName> _script_class_file_to_path;
	HashMap<String, Ref<Texture2D>> _script_icon_cache;

	Ref<Texture2D> _load_script_icon(const String &p_path) const;

public:
	EditorPlugin *get_handling_main_editor(Object *p_object);
	Vector<EditorPlugin *> get_handling_sub_editors(Object *p_object);
	EditorPlugin *get_editor_by_name(const String &p_name);

	void copy_object_params(Object *p_object);
	void paste_object_params(Object *p_object);

	Dictionary get_editor_plugin_states() const;
	Dictionary get_scene_editor_states(int p_idx) const;
	Dictionary get_scene_editor_states_with_selection(int p_idx) const;
	void set_editor_plugin_states(const Dictionary &p_states);
	void get_editor_breakpoints(List<String> *p_breakpoints);
	void clear_editor_states();
	void save_editor_external_data();
	void apply_changes_in_editors();

	void add_editor_plugin(EditorPlugin *p_plugin);
	void remove_editor_plugin(EditorPlugin *p_plugin);

	int get_editor_plugin_count() const;
	EditorPlugin *get_editor_plugin(int p_idx);

	void add_extension_editor_plugin(const StringName &p_class_name, EditorPlugin *p_plugin);
	void remove_extension_editor_plugin(const StringName &p_class_name);
	bool has_extension_editor_plugin(const StringName &p_class_name);
	EditorPlugin *get_extension_editor_plugin(const StringName &p_class_name);

	void add_undo_redo_inspector_hook_callback(Callable p_callable); // Callbacks should have this signature: void (Object* undo_redo, Object *modified_object, String property, Variant new_value)
	void remove_undo_redo_inspector_hook_callback(Callable p_callable);
	const Vector<Callable> get_undo_redo_inspector_hook_callback();

	void add_move_array_element_function(const StringName &p_class, Callable p_callable); // Function should have this signature: void (Object* undo_redo, Object *modified_object, String array_prefix, int element_index, int new_position)
	void remove_move_array_element_function(const StringName &p_class);
	Callable get_move_array_element_function(const StringName &p_class) const;

	void add_custom_type(const String &p_type, const String &p_inherits, const Ref<Script> &p_script, const Ref<Texture2D> &p_icon);
	Variant instantiate_custom_type(const String &p_type, const String &p_inherits);
	void remove_custom_type(const String &p_type);
	const HashMap<String, Vector<CustomType>> &get_custom_types() const { return custom_types; }
	const CustomType *get_custom_type_by_name(const String &p_name) const;
	const CustomType *get_custom_type_by_path(const String &p_path) const;
	bool is_type_recognized(const String &p_type) const;

	void instantiate_object_properties(Object *p_object);

	int add_edited_scene(int p_at_pos);
	void remove_scene(int p_idx);
	void set_scene_root(int p_idx, Node *p_root);
	void set_edited_scene(int p_idx);
	void set_edited_scene_root(Node *p_root);
	int get_edited_scene() const;
	int get_edited_scene_from_path(const String &p_path) const;
	Node *get_edited_scene_root(int p_idx = -1);
	int get_edited_scene_count() const;
	Vector<EditedScene> get_edited_scenes() const;

	// Per-document accessors (own render/physics world per open scene). Returns the
	// base EditorDocument; in v1 every document is a SceneDocument.
	EditorDocument *get_document(int p_idx = -1) const;
	EditorDocument *get_active_document() const;
	int find_document_index(const EditorDocument *p_document) const;

	// G2 S3: auxiliary (non-scene) document registry — script and help documents opened as
	// workspace tabs. get_or_create_* dedups by resource/class (one document per open script/class);
	// find_aux_document_by_path resolves a stored path (resource path, or "help://<class>").
	ScriptDocument *get_or_create_script_document(const Ref<Resource> &p_resource);
	ShaderDocument *get_or_create_shader_document(const Ref<Resource> &p_resource);
	ResourceDocument *get_or_create_resource_document(const Ref<Resource> &p_resource, EditorDocument *p_scene_context = nullptr);
	HelpDocument *get_or_create_help_document(const String &p_class);
	EditorDocument *find_aux_document_by_path(const String &p_path) const;

	// G2 M6.2: map a saved workspace tab path back to a live document (session restore) — an already-open
	// scene (by path), a help document (help://Class), or a script/text resource (existing aux doc, else
	// loaded). Null for an unresolvable/missing path. (The screen-host doc is not in EditorData; its
	// owner resolves that one.)
	EditorDocument *get_or_create_document_for_path(const String &p_path);
	void close_aux_document(EditorDocument *p_document);

	String get_scene_title(int p_idx, bool p_always_strip_extension = false) const;
	String get_scene_path(int p_idx) const;
	String get_scene_type(int p_idx) const;
	void set_scene_path(int p_idx, const String &p_path);
	Ref<Script> get_scene_root_script(int p_idx) const;
	uint64_t get_scene_time_opened(int p_idx) const;
	void set_scene_modified_time(int p_idx, uint64_t p_time);
	uint64_t get_scene_modified_time(int p_idx) const;
	void clear_edited_scenes();
	void set_edited_scene_live_edit_root(const NodePath &p_root);
	NodePath get_edited_scene_live_edit_root();
	bool check_and_update_scene(int p_idx);
	bool reload_scene_from_memory(int p_idx, bool p_mark_unsaved);
	void move_scene_to_index(int p_idx, int p_to_idx);
	void move_edited_scene_to_index(int p_idx);

	bool call_build();

	void set_scene_as_saved(int p_idx);
	bool is_scene_changed(int p_idx);

	int get_scene_history_id_from_path(const String &p_path) const;
	int get_current_edited_scene_history_id() const;
	int get_scene_history_id(int p_idx) const;

	void set_plugin_window_layout(Ref<ConfigFile> p_layout);
	void get_plugin_window_layout(Ref<ConfigFile> p_layout);

	void save_edited_scene_state(EditorSelection *p_selection, EditorSelectionHistory *p_history, const Dictionary &p_custom);
	Dictionary restore_edited_scene_state(EditorSelection *p_selection, EditorSelectionHistory *p_history);
	void notify_edited_scene_changed();
	void notify_resource_saved(const Ref<Resource> &p_resource);
	void notify_scene_saved(const String &p_path);
	void load_editor_plugin_states_from_config(const Ref<ConfigFile> &p_config_file, int p_idx);

	bool script_class_is_parent(const String &p_class, const String &p_inherits);
	Variant script_class_instance(const String &p_class);

	Ref<Script> script_class_load_script(const String &p_class) const;

	StringName script_class_get_name(const String &p_path) const;
	void script_class_set_name(const String &p_path, const StringName &p_class);

	String script_class_get_icon_path(const String &p_class, bool *r_valid = nullptr) const;
	void script_class_set_icon_path(const String &p_class, const String &p_icon_path);
	void script_class_clear_icon_paths() { _script_class_icon_paths.clear(); }
	void script_class_save_global_classes();
	void script_class_load_icon_paths();

	Ref<Texture2D> extension_class_get_icon(const String &p_class) const;

	Ref<Texture2D> get_script_icon(const String &p_script_path);
	void clear_script_icon_cache();

	EditorData();
	~EditorData();
};

/**
 * Stores and provides access to the nodes currently selected in the editor.
 *
 * This provides a central location for storing "selected" nodes, as a selection can be triggered from multiple places,
 * such as the SceneTreeDock or a main screen editor plugin (e.g. CanvasItemEditor).
 */
class EditorSelection : public Object {
	GDCLASS(EditorSelection, Object);

	// Contains the selected nodes and corresponding metadata.
	// Metadata objects come from calling _get_editor_data on the editor_plugins, passing the selected node.
	HashMap<ObjectID, Object *> selection;

	// Tracks whether the selection change signal has been emitted.
	// Prevents multiple signals being called in one frame.
	bool emitted = false;

	bool changed = false;
	bool node_list_changed = false;

	void _node_removed(Node *p_node);

	// Editor plugins which are related to selection.
	List<Object *> editor_plugins;
	LocalVector<ObjectID> top_selected_node_list;

	void _update_node_list();
	void _emit_change();

protected:
	static void _bind_methods();

public:
	// G2 D2: the consumer-facing method set is virtual so EditorActiveSelectionProxy can retarget
	// each call at the active document's selection (Model B). Everything else stays non-virtual.
	virtual void add_node(Node *p_node);
	virtual void remove_node(Node *p_node);
	virtual bool is_selected(Node *p_node) const;

	template <typename T>
	T *get_node_editor_data(Node *p_node) {
		return Object::cast_to<T>(get_node_meta(p_node));
	}
	// G2 D2: backing accessor for get_node_editor_data<T> (the template can't be virtual). The proxy
	// overrides it to reach the active target's per-node metadata.
	virtual Object *get_node_meta(Node *p_node) const;

	// Adds an editor plugin which can provide metadata for selected nodes.
	virtual void add_editor_plugin(Object *p_object);
	// G2 D2: whether any metadata provider has been registered — lets the proxy seed a target once.
	bool has_editor_plugins() const { return !editor_plugins.is_empty(); }

	virtual void update(bool p_deferred = true);
	virtual void clear();

	// Returns only the top level selected nodes.
	// That is, if the selection includes some node and a child of that node, only the parent is returned.
	virtual List<Node *> get_top_selected_node_list();
	// Same as get_top_selected_node_list but returns a copy in a TypedArray for binding to scripts.
	virtual TypedArray<Node> get_top_selected_nodes();
	// Returns all the selected nodes (list version of "get_selected_nodes").
	virtual List<Node *> get_full_selected_node_list();
	// Same as get_full_selected_node_list but returns a copy in a TypedArray for binding to scripts.
	virtual TypedArray<Node> get_selected_nodes();
	// Returns the map of selected objects and their metadata.
	virtual HashMap<ObjectID, Object *> &get_selection() { return selection; }

	virtual ~EditorSelection();
};

// G2 D2 (Model B): the stable EditorSelection object that all ~48 global consumers and the cached
// dock/editor pointers hold. It owns no selection state of its own while a scene document is active;
// instead it retargets every read/write at that document's own EditorSelection and relays its
// selection_changed as this object's own, so per-document selections are authoritative while the
// global object identity (and its signal) never changes. Falls back to inherited storage when no
// scene document is active (empty project / script-only session). Not a GDCLASS: it deliberately
// reports as EditorSelection so consumers and the inherited "selection_changed" signal are unaware.
class EditorActiveSelectionProxy : public EditorSelection {
	EditorSelection *target = nullptr;
	// Metadata providers (2D/3D/control editors) registered on the global selection at startup, kept
	// so each document's selection can be seeded on first activation (add_node populates per-doc meta).
	List<Object *> plugin_providers;

	void _relay_selection_changed();
	void _seed_plugins(EditorSelection *p_target);

public:
	// Point the proxy at p_target (nullptr => inherited fallback storage). Silent: consumers are
	// refreshed by the existing scene-switch machinery; the relay carries subsequent live changes.
	void set_target(EditorSelection *p_target);
	EditorSelection *get_target() const { return target; }

	virtual void add_node(Node *p_node) override;
	virtual void remove_node(Node *p_node) override;
	virtual bool is_selected(Node *p_node) const override;
	virtual Object *get_node_meta(Node *p_node) const override;
	virtual void add_editor_plugin(Object *p_object) override;
	virtual void update(bool p_deferred = true) override;
	virtual void clear() override;
	virtual List<Node *> get_top_selected_node_list() override;
	virtual TypedArray<Node> get_top_selected_nodes() override;
	virtual List<Node *> get_full_selected_node_list() override;
	virtual TypedArray<Node> get_selected_nodes() override;
	virtual HashMap<ObjectID, Object *> &get_selection() override;
};
