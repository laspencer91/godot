/**************************************************************************/
/*  editor_viewport_chrome.cpp                                            */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                          */
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
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY    */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "editor_viewport_chrome.h"

#include "core/object/class_db.h"
#include "editor/themes/editor_scale.h"

EditorViewportChromeRegistry *EditorViewportChromeRegistry::singleton = nullptr;

void EditorViewportChromeRegistration::_bind_methods() {
	ClassDB::bind_method(D_METHOD("unregister"), &EditorViewportChromeRegistration::unregister);
	ClassDB::bind_method(D_METHOD("is_registered"), &EditorViewportChromeRegistration::is_registered);
}

void EditorViewportChromeRegistration::unregister() {
	if (!registered) {
		return;
	}
	if (EditorViewportChromeRegistry::get_singleton()) {
		EditorViewportChromeRegistry::get_singleton()->unregister_registration(this);
	} else {
		registered = false;
		controls.clear();
	}
}

EditorViewportChromeRegistration::~EditorViewportChromeRegistration() {
	unregister();
}

void EditorViewportChromeRegistry::create() {
	ERR_FAIL_COND(singleton != nullptr);
	singleton = memnew(EditorViewportChromeRegistry);
}

void EditorViewportChromeRegistry::free() {
	if (!singleton) {
		return;
	}
	while (singleton->registrations.size() > 0) {
		singleton->registrations[singleton->registrations.size() - 1]->unregister();
	}
	memdelete(singleton);
	singleton = nullptr;
}

Ref<EditorViewportChromeRegistration> EditorViewportChromeRegistry::register_control_factory(const StringName &p_editor_id, int p_scope, int p_slot, const Callable &p_factory, int p_order) {
	ERR_FAIL_COND_V(p_editor_id.is_empty(), Ref<EditorViewportChromeRegistration>());
	ERR_FAIL_INDEX_V(p_scope, int(EditorViewportChrome::SCOPE_SUBVIEWPORT) + 1, Ref<EditorViewportChromeRegistration>());
	ERR_FAIL_INDEX_V(p_slot, int(EditorViewportChrome::SLOT_MAX), Ref<EditorViewportChromeRegistration>());
	ERR_FAIL_COND_V(!p_factory.is_valid(), Ref<EditorViewportChromeRegistration>());
	ERR_FAIL_COND_V_MSG(p_order < 0, Ref<EditorViewportChromeRegistration>(), "Negative viewport chrome order values are reserved for built-in editor controls.");

	Ref<EditorViewportChromeRegistration> registration;
	registration.instantiate();
	registration->editor_id = p_editor_id;
	registration->scope = p_scope;
	registration->slot = p_slot;
	registration->factory = p_factory;
	registration->order = p_order;
	registration->registered = true;
	registrations.push_back(registration.ptr());

	for (EditorViewportChrome *chrome : chromes) {
		_attach(registration.ptr(), chrome);
	}
	return registration;
}

void EditorViewportChromeRegistry::_attach(EditorViewportChromeRegistration *p_registration, EditorViewportChrome *p_chrome) {
	if (!p_registration->registered || p_registration->editor_id != p_chrome->get_editor_id() || p_registration->scope != p_chrome->get_scope()) {
		return;
	}

	const ObjectID chrome_id = p_chrome->get_instance_id();
	if (p_registration->controls.has(chrome_id)) {
		return;
	}

	Variant context = p_chrome->get_context();
	const Variant *args[1] = { &context };
	Callable::CallError call_error;
	Variant result;
	p_registration->factory.callp(args, 1, result, call_error);
	if (call_error.error != Callable::CallError::CALL_OK) {
		ERR_PRINT(vformat("Failed to create viewport chrome control: %s", Variant::get_callable_error_text(p_registration->factory, args, 1, call_error)));
		return;
	}

	Object *object = result;
	Control *control = Object::cast_to<Control>(object);
	if (!control) {
		ERR_PRINT("Viewport chrome control factory must return a Control.");
		return;
	}
	if (control->get_parent()) {
		ERR_PRINT("Viewport chrome control factory returned a Control that already has a parent.");
		return;
	}

	p_chrome->add_control(EditorViewportChrome::Slot(p_registration->slot), control, p_registration->order);
	p_registration->controls.insert(chrome_id, control->get_instance_id());
}

void EditorViewportChromeRegistry::_detach(EditorViewportChromeRegistration *p_registration, EditorViewportChrome *p_chrome, bool p_free_control) {
	const ObjectID chrome_id = p_chrome->get_instance_id();
	const ObjectID *control_id = p_registration->controls.getptr(chrome_id);
	if (!control_id) {
		return;
	}

	Control *control = Object::cast_to<Control>(ObjectDB::get_instance(*control_id));
	p_registration->controls.erase(chrome_id);
	if (!control || !p_free_control) {
		return;
	}

	p_chrome->remove_control(control);
	control->queue_free();
}

void EditorViewportChromeRegistry::unregister_registration(EditorViewportChromeRegistration *p_registration) {
	if (!p_registration->registered) {
		return;
	}
	for (EditorViewportChrome *chrome : chromes) {
		_detach(p_registration, chrome, true);
	}
	p_registration->registered = false;
	p_registration->controls.clear();
	registrations.erase(p_registration);
}

void EditorViewportChromeRegistry::add_chrome(EditorViewportChrome *p_chrome) {
	ERR_FAIL_NULL(p_chrome);
	ERR_FAIL_COND(chromes.find(p_chrome) >= 0);
	chromes.push_back(p_chrome);
	for (EditorViewportChromeRegistration *registration : registrations) {
		_attach(registration, p_chrome);
	}
}

