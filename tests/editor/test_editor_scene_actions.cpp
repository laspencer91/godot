/**************************************************************************/
/*  test_editor_scene_actions.cpp                                         */
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

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_editor_scene_actions)

#ifdef TOOLS_ENABLED

#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/object/script_language.h"
#include "editor/gui/editor_scene_actions.h"
#include "scene/3d/node_3d.h"
#include "scene/main/node.h"
#include "scene/resources/packed_scene.h"

namespace TestEditorSceneActions {

// Minimal script stub. The unit-test harness has no project filesystem, so a
// real GDScript with `class_name` (let alone a base-script chain) cannot be
// compiled here. This stub exercises exactly the accessors the registry uses:
// `get_global_name()` / `get_base_script()` for class matching, `is_tool()` for
// the harvest pre-filter, and a placeholder instance carrying the same
// PropertyInfo that `@export_tool_button` makes the GDScript parser emit.
class TestSceneActionScript : public Script {
	GDCLASS(TestSceneActionScript, Script);

protected:
	static void _bind_methods() {
		// The registry reads the Scene Actions opt-in by name, so the stub has
		// to expose the same bound method GDScript does.
		ClassDB::bind_method(D_METHOD("get_member_metadata", "member"), &TestSceneActionScript::get_member_metadata);
	}

public:
	StringName global_name;
	Ref<Script> base;
	bool tool_script = true;
	List<PropertyInfo> fake_properties;
	HashMap<StringName, Variant> fake_values;
	HashMap<StringName, Dictionary> fake_member_metadata;

	// Mirrors `GDScript::get_member_metadata()`, the per-member script metadata
	// surface `@field_meta` / `@export_tool_button`'s opt-in flag write to.
	Dictionary get_member_metadata(const StringName &p_member) const {
		const Dictionary *metadata = fake_member_metadata.getptr(p_member);
		return metadata ? *metadata : Dictionary();
	}

	PlaceHolderScriptInstance *make_instance(Object *p_this) {
		PlaceHolderScriptInstance *placeholder = memnew(PlaceHolderScriptInstance(nullptr, Ref<Script>(this), p_this));
		placeholder->update(fake_properties, fake_values);
		return placeholder;
	}

	virtual bool can_instantiate() const override { return true; }
	virtual ScriptInstance *instance_create(Object *p_this) override { return make_instance(p_this); }
	virtual PlaceHolderScriptInstance *placeholder_instance_create(Object *p_this) override { return make_instance(p_this); }

	virtual Ref<Script> get_base_script() const override { return base; }
	virtual StringName get_global_name() const override { return global_name; }
	virtual bool inherits_script(const Ref<Script> &p_script) const override { return false; }
	virtual StringName get_instance_base_type() const override { return SNAME("Node3D"); }
	virtual bool has_source_code() const override { return false; }
	virtual String get_source_code() const override { return String(); }
	virtual void set_source_code(const String &p_code) override {}
	virtual Error reload(bool p_keep_state = false) override { return OK; }
	virtual StringName get_doc_class_name() const override { return StringName(); }
	virtual Vector<DocData::ClassDoc> get_documentation() const override { return Vector<DocData::ClassDoc>(); }
	virtual String get_class_icon_path() const override { return String(); }
	virtual bool has_method(const StringName &p_method) const override { return false; }
	virtual MethodInfo get_method_info(const StringName &p_method) const override { return MethodInfo(); }
	virtual bool is_tool() const override { return tool_script; }
	virtual bool is_script_valid() const override { return true; }
	virtual bool is_abstract() const override { return false; }
	virtual ScriptLanguage *get_language() const override { return nullptr; }
	virtual bool has_script_signal(const StringName &p_signal) const override { return false; }
	virtual void get_script_signal_list(List<MethodInfo> *r_signals) const override {}
	virtual bool get_property_default_value(const StringName &p_property, Variant &r_value) const override { return false; }
	virtual void get_script_method_list(List<MethodInfo> *p_list) const override {}
	virtual void get_script_property_list(List<PropertyInfo> *p_list) const override {}
	virtual const Variant get_rpc_config() const override { return Variant(); }
};

// The node a tool button lives on, and the target of its Callable.
class TestSceneActionNode : public Node3D {
	GDCLASS(TestSceneActionNode, Node3D);

public:
	int invoke_count = 0;

