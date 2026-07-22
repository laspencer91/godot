/**************************************************************************/
/*  test_editor_edit_domain.cpp                                           */
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

TEST_FORCE_LINK(test_editor_edit_domain)

#ifdef TOOLS_ENABLED

#include "core/input/input_event.h"
#include "editor/gui/editor_edit_domain.h"
#include "editor/gui/editor_viewport_chrome.h"
#include "scene/gui/control.h"
#include "scene/main/scene_tree.h"

namespace TestEditorEditDomain {

struct SessionStats {
	int created = 0;
	int entered = 0;
	int exited = 0;
	int destroyed = 0;
	int input_calls = 0;
	int escape_calls = 0;
	int toggle_calls = 0;
	int rail_builds = 0;
	int panel_builds = 0;
	ObjectID rail_id;
	ObjectID panel_id;
};

class TestDomainSession : public EditorEditDomainSession {
	SessionStats *stats = nullptr;
	bool preserve_native_navigation = false;
	bool build_controls = false;

public:
	EditorEditDomainInput programmed_result = EditorEditDomainInput::PASS_TO_VIEWPORT;
	Node3DEditorView *entered_view = nullptr;
	bool gesture_active = false;
	bool toggle_handled = true;
	bool tool_toggled = false;

	virtual void enter(const EditorEditDomainContext &p_context) override {
		stats->entered++;
		entered_view = p_context.view;
	}

	virtual void exit() override {
		stats->exited++;
	}

	virtual EditorEditDomainInput handle_input(const EditorEditDomainContext &p_context, Camera3D *p_camera, const Ref<InputEvent> &p_event) override {
		stats->input_calls++;
		if (preserve_native_navigation) {
			Ref<InputEventMouseButton> mouse_button = p_event;
			if (mouse_button.is_valid()) {
				const MouseButton button = mouse_button->get_button_index();
				if (button == MouseButton::MIDDLE || button == MouseButton::WHEEL_UP || button == MouseButton::WHEEL_DOWN || button == MouseButton::WHEEL_LEFT || button == MouseButton::WHEEL_RIGHT) {
					return EditorEditDomainInput::PASS_TO_VIEWPORT;
				}
				if (button == MouseButton::RIGHT && !mouse_button->is_alt_pressed() && !mouse_button->is_shift_pressed() && !mouse_button->is_ctrl_pressed() && !mouse_button->is_meta_pressed()) {
					return EditorEditDomainInput::PASS_TO_VIEWPORT;
				}
				if (button == MouseButton::LEFT && (mouse_button->is_alt_pressed() || mouse_button->is_shift_pressed() || mouse_button->is_ctrl_pressed() || mouse_button->is_meta_pressed())) {
					return EditorEditDomainInput::PASS_TO_VIEWPORT;
				}
			}
		}
		return programmed_result;
	}

	virtual bool handle_escape() override {
		stats->escape_calls++;
		if (gesture_active) {
			gesture_active = false;
			return true;
		}
		return false;
	}

	virtual bool handle_tool_toggle() override {
		stats->toggle_calls++;
		if (toggle_handled) {
			tool_toggled = !tool_toggled;
		}
		return toggle_handled;
	}

	virtual Control *build_tool_rail() override {
		if (!build_controls) {
			return nullptr;
		}
		stats->rail_builds++;
		Control *rail = memnew(Control);
		stats->rail_id = rail->get_instance_id();
		return rail;
	}

	virtual Control *build_contextual_panel() override {
		if (!build_controls) {
			return nullptr;
		}
		stats->panel_builds++;
		Control *panel = memnew(Control);
		stats->panel_id = panel->get_instance_id();
		return panel;
	}

	TestDomainSession(SessionStats *p_stats, EditorEditDomainInput p_programmed_result, bool p_preserve_native_navigation, bool p_build_controls) {
		stats = p_stats;
		programmed_result = p_programmed_result;
		preserve_native_navigation = p_preserve_native_navigation;
		build_controls = p_build_controls;
		stats->created++;
	}