void EditorViewportChromeRegistry::remove_chrome(EditorViewportChrome *p_chrome) {
	ERR_FAIL_NULL(p_chrome);
	for (EditorViewportChromeRegistration *registration : registrations) {
		_detach(registration, p_chrome, false);
	}
	chromes.erase(p_chrome);
}

void EditorViewportChrome::_layout_slots() {
	const Size2 available_size = get_size();
	for (int i = 0; i < SLOT_MAX; i++) {
		VBoxContainer *slot = slots[i];
		const Size2 minimum_size = slot->get_combined_minimum_size();
		Vector2 position;

		switch (i) {
			case SLOT_TOP_LEFT:
				position = Vector2(safe_area[SIDE_LEFT], safe_area[SIDE_TOP]);
				break;
			case SLOT_TOP_CENTER:
				position = Vector2((available_size.x - minimum_size.x) * 0.5, safe_area[SIDE_TOP]);
				break;
			case SLOT_TOP_RIGHT:
				position = Vector2(available_size.x - safe_area[SIDE_RIGHT] - minimum_size.x, safe_area[SIDE_TOP]);
				break;
			case SLOT_BOTTOM_LEFT:
				position = Vector2(safe_area[SIDE_LEFT], available_size.y - safe_area[SIDE_BOTTOM] - minimum_size.y);
				break;
			case SLOT_BOTTOM_CENTER:
				position = Vector2((available_size.x - minimum_size.x) * 0.5, available_size.y - safe_area[SIDE_BOTTOM] - minimum_size.y);
				break;
			case SLOT_BOTTOM_RIGHT:
				position = Vector2(available_size.x - safe_area[SIDE_RIGHT] - minimum_size.x, available_size.y - safe_area[SIDE_BOTTOM] - minimum_size.y);
				break;
			case SLOT_CENTER_LEFT:
				position = Vector2(safe_area[SIDE_LEFT], (available_size.y - minimum_size.y) * 0.5);
				break;
			case SLOT_CENTER_RIGHT:
				position = Vector2(available_size.x - safe_area[SIDE_RIGHT] - minimum_size.x, (available_size.y - minimum_size.y) * 0.5);
				break;
			default:
				break;
		}

		fit_child_in_rect(slot, Rect2(position.max(Vector2()), minimum_size));
	}
}

void EditorViewportChrome::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_SORT_CHILDREN:
			_layout_slots();
			break;
		case NOTIFICATION_THEME_CHANGED:
			queue_sort();
			break;
	}
}

void EditorViewportChrome::add_control(Slot p_slot, Control *p_control, int p_order) {
	ERR_FAIL_INDEX(p_slot, SLOT_MAX);
	ERR_FAIL_NULL(p_control);
	ERR_FAIL_COND_MSG(p_control->get_parent(), "Viewport chrome controls must not already have a parent.");

	VBoxContainer *slot = slots[p_slot];
	int insertion_index = slot->get_child_count();
	for (int i = 0; i < slot->get_child_count(); i++) {
		Control *existing = Object::cast_to<Control>(slot->get_child(i));
		if (!existing) {
			continue;
		}
		const int *existing_order = control_orders[p_slot].getptr(existing->get_instance_id());
		if (existing_order && p_order < *existing_order) {
			insertion_index = i;
			break;
		}
	}

	slot->add_child(p_control);
	control_orders[p_slot].insert(p_control->get_instance_id(), p_order);
	if (insertion_index < slot->get_child_count() - 1) {
		slot->move_child(p_control, insertion_index);
	}
	queue_sort();
}

void EditorViewportChrome::remove_control(Control *p_control) {
	ERR_FAIL_NULL(p_control);
	for (int i = 0; i < SLOT_MAX; i++) {
		if (p_control->get_parent() == slots[i]) {
			control_orders[i].erase(p_control->get_instance_id());
			slots[i]->remove_child(p_control);
			queue_sort();
			return;
		}
	}
}

void EditorViewportChrome::set_safe_area_insets(int p_left, int p_top, int p_right, int p_bottom) {
	safe_area[SIDE_LEFT] = MAX(0, p_left);
	safe_area[SIDE_TOP] = MAX(0, p_top);
	safe_area[SIDE_RIGHT] = MAX(0, p_right);
	safe_area[SIDE_BOTTOM] = MAX(0, p_bottom);
	queue_sort();
}

EditorViewportChrome::EditorViewportChrome(const StringName &p_editor_id, Scope p_scope, const Dictionary &p_context) {
	editor_id = p_editor_id;
	scope = p_scope;
	context = p_context;
	context["editor_id"] = editor_id;
	context["scope"] = int(scope);

	set_mouse_filter(MOUSE_FILTER_IGNORE);
	set_anchors_and_offsets_preset(PRESET_FULL_RECT);
	set_safe_area_insets(10 * EDSCALE, 10 * EDSCALE, 10 * EDSCALE, 10 * EDSCALE);

	for (int i = 0; i < SLOT_MAX; i++) {
		slots[i] = memnew(VBoxContainer);
		slots[i]->set_mouse_filter(MOUSE_FILTER_IGNORE);
		slots[i]->set_theme_type_variation("ViewportChromeSlot");
		add_child(slots[i]);
	}

}

EditorViewportChrome::~EditorViewportChrome() {
	if (registry_active && EditorViewportChromeRegistry::get_singleton()) {
		EditorViewportChromeRegistry::get_singleton()->remove_chrome(this);
	}
}

void EditorViewportChrome::activate() {
	if (registry_active) {
		return;
	}
	registry_active = true;
	if (EditorViewportChromeRegistry::get_singleton()) {
		EditorViewportChromeRegistry::get_singleton()->add_chrome(this);
	}
}
