/**************************************************************************/
/*  editor_scene_actions.cpp                                              */
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

#include "editor_scene_actions.h"

#include "core/object/class_db.h"
#include "core/object/script_language.h"
#include "core/variant/dictionary.h"
#include "scene/main/node.h"

void EditorSceneActionRegistration::_bind_methods() {
	// Same surface as `EditorViewportChromeRegistration`: only the lifecycle is
	// scriptable, the configuration setters are a C++ plugin API.
	ClassDB::bind_method(D_METHOD("unregister"), &EditorSceneActionRegistration::unregister);
	ClassDB::bind_method(D_METHOD("is_registered"), &EditorSceneActionRegistration::is_registered);
}

void EditorSceneActionRegistration::set_disabled(bool p_disabled, const String &p_reason) {
	disabled = p_disabled;
	disabled_reason = p_disabled ? p_reason : String();
}

void EditorSceneActionRegistration::set_tooltip(const String &p_tooltip) {
	tooltip = p_tooltip;
}

void EditorSceneActionRegistration::set_tooltip_provider(const Callable &p_callable) {
	tooltip_provider = p_callable;
}

void EditorSceneActionRegistration::set_availability_predicate(const Callable &p_callable) {
	availability_predicate = p_callable;
}

void EditorSceneActionRegistration::unregister() {
	if (!registered) {
		return;
	}
	if (registry) {
		registry->_unregister_registration(this);
	} else {
		registered = false;
	}
}

EditorSceneActionRegistration::~EditorSceneActionRegistration() {
	unregister();
}

EditorSceneActionRegistry *EditorSceneActionRegistry::singleton = nullptr;

void EditorSceneActionRegistry::create() {
	ERR_FAIL_COND(singleton != nullptr);
	singleton = memnew(EditorSceneActionRegistry);
}

void EditorSceneActionRegistry::free() {
	if (!singleton) {
		return;
	}
	memdelete(singleton);
	singleton = nullptr;
}

EditorSceneActionRegistry::~EditorSceneActionRegistry() {
	// Registrations may outlive the registry: the owner holds the Ref. Detach
	// them so their destructor cannot reach a dangling registry.
	while (class_registrations.size() > 0) {
		class_registrations[class_registrations.size() - 1]->unregister();
	}
	providers.clear();
}

void EditorSceneActionRegistry::_unregister_registration(EditorSceneActionRegistration *p_registration) {
	if (!p_registration->registered) {
		return;
	}
	p_registration->registered = false;
	p_registration->registry = nullptr;
	class_registrations.erase(p_registration);
}

Ref<EditorSceneActionRegistration> EditorSceneActionRegistry::register_class_action(
		const StringName &p_provider_id, const StringName &p_action_id,
		const StringName &p_class_name, const String &p_label,
		const StringName &p_icon, const Callable &p_invoke) {
	ERR_FAIL_COND_V(p_provider_id.is_empty(), Ref<EditorSceneActionRegistration>());
	ERR_FAIL_COND_V(p_action_id.is_empty(), Ref<EditorSceneActionRegistration>());
	ERR_FAIL_COND_V(p_class_name.is_empty(), Ref<EditorSceneActionRegistration>());
	ERR_FAIL_COND_V(!p_invoke.is_valid(), Ref<EditorSceneActionRegistration>());

	// A class name that resolves to neither world can never match a node. Warn
	// rather than fail: script global classes are only known once the project's
	// scripts have been scanned.
	if (!ClassDB::class_exists(p_class_name) && !ScriptServer::is_global_class(p_class_name)) {
		WARN_PRINT(vformat("Scene action \"%s/%s\" targets class \"%s\", which is neither an engine class nor a script global class. It will never match a node.", p_provider_id, p_action_id, p_class_name));
	}

	Ref<EditorSceneActionRegistration> registration;
	registration.instantiate();
	registration->registry = this;
	registration->provider_id = p_provider_id;
	registration->action_id = p_action_id;
	registration->class_name = p_class_name;
	registration->label = p_label;
	registration->icon_name = p_icon;
	registration->invoke_callable = p_invoke;
	registration->registered = true;
	class_registrations.push_back(registration.ptr());
	return registration;
}

bool EditorSceneActionRegistry::register_provider(EditorSceneActionProvider *p_provider) {
	ERR_FAIL_NULL_V(p_provider, false);
	const StringName provider_id = p_provider->get_provider_id();
	ERR_FAIL_COND_V(provider_id.is_empty(), false);
	ERR_FAIL_COND_V_MSG(provider_id == get_tool_button_provider_id(), false, "The scene action provider ID \"tool_button\" is reserved for automatic tool button discovery.");
	for (EditorSceneActionProvider *provider : providers) {
		ERR_FAIL_COND_V_MSG(provider->get_provider_id() == provider_id, false, vformat("A scene action provider with ID '%s' is already registered.", provider_id));
	}
	providers.push_back(p_provider);
	return true;
}