	~TestDomainSession() {
		stats->destroyed++;
	}
};

class TestDomainProvider : public EditorEditDomainProvider {
	StringName domain_id;
	SessionStats *stats = nullptr;
	bool available = true;
	bool double_click = false;
	EditorEditDomainInput initial_result = EditorEditDomainInput::PASS_TO_VIEWPORT;
	bool preserve_native_navigation = false;
	bool build_controls = false;

public:
	virtual StringName get_domain_id() const override { return domain_id; }
	virtual bool is_available(const EditorEditDomainContext &p_context) const override { return available; }
	virtual bool can_activate_from_double_click(const EditorEditDomainContext &p_context, ObjectID p_hit) const override { return double_click; }

	virtual EditorEditDomainSession *create_session(const EditorEditDomainContext &p_context) const override {
		return memnew(TestDomainSession(stats, initial_result, preserve_native_navigation, build_controls));
	}

	TestDomainProvider(const StringName &p_domain_id, SessionStats *p_stats, bool p_available = true, bool p_double_click = false, EditorEditDomainInput p_initial_result = EditorEditDomainInput::PASS_TO_VIEWPORT, bool p_preserve_native_navigation = false, bool p_build_controls = false) {
		domain_id = p_domain_id;
		stats = p_stats;
		available = p_available;
		double_click = p_double_click;
		initial_result = p_initial_result;
		preserve_native_navigation = p_preserve_native_navigation;
		build_controls = p_build_controls;
	}
};

static EditorEditDomainInput _route_mouse_button(EditorEditDomainHost &p_host, MouseButton p_button, bool p_alt_pressed = false) {
	Ref<InputEventMouseButton> event;
	event.instantiate();
	event->set_button_index(p_button);
	event->set_pressed(true);
	event->set_alt_pressed(p_alt_pressed);
	return p_host.route_input(nullptr, nullptr, event);
}

static Ref<InputEventKey> _make_key_event(Key p_keycode) {
	Ref<InputEventKey> event;
	event.instantiate();
	event->set_keycode(p_keycode);
	event->set_pressed(true);
	return event;
}

TEST_CASE("[Editor][EditorEditDomain] Registry honors availability and double-click activation") {
	EditorEditDomainRegistry registry;
	SessionStats stats;
	TestDomainProvider primary(SNAME("test_domain_primary"), &stats);
	TestDomainProvider duplicate(SNAME("test_domain_primary"), &stats);
	TestDomainProvider unavailable(SNAME("test_domain_unavailable"), &stats, false, true);
	TestDomainProvider activator(SNAME("test_domain_activator"), &stats, true, true);

	REQUIRE(registry.register_provider(&primary));
	CHECK(registry.get_provider(primary.get_domain_id()) == &primary);
	ERR_PRINT_OFF;
	CHECK_FALSE(registry.register_provider(&duplicate));
	ERR_PRINT_ON;
	REQUIRE(registry.register_provider(&unavailable));
	REQUIRE(registry.register_provider(&activator));

	EditorEditDomainContext context;
	LocalVector<EditorEditDomainProvider *> available;
	registry.get_available_providers(context, available);
	CHECK(available.size() == 2);
	bool found_primary = false;
	bool found_activator = false;
	for (EditorEditDomainProvider *provider : available) {
		found_primary |= provider == &primary;
		found_activator |= provider == &activator;
	}
	CHECK(found_primary);
	CHECK(found_activator);
	CHECK(registry.find_double_click_provider(context, ObjectID()) == &activator);

	CHECK(registry.unregister_provider(primary.get_domain_id(), &primary));
	CHECK(registry.get_provider(primary.get_domain_id()) == nullptr);
	CHECK(registry.unregister_provider(unavailable.get_domain_id(), &unavailable));
	CHECK(registry.unregister_provider(activator.get_domain_id(), &activator));
}

TEST_CASE("[Editor][EditorEditDomain] Hosts own independent sessions per view") {
	EditorEditDomainRegistry *registry = EditorEditDomainRegistry::get_singleton();
	REQUIRE(registry != nullptr);
	SessionStats stats;
	TestDomainProvider provider(SNAME("test_domain_independent_hosts"), &stats);
	REQUIRE(registry->register_provider(&provider));

	int view_a_token = 0;
	int view_b_token = 0;
	Node3DEditorView *view_a = reinterpret_cast<Node3DEditorView *>(&view_a_token);
	Node3DEditorView *view_b = reinterpret_cast<Node3DEditorView *>(&view_b_token);
	{
		EditorEditDomainContext context_a;
		context_a.view = view_a;
		EditorEditDomainContext context_b;
		context_b.view = view_b;
		EditorEditDomainHost host_a;
		EditorEditDomainHost host_b;
		host_a.set_context(context_a);
		host_b.set_context(context_b);

		REQUIRE(host_a.enter_domain(provider.get_domain_id(), nullptr));
		REQUIRE(host_b.enter_domain(provider.get_domain_id(), nullptr));
		CHECK(host_a.get_active_session() != host_b.get_active_session());
		CHECK(static_cast<TestDomainSession *>(host_a.get_active_session())->entered_view == view_a);
		CHECK(static_cast<TestDomainSession *>(host_b.get_active_session())->entered_view == view_b);
		CHECK(stats.created == 2);
		CHECK(stats.entered == 2);

		host_a.exit_domain();
		CHECK_FALSE(host_a.is_active());
		CHECK(host_b.is_active());
		CHECK(stats.exited == 1);
		CHECK(stats.destroyed == 1);
	}
	CHECK(stats.exited == 2);
	CHECK(stats.destroyed == 2);
	CHECK(registry->unregister_provider(provider.get_domain_id(), &provider));
}

TEST_CASE("[Editor][EditorEditDomain] Provider removal notification exits the active session") {
	EditorEditDomainRegistry *registry = EditorEditDomainRegistry::get_singleton();
	REQUIRE(registry != nullptr);
	SessionStats stats;
	TestDomainProvider provider(SNAME("test_domain_provider_removal"), &stats);
	REQUIRE(registry->register_provider(&provider));

	EditorEditDomainHost host;
	REQUIRE(host.enter_domain(provider.get_domain_id(), nullptr));
	CHECK(host.is_active());
	CHECK(registry->unregister_provider(provider.get_domain_id(), &provider));
	host.notify_provider_unregistered(&provider);
	CHECK_FALSE(host.is_active());
	CHECK(host.get_active_session() == nullptr);
	CHECK(stats.exited == 1);
	CHECK(stats.destroyed == 1);
}

TEST_CASE("[Editor][EditorEditDomain] Tri-state routing preserves native navigation events") {
	EditorEditDomainRegistry *registry = EditorEditDomainRegistry::get_singleton();
	REQUIRE(registry != nullptr);
	SessionStats stats;
	TestDomainProvider provider(SNAME("test_domain_tri_state"), &stats, true, false, EditorEditDomainInput::PASS_TO_VIEWPORT, true);
	REQUIRE(registry->register_provider(&provider));

	EditorEditDomainHost host;
	REQUIRE(host.enter_domain(provider.get_domain_id(), nullptr));
	TestDomainSession *session = static_cast<TestDomainSession *>(host.get_active_session());
	Ref<InputEventAction> generic_event;
	generic_event.instantiate();

	session->programmed_result = EditorEditDomainInput::PASS_TO_VIEWPORT;
	CHECK(host.route_input(nullptr, nullptr, generic_event) == EditorEditDomainInput::PASS_TO_VIEWPORT);
	session->programmed_result = EditorEditDomainInput::BLOCK_NATIVE_EDIT;
	CHECK(host.route_input(nullptr, nullptr, generic_event) == EditorEditDomainInput::BLOCK_NATIVE_EDIT);
	session->programmed_result = EditorEditDomainInput::CONSUMED;
	CHECK(host.route_input(nullptr, nullptr, generic_event) == EditorEditDomainInput::CONSUMED);

	CHECK(_route_mouse_button(host, MouseButton::MIDDLE) == EditorEditDomainInput::PASS_TO_VIEWPORT);
	CHECK(_route_mouse_button(host, MouseButton::WHEEL_UP) == EditorEditDomainInput::PASS_TO_VIEWPORT);
	CHECK(_route_mouse_button(host, MouseButton::RIGHT) == EditorEditDomainInput::PASS_TO_VIEWPORT);
	CHECK(_route_mouse_button(host, MouseButton::LEFT, true) == EditorEditDomainInput::PASS_TO_VIEWPORT);

	host.exit_domain();
	CHECK(registry->unregister_provider(provider.get_domain_id(), &provider));
}

TEST_CASE("[Editor][EditorEditDomain] Escape cancels then exits and Tab toggles") {
	EditorEditDomainRegistry *registry = EditorEditDomainRegistry::get_singleton();
	REQUIRE(registry != nullptr);
	SessionStats stats;
	TestDomainProvider provider(SNAME("test_domain_escape_tab"), &stats);
	REQUIRE(registry->register_provider(&provider));

	EditorEditDomainHost host;
	REQUIRE(host.enter_domain(provider.get_domain_id(), nullptr));
	TestDomainSession *session = static_cast<TestDomainSession *>(host.get_active_session());
	session->gesture_active = true;
	Ref<InputEventKey> escape = _make_key_event(Key::ESCAPE);
	CHECK(host.route_input(nullptr, nullptr, escape) == EditorEditDomainInput::CONSUMED);
	CHECK(host.is_active());
	CHECK_FALSE(session->gesture_active);
	CHECK(stats.escape_calls == 1);
	CHECK(host.route_input(nullptr, nullptr, escape) == EditorEditDomainInput::CONSUMED);
	CHECK_FALSE(host.is_active());
	CHECK(stats.exited == 1);
	CHECK(stats.destroyed == 1);

	REQUIRE(host.enter_domain(provider.get_domain_id(), nullptr));
	session = static_cast<TestDomainSession *>(host.get_active_session());
	Ref<InputEventKey> tab = _make_key_event(Key::TAB);
	CHECK(host.route_input(nullptr, nullptr, tab) == EditorEditDomainInput::CONSUMED);
	CHECK(host.is_active());
	CHECK(session->tool_toggled);
	CHECK(stats.toggle_calls == 1);

	host.exit_domain();
	CHECK(registry->unregister_provider(provider.get_domain_id(), &provider));
}

TEST_CASE("[Editor][EditorEditDomain] Contextual controls mount in center slots and are freed on exit") {
	EditorEditDomainRegistry *registry = EditorEditDomainRegistry::get_singleton();
	REQUIRE(registry != nullptr);
	SessionStats stats;
	TestDomainProvider provider(SNAME("test_domain_contextual_controls"), &stats, true, false, EditorEditDomainInput::PASS_TO_VIEWPORT, false, true);
	REQUIRE(registry->register_provider(&provider));

	EditorViewportChrome *chrome = memnew(EditorViewportChrome(SNAME("test"), EditorViewportChrome::SCOPE_VIEW));
	EditorEditDomainHost host;
	host.set_chrome_host(chrome);
	REQUIRE(host.enter_domain(provider.get_domain_id(), nullptr));
	CHECK(stats.rail_builds == 1);
	CHECK(stats.panel_builds == 1);

	Control *rail = Object::cast_to<Control>(ObjectDB::get_instance(stats.rail_id));
	Control *panel = Object::cast_to<Control>(ObjectDB::get_instance(stats.panel_id));
	REQUIRE(rail != nullptr);
	REQUIRE(panel != nullptr);
	REQUIRE(rail->get_parent() != nullptr);
	REQUIRE(panel->get_parent() != nullptr);
	CHECK(rail->get_parent()->get_parent() == chrome);
	CHECK(panel->get_parent()->get_parent() == chrome);
	CHECK(rail->get_parent() != panel->get_parent());

	host.exit_domain();
	CHECK(rail->get_parent() == nullptr);
	CHECK(panel->get_parent() == nullptr);
	CHECK(rail->is_queued_for_deletion());
	CHECK(panel->is_queued_for_deletion());
	SceneTree::get_singleton()->process(0);
	CHECK(ObjectDB::get_instance(stats.rail_id) == nullptr);
	CHECK(ObjectDB::get_instance(stats.panel_id) == nullptr);

	CHECK(registry->unregister_provider(provider.get_domain_id(), &provider));
	host.set_chrome_host(nullptr);
	memdelete(chrome);
}

} // namespace TestEditorEditDomain

#endif // TOOLS_ENABLED
