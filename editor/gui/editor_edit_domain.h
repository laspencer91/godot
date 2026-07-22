/**************************************************************************/
/*  editor_edit_domain.h                                                  */
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

#include "core/object/object.h"
#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"

class Camera3D;
class Control;
class EditorDocument;
class InputEvent;
class Node3DEditorView;
class Node3DEditorViewport;
template <typename T>
class Ref;

// CSG-3B: Tri-state arbitration result. Aggregation precedence is consumed,
// then block native editing, then pass through to the viewport.
enum class EditorEditDomainInput {
	PASS_TO_VIEWPORT,
	BLOCK_NATIVE_EDIT,
	CONSUMED,
};

// CSG-3B: Typed, borrowed context shared by providers, sessions, and the host.
// Providers and sessions must not retain or own any of these pointers.
struct EditorEditDomainContext {
	Node3DEditorView *view = nullptr;
	EditorDocument *document = nullptr;
	Node3DEditorViewport *active_viewport = nullptr;
};

// CSG-3B: One pane-local session. Concrete domains own their tool state,
// gestures, overlays, and contextual controls here.
class EditorEditDomainSession : public Object {
	GDCLASS(EditorEditDomainSession, Object);

protected:
	static void _bind_methods() {}

public:
	virtual void enter(const EditorEditDomainContext &p_context) {}
	virtual void exit() {}
	virtual void retarget(const EditorEditDomainContext &p_context) {}

	virtual EditorEditDomainInput handle_input(const EditorEditDomainContext &p_context, Camera3D *p_camera, const Ref<InputEvent> &p_event) { return EditorEditDomainInput::PASS_TO_VIEWPORT; }
	virtual bool handle_escape() { return false; }
	virtual bool handle_tool_toggle() { return false; }
	virtual void draw_overlay(Node3DEditorViewport *p_viewport) {}

	virtual Control *build_tool_rail() { return nullptr; }
	virtual Control *build_contextual_panel() { return nullptr; }

	virtual ~EditorEditDomainSession() {}
};

// CSG-3B: Stateless factory borrowed by the registry. Sessions never retain
// the provider that created them.
class EditorEditDomainProvider {
public:
	virtual StringName get_domain_id() const = 0;
	virtual bool is_available(const EditorEditDomainContext &p_context) const = 0;
	virtual bool can_activate_from_double_click(const EditorEditDomainContext &p_context, ObjectID p_hit) const { return false; }
	virtual EditorEditDomainSession *create_session(const EditorEditDomainContext &p_context) const = 0;

	virtual ~EditorEditDomainProvider() {}
};

class EditorEditDomainRegistry {
	static EditorEditDomainRegistry *singleton;

	// Providers are borrowed. Owners unregister before destroying them.
	HashMap<StringName, EditorEditDomainProvider *> providers;

public:
	static void create();
	static void free();
	static EditorEditDomainRegistry *get_singleton() { return singleton; }

	bool register_provider(EditorEditDomainProvider *p_provider);
	bool unregister_provider(const StringName &p_domain_id, EditorEditDomainProvider *p_provider = nullptr);
	EditorEditDomainProvider *get_provider(const StringName &p_domain_id) const;
	EditorEditDomainProvider *find_double_click_provider(const EditorEditDomainContext &p_context, ObjectID p_hit) const;
	void get_available_providers(const EditorEditDomainContext &p_context, LocalVector<EditorEditDomainProvider *> &r_out) const;
};

// CSG-3B: One plain heap-owned host per pane-level 3D surface.
class EditorEditDomainHost {
	EditorEditDomainContext context;
	Control *chrome_host = nullptr;
	StringName active_domain_id;
	EditorEditDomainProvider *active_provider = nullptr;
	EditorEditDomainSession *active_session = nullptr;
	Control *mounted_rail = nullptr;
	Control *mounted_panel = nullptr;

	void _mount_chrome_controls();
	void _unmount_chrome_controls();

public:
	void set_context(const EditorEditDomainContext &p_context) { context = p_context; }
	void set_chrome_host(Control *p_chrome) { chrome_host = p_chrome; }

	bool is_active() const { return active_session != nullptr; }
	StringName get_active_domain_id() const { return active_domain_id; }
	EditorEditDomainSession *get_active_session() const { return active_session; }

	bool enter_domain(const StringName &p_domain_id, Node3DEditorViewport *p_viewport);
	void exit_domain();
	bool try_activate_from_double_click(Node3DEditorViewport *p_viewport, ObjectID p_hit);

	EditorEditDomainInput route_input(Node3DEditorViewport *p_viewport, Camera3D *p_camera, const Ref<InputEvent> &p_event);
	void route_draw(Node3DEditorViewport *p_viewport);

	void notify_provider_unregistered(EditorEditDomainProvider *p_provider);

	~EditorEditDomainHost();
};

#ifdef DEV_ENABLED
void register_editor_edit_domain_dev_providers();
void unregister_editor_edit_domain_dev_providers();
#endif
