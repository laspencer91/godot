/**************************************************************************/
/*  editor_viewport_chrome.h                                              */
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

#pragma once

#include "core/object/ref_counted.h"
#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "scene/gui/box_container.h"
#include "scene/gui/container.h"

class EditorViewportChrome;

class EditorViewportChromeRegistration : public RefCounted {
	GDCLASS(EditorViewportChromeRegistration, RefCounted);

	friend class EditorViewportChromeRegistry;

	StringName editor_id;
	int scope = 0;
	int slot = 0;
	Callable factory;
	int order = 0;
	bool registered = false;
	HashMap<ObjectID, ObjectID> controls;

protected:
	static void _bind_methods();

public:
	void unregister();
	bool is_registered() const { return registered; }

	~EditorViewportChromeRegistration();
};

class EditorViewportChromeRegistry {
	static EditorViewportChromeRegistry *singleton;

	LocalVector<EditorViewportChrome *> chromes;
	LocalVector<EditorViewportChromeRegistration *> registrations;

	void _attach(EditorViewportChromeRegistration *p_registration, EditorViewportChrome *p_chrome);
	void _detach(EditorViewportChromeRegistration *p_registration, EditorViewportChrome *p_chrome, bool p_free_control);

public:
	static void create();
	static void free();
	static EditorViewportChromeRegistry *get_singleton() { return singleton; }

	Ref<EditorViewportChromeRegistration> register_control_factory(const StringName &p_editor_id, int p_scope, int p_slot, const Callable &p_factory, int p_order);
	void unregister_registration(EditorViewportChromeRegistration *p_registration);
	void add_chrome(EditorViewportChrome *p_chrome);
	void remove_chrome(EditorViewportChrome *p_chrome);
};

class EditorViewportChrome : public Container {
	GDCLASS(EditorViewportChrome, Container);

	friend class EditorViewportChromeRegistry;

public:
	enum Scope {
		SCOPE_VIEW,
		SCOPE_SUBVIEWPORT,
	};

	enum Slot {
		SLOT_TOP_LEFT,
		SLOT_TOP_CENTER,
		SLOT_TOP_RIGHT,
		SLOT_BOTTOM_LEFT,
		SLOT_BOTTOM_CENTER,
		SLOT_BOTTOM_RIGHT,
		SLOT_CENTER_LEFT,
		SLOT_CENTER_RIGHT,
		SLOT_MAX,
	};

private:
	StringName editor_id;
	Scope scope = SCOPE_VIEW;
	Dictionary context;
	VBoxContainer *slots[SLOT_MAX] = {};
	HashMap<ObjectID, int> control_orders[SLOT_MAX];
	int safe_area[4] = {};
	bool registry_active = false;

	void _layout_slots();

protected:
	void _notification(int p_what);
	static void _bind_methods() {}

public:
	const StringName &get_editor_id() const { return editor_id; }
	Scope get_scope() const { return scope; }
	const Dictionary &get_context() const { return context; }

	void add_control(Slot p_slot, Control *p_control, int p_order = 0);
	void remove_control(Control *p_control);
	void set_safe_area_insets(int p_left, int p_top, int p_right, int p_bottom);
	void activate();

	EditorViewportChrome(const StringName &p_editor_id, Scope p_scope, const Dictionary &p_context = Dictionary());
	~EditorViewportChrome();
};
