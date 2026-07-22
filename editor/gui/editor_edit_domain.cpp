/**************************************************************************/
/*  editor_edit_domain.cpp                                                */
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

#include "editor_edit_domain.h"

#include "core/input/input_event.h"
#include "editor/gui/editor_viewport_chrome.h"

#ifdef DEV_ENABLED
#include "core/string/print_string.h"
#include "editor/scene/3d/node_3d_editor_viewport.h"
#include "scene/gui/box_container.h"
#include "scene/gui/button.h"
#include "scene/gui/label.h"
#include "scene/gui/panel_container.h"
#endif

EditorEditDomainRegistry *EditorEditDomainRegistry::singleton = nullptr;

void EditorEditDomainRegistry::create() {
	ERR_FAIL_COND(singleton != nullptr);
	singleton = memnew(EditorEditDomainRegistry);
}

void EditorEditDomainRegistry::free() {
	if (!singleton) {
		return;
	}
	// The registry borrows providers. Clearing resolution before deleting the
	// registry cannot invalidate sessions, which never retain a provider.
	singleton->providers.clear();
	memdelete(singleton);
	singleton = nullptr;
}

bool EditorEditDomainRegistry::register_provider(EditorEditDomainProvider *p_provider) {
	ERR_FAIL_NULL_V(p_provider, false);
	const StringName domain_id = p_provider->get_domain_id();
	ERR_FAIL_COND_V(domain_id.is_empty(), false);
	ERR_FAIL_COND_V_MSG(providers.has(domain_id), false, vformat("An edit domain provider with ID '%s' is already registered.", domain_id));
	providers.insert(domain_id, p_provider);
	return true;
}

bool EditorEditDomainRegistry::unregister_provider(const StringName &p_domain_id, EditorEditDomainProvider *p_provider) {
	EditorEditDomainProvider **registered = providers.getptr(p_domain_id);
	if (!registered || (p_provider && *registered != p_provider)) {
		return false;
	}
	providers.erase(p_domain_id);
	return true;
}

EditorEditDomainProvider *EditorEditDomainRegistry::get_provider(const StringName &p_domain_id) const {
	EditorEditDomainProvider *const *provider = providers.getptr(p_domain_id);
	return provider ? *provider : nullptr;
}

EditorEditDomainProvider *EditorEditDomainRegistry::find_double_click_provider(const EditorEditDomainContext &p_context, ObjectID p_hit) const {
	for (const KeyValue<StringName, EditorEditDomainProvider *> &E : providers) {
		EditorEditDomainProvider *provider = E.value;
		if (provider->is_available(p_context) && provider->can_activate_from_double_click(p_context, p_hit)) {
			return provider;
		}
	}
	return nullptr;
}

void EditorEditDomainRegistry::get_available_providers(const EditorEditDomainContext &p_context, LocalVector<EditorEditDomainProvider *> &r_out) const {
	for (const KeyValue<StringName, EditorEditDomainProvider *> &E : providers) {
		if (E.value->is_available(p_context)) {
			r_out.push_back(E.value);
		}
	}
}

void EditorEditDomainHost::_mount_chrome_controls() {
	EditorViewportChrome *chrome = Object::cast_to<EditorViewportChrome>(chrome_host);
	if (!chrome || !active_session) {
		return;
	}

	mounted_rail = active_session->build_tool_rail();
	if (mounted_rail) {
		chrome->add_control(EditorViewportChrome::SLOT_CENTER_LEFT, mounted_rail);
	}

	mounted_panel = active_session->build_contextual_panel();
	if (mounted_panel) {
		chrome->add_control(EditorViewportChrome::SLOT_CENTER_RIGHT, mounted_panel);
	}
}

void EditorEditDomainHost::_unmount_chrome_controls() {
	EditorViewportChrome *chrome = Object::cast_to<EditorViewportChrome>(chrome_host);
	if (mounted_rail) {
		if (chrome) {
			chrome->remove_control(mounted_rail);
		}
		mounted_rail->queue_free();
		mounted_rail = nullptr;
	}
	if (mounted_panel) {
		if (chrome) {
			chrome->remove_control(mounted_panel);
		}
		mounted_panel->queue_free();
		mounted_panel = nullptr;
	}
}

