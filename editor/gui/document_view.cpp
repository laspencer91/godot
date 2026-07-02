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
#include "editor/scene/3d/node_3d_editor_plugin.h"
#include "editor/scene/canvas_item_editor_plugin.h"

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

	// Host the editor surface for this document's kind, pointed at THIS document's isolated
	// world so the pane renders p_document's scene independently of the globally-active one.
	// 2D scenes get a CanvasItemEditorView (the real 2D editor view, minted per-document); 3D (and
	// mixed/unknown, until the ⑤b 2D/3D toggle) get a Node3DEditorView. Symmetric factory calls.
	const EditorDocument::Type type = p_document ? p_document->get_type() : EditorDocument::TYPE_UNKNOWN;
	if (type == EditorDocument::TYPE_SCENE_2D) {
		CanvasItemEditor *canvas_editor = CanvasItemEditor::get_singleton();
		if (canvas_editor) {
			editor_surface = canvas_editor->create_view_bound_to(p_document);
			if (editor_surface) {
				add_child(editor_surface);
				editor_surface->set_h_size_flags(SIZE_EXPAND_FILL);
				editor_surface->set_v_size_flags(SIZE_EXPAND_FILL);
			}
		}
	} else {
		Node3DEditor *spatial = Node3DEditor::get_singleton();
		if (spatial) {
			editor_surface = spatial->create_view_bound_to(p_document ? p_document->get_world_3d() : Ref<World3D>());
			if (editor_surface) {
				add_child(editor_surface);
				editor_surface->set_h_size_flags(SIZE_EXPAND_FILL);
				editor_surface->set_v_size_flags(SIZE_EXPAND_FILL);
			}
		}
	}
}

DocumentView::~DocumentView() {
	// editor_surface is a child Control freed by the scene tree; doc_view is a plain
	// C++ object we own, so free it here.
	if (doc_view) {
		memdelete(doc_view);
		doc_view = nullptr;
	}
}
