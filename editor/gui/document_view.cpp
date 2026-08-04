/**************************************************************************/
/*  document_view.cpp                                                     */
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

#include "document_view.h"

#include "editor/editor_document.h"
#include "editor/gui/editor_document_surface.h"
#include "scene/gui/box_container.h"

DocumentView::DocumentView(EditorDocument *p_document) {
	set_h_size_flags(SIZE_EXPAND_FILL);
	set_v_size_flags(SIZE_EXPAND_FILL);
	add_theme_constant_override("margin_left", 0);
	add_theme_constant_override("margin_right", 0);
	add_theme_constant_override("margin_top", 0);
	add_theme_constant_override("margin_bottom", 0);

	// Bind the model side: this view presents p_document.
	doc_view = memnew(EditorDocumentView);
	doc_view->set_document(p_document);

	// G2 S7 (seam #8): a vertical stack so shared chrome (menu strip / find bar) can mount
	// above/below the surface while this tab is current.
	content_vbox = memnew(VBoxContainer);
	content_vbox->set_h_size_flags(SIZE_EXPAND_FILL);
	content_vbox->set_v_size_flags(SIZE_EXPAND_FILL);
	content_vbox->add_theme_constant_override("separation", 0);
	add_child(content_vbox);

	// CSG-3A: Construction routing lives entirely behind the registry. Providers are
	// borrowed only for this call; the returned instance owns every later lifecycle hook.
	EditorDocumentSurfaceRegistry *registry = EditorDocumentSurfaceRegistry::get_singleton();
	EditorDocumentSurfaceProvider *provider = registry ? registry->resolve_default_surface(p_document) : nullptr;
	if (provider) {
		EditorDocumentSurfaceContext context;
		context.document = p_document;
		context.document_view = doc_view;
		context.host_view = this;
		context.chrome_host = content_vbox;
		surface_instance = provider->create(context);
	}

	// Parent + stretch the minted surface identically regardless of kind.
	Control *editor_surface = get_editor_surface();
	if (editor_surface) {
		content_vbox->add_child(editor_surface);
		editor_surface->set_h_size_flags(SIZE_EXPAND_FILL);
		editor_surface->set_v_size_flags(SIZE_EXPAND_FILL);
	}
}

Control *DocumentView::get_editor_surface() const {
	return surface_instance ? surface_instance->get_root_control() : nullptr;
}

EditorDocument *DocumentView::get_document() const {
	return doc_view ? doc_view->get_document() : nullptr;
}

bool DocumentView::save_document() {
	return surface_instance && surface_instance->save();
}

bool DocumentView::save_document_as() {
	return surface_instance && surface_instance->save_as();
}

Control *DocumentView::get_chrome_host() const {
	return content_vbox;
}

SubViewport *DocumentView::get_scene_viewport() const {
	return surface_instance ? surface_instance->get_scene_viewport() : nullptr;
}

Control *DocumentView::get_toolbar_host() const {
	return surface_instance ? surface_instance->get_toolbar_host() : nullptr;
}

bool DocumentView::is_scene_view() const {
	return surface_instance && surface_instance->is_scene_view();
}

bool DocumentView::is_scene_view_2d() const {
	return surface_instance && surface_instance->is_scene_view_2d();
}

bool DocumentView::set_scene_view_2d(bool p_2d) {
	return surface_instance && surface_instance->set_scene_view_2d(p_2d);
}

void DocumentView::set_context_active(bool p_active) {
	if (context_active == p_active) {
		return;
	}
	if (!p_active) {
		capture_editor_state();
	}
	context_active = p_active;
	if (doc_view) {
		doc_view->set_active(p_active);
	}
	if (surface_instance) {
		surface_instance->set_context_active(p_active);
	}
	if (p_active) {
		apply_editor_state();
	}
}

void DocumentView::capture_editor_state() {
	if (doc_view && surface_instance) {
		surface_instance->capture_view_state(doc_view->get_editor_states());
	}
}

void DocumentView::apply_editor_state() {
	if (doc_view && surface_instance) {
		surface_instance->apply_view_state(doc_view->get_editor_states());
	}
}

void DocumentView::notify_surface_closing() {
	if (surface_instance) {
		surface_instance->notify_surface_closing();
	}
}

void DocumentView::_notification(int p_what) {
	if (p_what == NOTIFICATION_ENTER_TREE) {
		if (surface_instance) {
			surface_instance->document_view_entered_tree();
		}
		return;
	}
	if (p_what == NOTIFICATION_THEME_CHANGED) {
		if (surface_instance) {
			surface_instance->document_view_theme_changed();
		}
		return;
	}
	if (p_what == NOTIFICATION_READY) {
		if (surface_instance) {
			surface_instance->document_view_ready();
		}
		apply_editor_state();
		return;
	}
	if (p_what != NOTIFICATION_PREDELETE) {
		return;
	}

	if (context_active) {
		set_context_active(false);
	} else {
		capture_editor_state();
	}
	if (surface_instance) {
		// PREDELETE dispatches derived-first, so this runs BEFORE Node's handler frees the children -
		// the last moment the surface is guaranteed alive (the destructor is too late: children are
		// already freed by then). CSG-3A keeps every park/release operation inside this window.
		surface_instance->pre_delete_cleanup();
		memdelete(surface_instance);
		surface_instance = nullptr;
	}
}

DocumentView::~DocumentView() {
	// doc_view is a plain C++ object we own; the surface instance was released during
	// derived-first PREDELETE and Node's handler then freed its remaining root Control.
	if (surface_instance) {
		memdelete(surface_instance);
		surface_instance = nullptr;
	}
	if (doc_view) {
		memdelete(doc_view);
		doc_view = nullptr;
	}
}