bool EditorSceneActionRegistry::unregister_provider(EditorSceneActionProvider *p_provider) {
	ERR_FAIL_NULL_V(p_provider, false);
	return providers.erase(p_provider);
}

bool EditorSceneActionRegistry::_script_chain_has_global_name(const Ref<Script> &p_script, const StringName &p_class) {
	Ref<Script> script = p_script;
	while (script.is_valid()) {
		if (script->get_global_name() == p_class) {
			return true;
		}
		script = script->get_base_script();
	}
	return false;
}

bool EditorSceneActionRegistry::node_matches_class(const Node *p_node, const StringName &p_class) {
	ERR_FAIL_NULL_V(p_node, false);
	if (p_class.is_empty()) {
		return false;
	}
	// Engine classes cover their subclasses through `is_class()`.
	if (ClassDB::class_exists(p_class)) {
		return p_node->is_class(p_class);
	}
	// Otherwise the name can only be a script global class; walk the chain.
	const Ref<Script> script = p_node->get_script();
	return _script_chain_has_global_name(script, p_class);
}

bool EditorSceneActionRegistry::_is_node_included(Node *p_scene_root, Node *p_node) {
	// Verbatim the SceneTreeEditor visibility rule, so the action list contains
	// exactly what the Scene dock shows.
	if (p_node == p_scene_root) {
		return true;
	}
	Node *owner = p_node->get_owner();
	if (owner == p_scene_root) {
		return true;
	}
	return owner != nullptr && p_scene_root->is_editable_instance(owner);
}

void EditorSceneActionRegistry::_fill_entry_address(Node *p_scene_root, Node *p_node, EditorSceneActionEntry &r_entry) {
	r_entry.node_id = p_node->get_instance_id();
	r_entry.node_path = p_scene_root->get_path_to(p_node);
	r_entry.node_name = p_node->get_name();
}

void EditorSceneActionRegistry::_build_match_cache(ClassMatchCache &r_cache) const {
	for (uint32_t i = 0; i < class_registrations.size(); i++) {
		if (ClassDB::class_exists(class_registrations[i]->class_name)) {
			r_cache.classdb_registrations.push_back(i);
		} else {
			r_cache.script_registrations.push_back(i);
		}
	}
}

bool EditorSceneActionRegistry::_tool_button_opts_in(const Ref<Script> &p_script, const StringName &p_property) {
	// The opt-in flag cannot ride on the PropertyInfo: the inspector splits the
	// tool button hint with `rsplit(",", true, 1)`, so a third hint token would
	// render as the icon. It rides on the script's per-member metadata instead
	// (`@export_tool_button`'s third argument, or `@field_meta("scene_action")`),
	// read here by name so the editor keeps no dependency on a language module.
	// A script language that exposes no such method never opts in.
	ERR_FAIL_COND_V(p_script.is_null(), false);

	const Variant member = p_property;
	const Variant *argument = &member;
	Callable::CallError ce;
	const Variant metadata = p_script->callp(SNAME("get_member_metadata"), &argument, 1, ce);
	if (ce.error != Callable::CallError::CALL_OK || metadata.get_type() != Variant::DICTIONARY) {
		return false;
	}

	const Dictionary metadata_dict = metadata;
	return metadata_dict.get(get_scene_action_metadata_key(), false).booleanize();
}

void EditorSceneActionRegistry::_collect_tool_buttons(Node *p_scene_root, Node *p_node, LocalVector<EditorSceneActionEntry> &r_out) const {
	// The `is_tool()` pre-filter is the whole performance story: only `@tool`
	// scripted nodes pay for `get_property_list()`.
	const Ref<Script> script = p_node->get_script();
	if (script.is_null() || !script->is_tool()) {
		return;
	}

	List<PropertyInfo> plist;
	p_node->get_property_list(&plist);
	for (const PropertyInfo &pi : plist) {
		if (pi.type != Variant::CALLABLE || pi.hint != PROPERTY_HINT_TOOL_BUTTON || !(pi.usage & PROPERTY_USAGE_EDITOR)) {
			continue;
		}

		// Tool buttons are opt-in: a scene carries far more of them than are
		// worth reaching without selecting their node, so only the ones that
		// ask for it surface here. Everything else stays inspector-only.
		if (!_tool_button_opts_in(script, pi.name)) {
			continue;
		}

		// Split exactly as `EditorInspectorToolButtonPlugin::parse_property()`.
		const PackedStringArray splits = pi.hint_string.rsplit(",", true, 1);
		const String &hint_text = splits[0]; // Safe since `splits` cannot be empty.
		const String &hint_icon = splits.size() > 1 ? splits[1] : "Callable";

		EditorSceneActionEntry entry;
		_fill_entry_address(p_scene_root, p_node, entry);
		entry.provider_id = get_tool_button_provider_id();
		entry.action_id = pi.name;
		entry.label = hint_text;
		entry.icon_name = hint_icon;
		entry.enabled = !(pi.usage & PROPERTY_USAGE_READ_ONLY);
		r_out.push_back(entry);
	}
}