bool EditorEditDomainHost::enter_domain(const StringName &p_domain_id, Node3DEditorViewport *p_viewport) {
	EditorEditDomainRegistry *registry = EditorEditDomainRegistry::get_singleton();
	if (!registry) {
		return false;
	}

	EditorEditDomainProvider *provider = registry->get_provider(p_domain_id);
	context.active_viewport = p_viewport;
	if (!provider || !provider->is_available(context)) {
		return false;
	}

	EditorEditDomainSession *session = provider->create_session(context);
	if (!session) {
		return false;
	}

	exit_domain();
	context.active_viewport = p_viewport;
	active_domain_id = p_domain_id;
	active_provider = provider;
	active_session = session;
	active_session->enter(context);
	_mount_chrome_controls();
	return true;
}

void EditorEditDomainHost::exit_domain() {
	if (!active_session) {
		return;
	}

	_unmount_chrome_controls();
	active_session->exit();
	memdelete(active_session);
	active_session = nullptr;
	active_provider = nullptr;
	active_domain_id = StringName();
	context.active_viewport = nullptr;
}

bool EditorEditDomainHost::try_activate_from_double_click(Node3DEditorViewport *p_viewport, ObjectID p_hit) {
	EditorEditDomainRegistry *registry = EditorEditDomainRegistry::get_singleton();
	if (!registry) {
		return false;
	}

	context.active_viewport = p_viewport;
	EditorEditDomainProvider *provider = registry->find_double_click_provider(context, p_hit);
	return provider && enter_domain(provider->get_domain_id(), p_viewport);
}

EditorEditDomainInput EditorEditDomainHost::route_input(Node3DEditorViewport *p_viewport, Camera3D *p_camera, const Ref<InputEvent> &p_event) {
	if (!active_session) {
		return EditorEditDomainInput::PASS_TO_VIEWPORT;
	}

	context.active_viewport = p_viewport;
	Ref<InputEventKey> key_event = p_event;
	if (key_event.is_valid() && key_event->is_pressed() && !key_event->is_echo()) {
		if (key_event->get_keycode() == Key::ESCAPE) {
			if (!active_session->handle_escape()) {
				exit_domain();
			}
			return EditorEditDomainInput::CONSUMED;
		}
		if (key_event->get_keycode() == Key::TAB && active_session->handle_tool_toggle()) {
			return EditorEditDomainInput::CONSUMED;
		}
	}

	return active_session->handle_input(context, p_camera, p_event);
}

void EditorEditDomainHost::route_draw(Node3DEditorViewport *p_viewport) {
	if (!active_session) {
		return;
	}
	context.active_viewport = p_viewport;
	active_session->draw_overlay(p_viewport);
}

void EditorEditDomainHost::notify_provider_unregistered(EditorEditDomainProvider *p_provider) {
	if (p_provider == active_provider) {
		exit_domain();
	}
}

EditorEditDomainHost::~EditorEditDomainHost() {
	exit_domain();
}

#ifdef DEV_ENABLED

class DummyEditDomainSession : public EditorEditDomainSession {
	bool entered = false;
	bool gesture_active = false;
	bool surface_tool = true;

	static bool _has_modifiers(const Ref<InputEventMouseButton> &p_event) {
		return p_event->is_alt_pressed() || p_event->is_shift_pressed() || p_event->is_ctrl_pressed() || p_event->is_meta_pressed();
	}

public:
	virtual void enter(const EditorEditDomainContext &p_context) override {
		entered = true;
		if (p_context.active_viewport) {
			p_context.active_viewport->update_surface();
		}
		print_line("DEV: entered edit domain 'dev_dummy'.");
	}

	virtual void exit() override {
		entered = false;
		gesture_active = false;
		print_line("DEV: exited edit domain 'dev_dummy'.");
	}