	void ping() { invoke_count++; }
};

// Receiver for `register_class_action()` invocations.
class TestSceneActionSink : public Object {
	GDCLASS(TestSceneActionSink, Object);

public:
	int calls = 0;
	ObjectID last_node;

	void bake_node(Node *p_node) {
		calls++;
		last_node = p_node ? p_node->get_instance_id() : ObjectID();
	}

	String tooltip_for(Node *p_node) { return "Tooltip for " + String(p_node->get_name()); }
	bool available_for(Node *p_node) { return false; }
};

class TestSceneActionStubProvider : public EditorSceneActionProvider {
public:
	mutable int collect_calls = 0;
	mutable int invoke_calls = 0;
	mutable ObjectID last_node;
	mutable StringName last_action;
	bool invoke_result = true;

	virtual StringName get_provider_id() const override { return SNAME("test_scene_action_stub"); }

	virtual bool handles_node(const Node *p_node) const override {
		return Object::cast_to<Node3D>(p_node) != nullptr;
	}

	virtual void collect_actions(Node *p_node, LocalVector<EditorSceneActionEntry> &r_out) const override {
		collect_calls++;
		EditorSceneActionEntry entry;
		entry.action_id = SNAME("stub_action");
		entry.label = "Stub Action";
		entry.icon_name = SNAME("Callable");
		r_out.push_back(entry);
	}