void EditorSceneActionRegistry::_collect_provider_actions(Node *p_scene_root, Node *p_node, LocalVector<EditorSceneActionEntry> &r_out) const {
	for (EditorSceneActionProvider *provider : providers) {
		if (!provider->handles_node(p_node)) {
			continue;
		}
		const uint32_t first = r_out.size();
		provider->collect_actions(p_node, r_out);
		// The provider only fills the action-side fields; the registry owns the
		// address, so a provider can never hand back a stale node reference.
		for (uint32_t i = first; i < r_out.size(); i++) {
			_fill_entry_address(p_scene_root, p_node, r_out[i]);
			r_out[i].provider_id = provider->get_provider_id();
		}
	}
}

void EditorSceneActionRegistry::_emit_class_entries(Node *p_scene_root, Node *p_node, const LocalVector<uint32_t> &p_matched, LocalVector<EditorSceneActionEntry> &r_out) const {
	for (const uint32_t index : p_matched) {
		const EditorSceneActionRegistration *registration = class_registrations[index];

		EditorSceneActionEntry entry;
		_fill_entry_address(p_scene_root, p_node, entry);
		entry.provider_id = registration->provider_id;
		entry.action_id = registration->action_id;
		entry.label = registration->label;
		entry.icon_name = registration->icon_name;
		entry.tooltip = registration->tooltip;
		entry.enabled = !registration->disabled;
		entry.disabled_reason = registration->disabled ? registration->disabled_reason : String();

		if (registration->tooltip_provider.is_valid()) {
			const Variant result = registration->tooltip_provider.call(p_node);
			if (result.get_type() == Variant::STRING) {
				entry.tooltip = result;
			}
		}
		if (entry.enabled && registration->availability_predicate.is_valid()) {
			const Variant result = registration->availability_predicate.call(p_node);
			if (result.get_type() == Variant::BOOL && !bool(result)) {
				entry.enabled = false;
			}
		}
		r_out.push_back(entry);
	}
}

void EditorSceneActionRegistry::_collect_class_actions(Node *p_scene_root, Node *p_node, ClassMatchCache &r_cache, LocalVector<EditorSceneActionEntry> &r_out) const {
	if (!r_cache.classdb_registrations.is_empty()) {
		const StringName node_class = p_node->get_class_name();
		LocalVector<uint32_t> *matched = r_cache.by_class.getptr(node_class);
		if (!matched) {
			LocalVector<uint32_t> computed;
			for (const uint32_t index : r_cache.classdb_registrations) {
				if (p_node->is_class(class_registrations[index]->class_name)) {
					computed.push_back(index);
				}
			}
			matched = &r_cache.by_class.insert(node_class, computed)->value;
		}
		_emit_class_entries(p_scene_root, p_node, *matched, r_out);
	}

	if (r_cache.script_registrations.is_empty()) {
		return;
	}

	// Script global classes are a property of the node's script, not of its
	// engine class, so they get their own memo.
	const Ref<Script> script = p_node->get_script();
	const ObjectID script_id = script.is_valid() ? script->get_instance_id() : ObjectID();
	LocalVector<uint32_t> *matched = r_cache.by_script.getptr(script_id);
	if (!matched) {
		LocalVector<uint32_t> computed;
		if (script.is_valid()) {
			for (const uint32_t index : r_cache.script_registrations) {
				if (_script_chain_has_global_name(script, class_registrations[index]->class_name)) {
					computed.push_back(index);
				}
			}
		}
		matched = &r_cache.by_script.insert(script_id, computed)->value;
	}
	_emit_class_entries(p_scene_root, p_node, *matched, r_out);
}

void EditorSceneActionRegistry::_collect_recursive(Node *p_scene_root, Node *p_node, ClassMatchCache &r_cache, LocalVector<EditorSceneActionEntry> &r_out) const {
	if (_is_node_included(p_scene_root, p_node)) {
		_collect_tool_buttons(p_scene_root, p_node, r_out);
		_collect_provider_actions(p_scene_root, p_node, r_out);
		_collect_class_actions(p_scene_root, p_node, r_cache, r_out);
	}

	// Descend regardless: an editable instance's grandchildren are only
	// reachable through nodes that are themselves excluded.
	for (int i = 0; i < p_node->get_child_count(); i++) {
		_collect_recursive(p_scene_root, p_node->get_child(i), r_cache, r_out);
	}
}