	virtual EditorEditDomainInput handle_input(const EditorEditDomainContext &p_context, Camera3D *p_camera, const Ref<InputEvent> &p_event) override {
		if (p_context.active_viewport) {
			Ref<View3DController> controller = p_context.active_viewport->get_controller();
			if (controller.is_valid() && (controller->is_navigating() || controller->cursor.region_select)) {
				return EditorEditDomainInput::PASS_TO_VIEWPORT;
			}
		}

		Ref<InputEventMouseButton> mouse_button = p_event;
		if (mouse_button.is_valid()) {
			const MouseButton button = mouse_button->get_button_index();
			if (button == MouseButton::MIDDLE || button == MouseButton::WHEEL_UP || button == MouseButton::WHEEL_DOWN || button == MouseButton::WHEEL_LEFT || button == MouseButton::WHEEL_RIGHT || button == MouseButton::RIGHT) {
				return EditorEditDomainInput::PASS_TO_VIEWPORT;
			}
			if (button == MouseButton::LEFT) {
				if (_has_modifiers(mouse_button)) {
					return EditorEditDomainInput::PASS_TO_VIEWPORT;
				}
				gesture_active = mouse_button->is_pressed();
				return EditorEditDomainInput::CONSUMED;
			}
		}

		Ref<InputEventMouseMotion> mouse_motion = p_event;
		if (mouse_motion.is_valid() && mouse_motion->get_button_mask().has_flag(MouseButtonMask::LEFT)) {
			gesture_active = true;
			return EditorEditDomainInput::BLOCK_NATIVE_EDIT;
		}

		return EditorEditDomainInput::PASS_TO_VIEWPORT;
	}

	virtual bool handle_escape() override {
		if (!gesture_active) {
			return false;
		}
		gesture_active = false;
		return true;
	}

	virtual bool handle_tool_toggle() override {
		surface_tool = !surface_tool;
		return true;
	}

	virtual void draw_overlay(Node3DEditorViewport *p_viewport) override {
		if (!entered || !p_viewport) {
			return;
		}
		Control *surface = p_viewport->get_surface();
		const Rect2 marker_rect(Point2(18, 18), Size2(196, 38));
		surface->draw_rect(marker_rect, Color(0.08, 0.12, 0.18, 0.88));
		surface->draw_rect(marker_rect, Color(0.35, 0.8, 1.0, 1.0), false, 2.0);
		surface->draw_string(surface->get_theme_default_font(), marker_rect.position + Point2(10, 25), "DEV Edit Domain", HORIZONTAL_ALIGNMENT_LEFT, -1, surface->get_theme_default_font_size(), Color(0.9, 0.96, 1.0));
	}

	virtual Control *build_tool_rail() override {
		VBoxContainer *rail = memnew(VBoxContainer);
		rail->set_name("DevDummyEditDomainToolRail");
		Button *surface_button = memnew(Button);
		surface_button->set_text("Surface");
		rail->add_child(surface_button);
		Button *operand_button = memnew(Button);
		operand_button->set_text("Operand");
		rail->add_child(operand_button);
		return rail;
	}

	virtual Control *build_contextual_panel() override {
		PanelContainer *panel = memnew(PanelContainer);
		panel->set_name("DevDummyEditDomainContextPanel");
		Label *label = memnew(Label);
		label->set_text("DEV Domain Active");
		panel->add_child(label);
		return panel;
	}
};

class DummyEditDomainProvider : public EditorEditDomainProvider {
public:
	virtual StringName get_domain_id() const override { return SNAME("dev_dummy"); }

	virtual bool is_available(const EditorEditDomainContext &p_context) const override {
		return p_context.view != nullptr;
	}

	virtual bool can_activate_from_double_click(const EditorEditDomainContext &p_context, ObjectID p_hit) const override {
		return true;
	}

	virtual EditorEditDomainSession *create_session(const EditorEditDomainContext &p_context) const override {
		return memnew(DummyEditDomainSession);
	}
};

static DummyEditDomainProvider *dev_dummy_edit_domain_provider = nullptr;

void register_editor_edit_domain_dev_providers() {
	EditorEditDomainRegistry *registry = EditorEditDomainRegistry::get_singleton();
	ERR_FAIL_NULL(registry);
	ERR_FAIL_COND(dev_dummy_edit_domain_provider != nullptr);

	dev_dummy_edit_domain_provider = memnew(DummyEditDomainProvider);
	if (!registry->register_provider(dev_dummy_edit_domain_provider)) {
		memdelete(dev_dummy_edit_domain_provider);
		dev_dummy_edit_domain_provider = nullptr;
		ERR_FAIL_MSG("Failed to register the DEV edit domain provider.");
	}
}

void unregister_editor_edit_domain_dev_providers() {
	if (!dev_dummy_edit_domain_provider) {
		return;
	}
	if (EditorEditDomainRegistry *registry = EditorEditDomainRegistry::get_singleton()) {
		registry->unregister_provider(dev_dummy_edit_domain_provider->get_domain_id(), dev_dummy_edit_domain_provider);
	}
	memdelete(dev_dummy_edit_domain_provider);
	dev_dummy_edit_domain_provider = nullptr;
}

#endif // DEV_ENABLED