	virtual bool invoke(Node *p_node, const StringName &p_action_id) const override {
		invoke_calls++;
		last_node = p_node->get_instance_id();
		last_action = p_action_id;
		return invoke_result;
	}
};

static Ref<TestSceneActionScript> _make_script(const StringName &p_global_name, bool p_tool = true) {
	// `get_member_metadata()` is only reachable once the class is in ClassDB.
	if (!ClassDB::class_exists(TestSceneActionScript::get_class_static())) {
		GDREGISTER_CLASS(TestSceneActionScript);
	}

	Ref<TestSceneActionScript> script;
	script.instantiate();
	script->global_name = p_global_name;
	script->tool_script = p_tool;
	return script;
}

// Mirrors what `@export_tool_button("Ping", "Bake", <scene_action>)` produces:
// the PropertyInfo is identical either way, and the opt-in flag only shows up
// as member metadata. `p_scene_action == OPT_OUT` writes no metadata at all,
// like a two-argument annotation.
enum ToolButtonOptIn {
	OPT_OUT,
	OPT_IN,
	OPT_OUT_EXPLICIT, // `@field_meta("scene_action", false)`.
};

static void _add_tool_button(const Ref<TestSceneActionScript> &p_script, const StringName &p_name, const String &p_hint_string, const Variant &p_value, ToolButtonOptIn p_scene_action = OPT_IN, uint32_t p_extra_usage = 0) {
	PropertyInfo pi(Variant::CALLABLE, p_name, PROPERTY_HINT_TOOL_BUTTON, p_hint_string, PROPERTY_USAGE_EDITOR | p_extra_usage);
	p_script->fake_properties.push_back(pi);
	p_script->fake_values[p_name] = p_value;

	if (p_scene_action != OPT_OUT) {
		Dictionary metadata;
		metadata[EditorSceneActionRegistry::get_scene_action_metadata_key()] = p_scene_action == OPT_IN;
		p_script->fake_member_metadata[p_name] = metadata;
	}
}

// Root -> [Owned, Instance -> InnerChild]. `Instance` is an instanced sub-scene
// root owned by the edited scene; `InnerChild` is owned by `Instance`.
static Node3D *_make_scene(Node3D **r_owned, Node **r_instance, Node **r_inner_child) {
	Node3D *inner_root = memnew(Node3D);
	inner_root->set_name("Instance");
	Node3D *inner_child = memnew(Node3D);
	inner_child->set_name("InnerChild");
	inner_root->add_child(inner_child);
	inner_child->set_owner(inner_root);

	Ref<PackedScene> packed;
	packed.instantiate();
	REQUIRE(packed->pack(inner_root) == OK);
	memdelete(inner_root);

	Node3D *root = memnew(Node3D);
	root->set_name("Root");

	Node3D *owned = memnew(Node3D);
	owned->set_name("Owned");
	root->add_child(owned);
	owned->set_owner(root);

	Node *instance = packed->instantiate();
	REQUIRE(instance != nullptr);
	instance->set_name("Instance");
	root->add_child(instance);
	instance->set_owner(root);

	REQUIRE(instance->get_child_count() == 1);
	REQUIRE(instance->get_child(0)->get_owner() == instance);

	*r_owned = owned;
	*r_instance = instance;
	*r_inner_child = instance->get_child(0);
	return root;
}

static bool _has_path(const LocalVector<EditorSceneActionEntry> &p_entries, const String &p_path) {
	for (const EditorSceneActionEntry &entry : p_entries) {
		if (String(entry.node_path) == p_path) {
			return true;
		}
	}
	return false;
}

TEST_CASE("[Editor][SceneActions] Registry singleton is created with the editor types") {
	CHECK(EditorSceneActionRegistry::get_singleton() != nullptr);
	CHECK(EditorSceneActionRegistry::get_tool_button_provider_id() == StringName("tool_button"));
	CHECK(EditorSceneActionRegistry::get_scene_action_metadata_key() == StringName("scene_action"));
}

TEST_CASE("[Editor][SceneActions] node_matches_class resolves engine classes") {
	Node3D *node = memnew(Node3D);

	// Exact engine class.
	CHECK(EditorSceneActionRegistry::node_matches_class(node, SNAME("Node3D")));
	// Engine base class: the `is_class()` walk covers subclasses.
	CHECK(EditorSceneActionRegistry::node_matches_class(node, SNAME("Node")));
	// Unrelated engine class.
	CHECK_FALSE(EditorSceneActionRegistry::node_matches_class(node, SNAME("Camera3D")));
	// Nonexistent class name, with no script to fall back to.
	CHECK_FALSE(EditorSceneActionRegistry::node_matches_class(node, SNAME("ThisClassDoesNotExist_SceneActions")));
	CHECK_FALSE(EditorSceneActionRegistry::node_matches_class(node, StringName()));

	// A C++ subclass matches its engine base.
	TestSceneActionNode *derived = memnew(TestSceneActionNode);
	CHECK(EditorSceneActionRegistry::node_matches_class(derived, SNAME("Node3D")));

	memdelete(derived);
	memdelete(node);
}

TEST_CASE("[Editor][SceneActions] node_matches_class resolves script global classes") {
	Node3D *node = memnew(Node3D);

	Ref<TestSceneActionScript> base_script = _make_script(SNAME("TestSceneActionBaseClass"));
	Ref<TestSceneActionScript> derived_script = _make_script(SNAME("TestSceneActionDerivedClass"));
	derived_script->base = base_script;
	node->set_script(derived_script);
	REQUIRE(node->get_script().get_type() == Variant::OBJECT);

	// The script's own global class name.
	CHECK(EditorSceneActionRegistry::node_matches_class(node, SNAME("TestSceneActionDerivedClass")));
	// A global class name further up the base-script chain.
	CHECK(EditorSceneActionRegistry::node_matches_class(node, SNAME("TestSceneActionBaseClass")));
	// An unrelated script class.
	CHECK_FALSE(EditorSceneActionRegistry::node_matches_class(node, SNAME("TestSceneActionUnrelatedClass")));
	// A scripted node still matches through ClassDB.
	CHECK(EditorSceneActionRegistry::node_matches_class(node, SNAME("Node3D")));

	node->set_script(Variant());
	memdelete(node);
}

TEST_CASE("[Editor][SceneActions] collect() walks the scene like the Scene dock") {
	Node3D *owned = nullptr;
	Node *instance = nullptr;
	Node *inner_child = nullptr;
	Node3D *root = _make_scene(&owned, &instance, &inner_child);

	EditorSceneActionRegistry registry;
	TestSceneActionSink *sink = memnew(TestSceneActionSink);
	Ref<EditorSceneActionRegistration> registration = registry.register_class_action(
			SNAME("test_walk"), SNAME("bake"), SNAME("Node3D"), "Bake", SNAME("Bake"),
			callable_mp(sink, &TestSceneActionSink::bake_node));
	REQUIRE(registration.is_valid());

	LocalVector<EditorSceneActionEntry> entries;
	registry.collect(root, entries);

	// Root, owned child and the instanced sub-scene root; not the instance's
	// own children.
	CHECK(entries.size() == 3);
	CHECK(_has_path(entries, "."));
	CHECK(_has_path(entries, "Owned"));
	CHECK(_has_path(entries, "Instance"));
	CHECK_FALSE(_has_path(entries, "Instance/InnerChild"));

	// Depth-first scene-tree order, plus the address fields the menu needs.
	CHECK(entries[0].node_id == root->get_instance_id());
	CHECK(entries[0].node_name == "Root");
	CHECK(entries[1].node_id == owned->get_instance_id());
	CHECK(entries[2].node_id == instance->get_instance_id());
	CHECK(entries[0].provider_id == StringName("test_walk"));
	CHECK(entries[0].action_id == StringName("bake"));
	CHECK(entries[0].label == "Bake");
	CHECK(entries[0].icon_name == StringName("Bake"));
	CHECK(entries[0].enabled);

	// Invoking a class action reaches the registered Callable with the node.
	CHECK(registry.invoke(entries[1]) == OK);
	CHECK(sink->calls == 1);
	CHECK(sink->last_node == owned->get_instance_id());

	// Editable Children makes the instance's children visible to the walk.
	root->set_editable_instance(instance, true);
	entries.clear();
	registry.collect(root, entries);
	CHECK(entries.size() == 4);
	CHECK(_has_path(entries, "Instance/InnerChild"));
	CHECK(entries[3].node_id == inner_child->get_instance_id());

	memdelete(root);
	memdelete(sink);
}

TEST_CASE("[Editor][SceneActions] Class registrations honor disabled state and callables") {
	Node3D *root = memnew(Node3D);
	root->set_name("Root");

	EditorSceneActionRegistry registry;
	TestSceneActionSink *sink = memnew(TestSceneActionSink);

	SUBCASE("Disabled registrations report their reason") {
		Ref<EditorSceneActionRegistration> registration = registry.register_class_action(
				SNAME("test_state"), SNAME("bake"), SNAME("Node3D"), "Bake", SNAME("Bake"),
				callable_mp(sink, &TestSceneActionSink::bake_node));
		REQUIRE(registration.is_valid());
		registration->set_disabled(true, "Not supported on this GPU");
		registration->set_tooltip("Static tooltip");

		LocalVector<EditorSceneActionEntry> entries;
		registry.collect(root, entries);
		REQUIRE(entries.size() == 1);
		CHECK_FALSE(entries[0].enabled);
		CHECK(entries[0].disabled_reason == "Not supported on this GPU");
		CHECK(entries[0].tooltip == "Static tooltip");
	}

	SUBCASE("Tooltip provider and availability predicate are consulted per node") {
		Ref<EditorSceneActionRegistration> registration = registry.register_class_action(
				SNAME("test_state"), SNAME("bake"), SNAME("Node3D"), "Bake", SNAME("Bake"),
				callable_mp(sink, &TestSceneActionSink::bake_node));
		REQUIRE(registration.is_valid());
		registration->set_tooltip_provider(callable_mp(sink, &TestSceneActionSink::tooltip_for));
		registration->set_availability_predicate(callable_mp(sink, &TestSceneActionSink::available_for));

		LocalVector<EditorSceneActionEntry> entries;
		registry.collect(root, entries);
		REQUIRE(entries.size() == 1);
		CHECK(entries[0].tooltip == "Tooltip for Root");
		CHECK_FALSE(entries[0].enabled);
	}

	SUBCASE("Dropping the Ref unregisters the action") {
		{
			Ref<EditorSceneActionRegistration> registration = registry.register_class_action(
					SNAME("test_state"), SNAME("bake"), SNAME("Node3D"), "Bake", SNAME("Bake"),
					callable_mp(sink, &TestSceneActionSink::bake_node));
			REQUIRE(registration.is_valid());
			CHECK(registration->is_registered());
		}
		LocalVector<EditorSceneActionEntry> entries;
		registry.collect(root, entries);
		CHECK(entries.is_empty());
	}

	SUBCASE("A class name in neither world warns and never matches") {
		ERR_PRINT_OFF;
		Ref<EditorSceneActionRegistration> registration = registry.register_class_action(
				SNAME("test_state"), SNAME("bake"), SNAME("ThisClassDoesNotExist_SceneActions"), "Bake", SNAME("Bake"),
				callable_mp(sink, &TestSceneActionSink::bake_node));
		ERR_PRINT_ON;
		REQUIRE(registration.is_valid());
		LocalVector<EditorSceneActionEntry> entries;
		registry.collect(root, entries);
		CHECK(entries.is_empty());
	}

	memdelete(root);
	memdelete(sink);
}

TEST_CASE("[Editor][SceneActions] Tool buttons are harvested from @tool scripts and invoked") {
	Node3D *root = memnew(Node3D);
	root->set_name("Root");
	TestSceneActionNode *node = memnew(TestSceneActionNode);
	node->set_name("ToolNode");
	root->add_child(node);
	node->set_owner(root);

	EditorSceneActionRegistry registry;

	SUBCASE("A node without a script yields nothing") {
		LocalVector<EditorSceneActionEntry> entries;
		registry.collect(root, entries);
		CHECK(entries.is_empty());
	}

	SUBCASE("A non-tool script is pruned before get_property_list()") {
		Ref<TestSceneActionScript> script = _make_script(SNAME("TestSceneActionPlainClass"), false);
		_add_tool_button(script, SNAME("test_tool_button"), "Ping,Bake", callable_mp(node, &TestSceneActionNode::ping));
		node->set_script(script);

		LocalVector<EditorSceneActionEntry> entries;
		registry.collect(root, entries);
		CHECK(entries.is_empty());

		node->set_script(Variant());
	}

	SUBCASE("A tool script yields a label/icon split entry that invokes the Callable") {
		Ref<TestSceneActionScript> script = _make_script(SNAME("TestSceneActionToolClass"));
		_add_tool_button(script, SNAME("test_tool_button"), "Ping,Bake", callable_mp(node, &TestSceneActionNode::ping));
		node->set_script(script);

		LocalVector<EditorSceneActionEntry> entries;
		registry.collect(root, entries);
		REQUIRE(entries.size() == 1);
		CHECK(entries[0].provider_id == EditorSceneActionRegistry::get_tool_button_provider_id());
		CHECK(entries[0].action_id == StringName("test_tool_button"));
		CHECK(entries[0].label == "Ping");
		CHECK(entries[0].icon_name == StringName("Bake"));
		CHECK(entries[0].node_name == "ToolNode");
		CHECK(String(entries[0].node_path) == "ToolNode");
		CHECK(entries[0].enabled);

		// Invocation by live instance, then by persistent address.
		CHECK(registry.invoke(entries[0]) == OK);
		CHECK(node->invoke_count == 1);
		CHECK(registry.invoke_by_address(root, entries[0].node_path,
					  entries[0].provider_id, entries[0].action_id) == OK);
		CHECK(node->invoke_count == 2);

		node->set_script(Variant());
	}

	SUBCASE("Only tool buttons that opt in are collected") {
		Ref<TestSceneActionScript> script = _make_script(SNAME("TestSceneActionToolClass"));
		// `@export_tool_button("Bake", "Bake", true)`.
		_add_tool_button(script, SNAME("test_opted_in_button"), "Bake,Bake", callable_mp(node, &TestSceneActionNode::ping), OPT_IN);
		// `@export_tool_button("Clear", "Remove")` -- the backward-compatible
		// two-argument form stays inspector-only.
		_add_tool_button(script, SNAME("test_inspector_only_button"), "Clear,Remove", callable_mp(node, &TestSceneActionNode::ping), OPT_OUT);
		// `@field_meta("scene_action", false)` opts out just as loudly.
		_add_tool_button(script, SNAME("test_explicitly_excluded_button"), "Rebuild,Reload", callable_mp(node, &TestSceneActionNode::ping), OPT_OUT_EXPLICIT);
		node->set_script(script);

		LocalVector<EditorSceneActionEntry> entries;
		registry.collect(root, entries);
		REQUIRE(entries.size() == 1);
		CHECK(entries[0].action_id == StringName("test_opted_in_button"));
		CHECK(entries[0].label == "Bake");

		// The opted-in entry still invokes like any other tool button.
		CHECK(registry.invoke(entries[0]) == OK);
		CHECK(node->invoke_count == 1);

		node->set_script(Variant());
	}

	SUBCASE("A hint string without an icon falls back to Callable") {
		Ref<TestSceneActionScript> script = _make_script(SNAME("TestSceneActionToolClass"));
		_add_tool_button(script, SNAME("test_tool_button"), "Ping", callable_mp(node, &TestSceneActionNode::ping));
		node->set_script(script);

		LocalVector<EditorSceneActionEntry> entries;
		registry.collect(root, entries);
		REQUIRE(entries.size() == 1);
		CHECK(entries[0].label == "Ping");
		CHECK(entries[0].icon_name == StringName("Callable"));

		node->set_script(Variant());
	}

	SUBCASE("PROPERTY_USAGE_READ_ONLY disables the entry") {
		Ref<TestSceneActionScript> script = _make_script(SNAME("TestSceneActionToolClass"));
		_add_tool_button(script, SNAME("test_tool_button"), "Ping,Bake", callable_mp(node, &TestSceneActionNode::ping), OPT_IN, PROPERTY_USAGE_READ_ONLY);
		node->set_script(script);

		LocalVector<EditorSceneActionEntry> entries;
		registry.collect(root, entries);
		REQUIRE(entries.size() == 1);
		CHECK_FALSE(entries[0].enabled);

		node->set_script(Variant());
	}

	SUBCASE("A non-Callable property value errors instead of crashing") {
		Ref<TestSceneActionScript> script = _make_script(SNAME("TestSceneActionToolClass"));
		_add_tool_button(script, SNAME("test_tool_button"), "Ping,Bake", 42);
		node->set_script(script);

		LocalVector<EditorSceneActionEntry> entries;
		registry.collect(root, entries);
		REQUIRE(entries.size() == 1);
		ERR_PRINT_OFF;
		CHECK(registry.invoke(entries[0]) != OK);
		ERR_PRINT_ON;
		CHECK(node->invoke_count == 0);

		node->set_script(Variant());
	}

	memdelete(root);
}

TEST_CASE("[Editor][SceneActions] invoke_by_address round-trips through a stub provider") {
	EditorSceneActionRegistry *registry = EditorSceneActionRegistry::get_singleton();
	REQUIRE(registry != nullptr);

	TestSceneActionStubProvider provider;
	REQUIRE(registry->register_provider(&provider));
	ERR_PRINT_OFF;
	CHECK_FALSE(registry->register_provider(&provider));
	ERR_PRINT_ON;

	Node3D *owned = nullptr;
	Node *instance = nullptr;
	Node *inner_child = nullptr;
	Node3D *root = _make_scene(&owned, &instance, &inner_child);

	LocalVector<EditorSceneActionEntry> entries;
	registry->collect(root, entries);
	REQUIRE(entries.size() == 3);
	CHECK(provider.collect_calls == 3);
	// The registry, not the provider, owns the address fields.
	CHECK(entries[1].provider_id == provider.get_provider_id());
	CHECK(entries[1].action_id == StringName("stub_action"));
	CHECK(entries[1].node_id == owned->get_instance_id());
	CHECK(String(entries[1].node_path) == "Owned");
	CHECK(entries[1].node_name == "Owned");

	// The (NodePath, provider_id, action_id) triple is enough to invoke.
	CHECK(registry->invoke_by_address(root, entries[1].node_path,
				  entries[1].provider_id, entries[1].action_id) == OK);
	CHECK(provider.invoke_calls == 1);
	CHECK(provider.last_node == owned->get_instance_id());
	CHECK(provider.last_action == StringName("stub_action"));

	// The same entry through the live-instance path.
	CHECK(registry->invoke(entries[1]) == OK);
	CHECK(provider.invoke_calls == 2);

	// A provider that reports failure surfaces it.
	provider.invoke_result = false;
	CHECK(registry->invoke(entries[1]) == FAILED);

	// Unknown address and unknown provider both fail without invoking.
	ERR_PRINT_OFF;
	CHECK(registry->invoke_by_address(root, NodePath("NoSuchNode"),
				  provider.get_provider_id(), SNAME("stub_action")) == ERR_DOES_NOT_EXIST);
	CHECK(registry->invoke_by_address(root, entries[1].node_path,
				  SNAME("no_such_provider"), SNAME("stub_action")) == ERR_DOES_NOT_EXIST);
	ERR_PRINT_ON;
	CHECK(provider.invoke_calls == 3);

	memdelete(root);
	CHECK(registry->unregister_provider(&provider));
	CHECK_FALSE(registry->unregister_provider(&provider));
}

} // namespace TestEditorSceneActions

#endif // TOOLS_ENABLED
