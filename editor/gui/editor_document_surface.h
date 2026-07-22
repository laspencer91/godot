/**************************************************************************/
/*  editor_document_surface.h                                             */
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

class Control;
class DocumentView;
class EditorDocument;
class EditorDocumentView;
class SubViewport;

// CSG-3A: Everything a stateless provider needs to mint one pane-owned surface. The
// context is borrowed for construction; providers and instances do not own the document,
// its view state, the DocumentView, or its shared-chrome host.
struct EditorDocumentSurfaceContext {
	EditorDocument *document = nullptr;
	EditorDocumentView *document_view = nullptr;
	DocumentView *host_view = nullptr;
	Control *chrome_host = nullptr;
};

// CSG-3A: A concrete surface lifetime, independent from the provider that created it.
// DocumentView owns the instance, while the returned root Control is owned by the scene
// tree after DocumentView parents it. Provider removal therefore cannot invalidate a live
// pane's cleanup path.
class EditorDocumentSurfaceInstance : public Object {
	GDCLASS(EditorDocumentSurfaceInstance, Object);

	Control *root_control = nullptr;

protected:
	static void _bind_methods() {}
	void set_root_control(Control *p_root) { root_control = p_root; }

public:
	Control *get_root_control() const { return root_control; }

	virtual void set_context_active(bool p_active) {}
	virtual void notify_surface_closing() {}
	virtual void pre_delete_cleanup() {}

	// State that needs a live tree remains split from factory-time restoration.
	virtual void document_view_entered_tree() {}
	virtual void document_view_theme_changed() {}
	virtual void document_view_ready() {}

	// Typed adapters preserve DocumentView's existing scene-facing API without a
	// capability dictionary or knowledge of concrete built-in instance classes.
	virtual SubViewport *get_scene_viewport() const { return nullptr; }
	virtual Control *get_toolbar_host() const { return nullptr; }
	virtual bool is_scene_view() const { return false; }
	virtual bool is_scene_view_2d() const { return false; }
	virtual bool set_scene_view_2d(bool p_2d) { return false; }

	virtual ~EditorDocumentSurfaceInstance() {}
};

// CSG-3A: Providers are stateless and are never retained by the instances they create.
class EditorDocumentSurfaceProvider {
public:
	virtual StringName get_surface_id() const = 0;
	virtual bool supports(EditorDocument *p_document) const = 0;
	virtual EditorDocumentSurfaceInstance *create(const EditorDocumentSurfaceContext &p_context) const = 0;

	virtual ~EditorDocumentSurfaceProvider() {}
};

class EditorDocumentSurfaceRegistry {
	static EditorDocumentSurfaceRegistry *singleton;

	// Providers are borrowed. Owners must unregister before destroying them; instances
	// remain valid because they never keep a provider pointer.
	HashMap<StringName, EditorDocumentSurfaceProvider *> providers;

public:
	static void create();
	static void free();
	static EditorDocumentSurfaceRegistry *get_singleton() { return singleton; }

	bool register_provider(EditorDocumentSurfaceProvider *p_provider);
	bool unregister_provider(const StringName &p_surface_id, EditorDocumentSurfaceProvider *p_provider = nullptr);
	EditorDocumentSurfaceProvider *resolve_surface(const StringName &p_surface_id, EditorDocument *p_document) const;
	StringName get_default_surface_id(EditorDocument *p_document) const;
	EditorDocumentSurfaceProvider *resolve_default_surface(EditorDocument *p_document) const;
};

// Built-ins are registered next to registry creation during editor startup and removed
// before registry destruction. Kept explicit so shutdown ownership is symmetric.
void register_editor_document_surface_providers();
void unregister_editor_document_surface_providers();