void EditorSceneActionRegistry::collect(Node *p_scene_root, LocalVector<EditorSceneActionEntry> &r_out) const {
	ERR_FAIL_NULL(p_scene_root);
	ClassMatchCache cache;
	_build_match_cache(cache);
	_collect_recursive(p_scene_root, p_scene_root, cache, r_out);
}

Error EditorSceneActionRegistry::_invoke_tool_button(Node *p_node, const StringName &p_property) {
	// Line-for-line port of `EditorInspectorToolButtonPlugin::_call_action()` /
	// `_invoke_callable()`, minus the MultiNodeEdit branch. The Callable is read
	// here, never at scan time.
	const Variant value = p_node->get(p_property);
	ERR_FAIL_COND_V_MSG(value.get_type() != Variant::CALLABLE, ERR_INVALID_DATA, vformat(R"(The value of property "%s" is %s, but Callable was expected.)", p_property, Variant::get_type_name(value.get_type())));

	const Callable callable = value;
	ERR_FAIL_COND_V_MSG(!callable.is_valid(), ERR_INVALID_DATA, vformat(R"(Tool button action "%s" is an invalid callable.)", callable));

	Variant ret;
	Callable::CallError ce;
	callable.callp(nullptr, 0, ret, ce);
	ERR_FAIL_COND_V_MSG(ce.error != Callable::CallError::CALL_OK, FAILED, vformat(R"(Error calling tool button action "%s": %s)", callable, Variant::get_call_error_text(callable.get_method(), nullptr, 0, ce)));
	return OK;
}

Error EditorSceneActionRegistry::_invoke_resolved(Node *p_node, const StringName &p_provider_id, const StringName &p_action_id) const {
	ERR_FAIL_NULL_V(p_node, ERR_INVALID_PARAMETER);
	ERR_FAIL_COND_V(p_action_id.is_empty(), ERR_INVALID_PARAMETER);

	if (p_provider_id == get_tool_button_provider_id()) {
		return _invoke_tool_button(p_node, p_action_id);
	}

	for (EditorSceneActionProvider *provider : providers) {
		if (provider->get_provider_id() != p_provider_id) {
			continue;
		}
		return provider->invoke(p_node, p_action_id) ? OK : FAILED;
	}

	for (EditorSceneActionRegistration *registration : class_registrations) {
		if (registration->provider_id != p_provider_id || registration->action_id != p_action_id) {
			continue;
		}
		ERR_FAIL_COND_V_MSG(!registration->invoke_callable.is_valid(), ERR_INVALID_DATA, vformat(R"(Scene action "%s/%s" has an invalid callable.)", p_provider_id, p_action_id));
		Variant ret;
		Callable::CallError ce;
		const Variant node_argument = p_node;
		const Variant *arguments[1] = { &node_argument };
		registration->invoke_callable.callp(arguments, 1, ret, ce);
		ERR_FAIL_COND_V_MSG(ce.error != Callable::CallError::CALL_OK, FAILED, vformat(R"(Error calling scene action "%s/%s": %s)", p_provider_id, p_action_id, Variant::get_call_error_text(registration->invoke_callable.get_method(), arguments, 1, ce)));
		return OK;
	}

	ERR_FAIL_V_MSG(ERR_DOES_NOT_EXIST, vformat(R"(No scene action provider handles "%s/%s".)", p_provider_id, p_action_id));
}

Error EditorSceneActionRegistry::invoke(const EditorSceneActionEntry &p_entry) const {
	Node *node = Object::cast_to<Node>(ObjectDB::get_instance(p_entry.node_id));
	ERR_FAIL_NULL_V_MSG(node, ERR_INVALID_PARAMETER, vformat(R"(Failed to run scene action "%s" on a previously freed node.)", p_entry.action_id));
	return _invoke_resolved(node, p_entry.provider_id, p_entry.action_id);
}

Error EditorSceneActionRegistry::invoke_by_address(Node *p_scene_root, const NodePath &p_node_path,
		const StringName &p_provider_id, const StringName &p_action_id) const {
	ERR_FAIL_NULL_V(p_scene_root, ERR_INVALID_PARAMETER);
	Node *node = p_scene_root->get_node_or_null(p_node_path);
	ERR_FAIL_NULL_V_MSG(node, ERR_DOES_NOT_EXIST, vformat(R"(Scene action "%s/%s" targets node "%s", which no longer exists.)", p_provider_id, p_action_id, p_node_path));
	return _invoke_resolved(node, p_provider_id, p_action_id);
}
