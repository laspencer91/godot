/**************************************************************************/
/*  editor_scene_actions.h                                                */
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

#include "core/object/ref_counted.h"
#include "core/string/node_path.h"
#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "core/variant/callable.h"

class Node;
class Script;

// Scene Actions Phase 1: one discovered, addressable action. Plain struct with
// no GUI dependency; every field a future composite-action list would need to
// serialize lives here. The entry never holds a Callable -- invocation always
// re-resolves the node and re-reads the action at press time.
struct EditorSceneActionEntry {
	ObjectID node_id; // Live instance, re-resolved at invoke time.
	NodePath node_path; // scene_root->get_path_to(node): the persistent address.
	String node_name; // Display only.
	StringName provider_id; // "tool_button", or the C++ provider's ID.
	StringName action_id; // Tool button: the property name (stable across label edits).
	String label;
	StringName icon_name;
	String tooltip;
	bool enabled = true;
	String disabled_reason;
};

// Escape hatch for action sources that need more than one action per class.
// Providers are borrowed; owners unregister before destroying them.
class EditorSceneActionProvider {
public:
	virtual StringName get_provider_id() const = 0;
	virtual bool handles_node(const Node *p_node) const = 0;
	virtual void collect_actions(Node *p_node, LocalVector<EditorSceneActionEntry> &r_out) const = 0;
	virtual bool invoke(Node *p_node, const StringName &p_action_id) const = 0;

	virtual ~EditorSceneActionProvider() {}
};

class EditorSceneActionRegistry;

// One "single action on a single class" registration, the overwhelmingly common
// case for C++ editor plugins. The owner keeps the Ref; dropping it unregisters.
class EditorSceneActionRegistration : public RefCounted {
	GDCLASS(EditorSceneActionRegistration, RefCounted);

	friend class EditorSceneActionRegistry;

	// Borrowed. The registry clears this when the registration is removed, and
	// unregisters every surviving registration when it is destroyed.
	EditorSceneActionRegistry *registry = nullptr;

	StringName provider_id;
	StringName action_id;
	StringName class_name;
	String label;
	StringName icon_name;
	String tooltip;
	Callable invoke_callable; // (Node *) -> void
	Callable tooltip_provider; // (Node *) -> String
	Callable availability_predicate; // (Node *) -> bool
	bool disabled = false;
	String disabled_reason;
	bool registered = false;

protected:
	static void _bind_methods();

public:
	void set_disabled(bool p_disabled, const String &p_reason = String());
	bool is_disabled() const { return disabled; }

	void set_tooltip(const String &p_tooltip);
	void set_tooltip_provider(const Callable &p_callable);
	void set_availability_predicate(const Callable &p_callable);

	void unregister();
	bool is_registered() const { return registered; }

	~EditorSceneActionRegistration();
};

class EditorSceneActionRegistry {
	static EditorSceneActionRegistry *singleton;

	friend class EditorSceneActionRegistration;

	// Providers are borrowed. Registrations are borrowed too: the caller owns
	// the only Ref, and its destructor unregisters.
	LocalVector<EditorSceneActionProvider *> providers;
	LocalVector<EditorSceneActionRegistration *> class_registrations;

	// Per-scan memo so a large scene does the class walk once per distinct
	// class (engine classes) and once per distinct script (script classes),
	// instead of once per node.
	struct ClassMatchCache {
		LocalVector<uint32_t> classdb_registrations;
		LocalVector<uint32_t> script_registrations;
		HashMap<StringName, LocalVector<uint32_t>> by_class;
		HashMap<ObjectID, LocalVector<uint32_t>> by_script;
	};

	static bool _is_node_included(Node *p_scene_root, Node *p_node);
	static void _fill_entry_address(Node *p_scene_root, Node *p_node, EditorSceneActionEntry &r_entry);
	static bool _script_chain_has_global_name(const Ref<Script> &p_script, const StringName &p_class);
	static Error _invoke_tool_button(Node *p_node, const StringName &p_property);

	void _build_match_cache(ClassMatchCache &r_cache) const;
	void _collect_recursive(Node *p_scene_root, Node *p_node, ClassMatchCache &r_cache, LocalVector<EditorSceneActionEntry> &r_out) const;
	void _collect_tool_buttons(Node *p_scene_root, Node *p_node, LocalVector<EditorSceneActionEntry> &r_out) const;
	void _collect_provider_actions(Node *p_scene_root, Node *p_node, LocalVector<EditorSceneActionEntry> &r_out) const;
	void _collect_class_actions(Node *p_scene_root, Node *p_node, ClassMatchCache &r_cache, LocalVector<EditorSceneActionEntry> &r_out) const;
	void _emit_class_entries(Node *p_scene_root, Node *p_node, const LocalVector<uint32_t> &p_matched, LocalVector<EditorSceneActionEntry> &r_out) const;
	Error _invoke_resolved(Node *p_node, const StringName &p_provider_id, const StringName &p_action_id) const;

	void _unregister_registration(EditorSceneActionRegistration *p_registration);

public:
	static void create();
	static void free();
	static EditorSceneActionRegistry *get_singleton() { return singleton; }

	static StringName get_tool_button_provider_id() { return SNAME("tool_button"); }

	// Explicit side (C++ plugins).
	Ref<EditorSceneActionRegistration> register_class_action(
			const StringName &p_provider_id, const StringName &p_action_id,
			const StringName &p_class_name, const String &p_label,
			const StringName &p_icon, const Callable &p_invoke);

	bool register_provider(EditorSceneActionProvider *p_provider);
	bool unregister_provider(EditorSceneActionProvider *p_provider);

	// Automatic side (tool buttons) + explicit side, in scene-tree order.
	void collect(Node *p_scene_root, LocalVector<EditorSceneActionEntry> &r_out) const;

	Error invoke(const EditorSceneActionEntry &p_entry) const;
	Error invoke_by_address(Node *p_scene_root, const NodePath &p_node_path,
			const StringName &p_provider_id, const StringName &p_action_id) const;

	static bool node_matches_class(const Node *p_node, const StringName &p_class);

	~EditorSceneActionRegistry();
};
