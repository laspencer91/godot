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

#include "core/object/callable_mp.h"
#include "editor/doc/editor_help.h"
#include "editor/editor_document.h"
#include "editor/scene/3d/node_3d_editor_plugin.h"
#include "editor/scene/canvas_item_editor_plugin.h"
#include "editor/script/script_editor_plugin.h"

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
	// mixed/unknown, until the ⑤b 2D/3D toggle) get a Node3DEditorView. Script/help documents get a
	// ScriptTextEditor/EditorHelp view. Symmetric factory calls per kind.
	const EditorDocument::Type type = p_document ? p_document->get_type() : EditorDocument::TYPE_UNKNOWN;
	switch (type) {
		case EditorDocument::TYPE_SCRIPT: {
			// G2 S4: the per-script VIEW is minted by the ScriptEditor SERVICES singleton, fully wired to
			// menus / find-in-files / save-all / debugger. The singleton stays; only the view is per-tab.
			ScriptDocument *sd = static_cast<ScriptDocument *>(p_document);
			if (ScriptEditor *se = ScriptEditor::get_singleton()) {
				editor_surface = se->create_editor_view(sd->get_script_resource());
			}
		} break;
		case EditorDocument::TYPE_HELP: {
			// G2 S4: the view is an EditorHelp. go_to_class needs the view in the tree (theme + doc data),
			// so defer it until after this DocumentView has been parented into its pane.
			HelpDocument *hd = static_cast<HelpDocument *>(p_document);
			EditorHelp *help = memnew(EditorHelp);
			help->set_name(hd->get_class_name());
			callable_mp(help, &EditorHelp::go_to_class).call_deferred(hd->get_class_name());
			editor_surface = help;
		} break;
		case EditorDocument::TYPE_SCREEN_HOST: {
			// G2 S5.5: this view hosts the legacy main-screen stack ITSELF (seam #5). The stack is
			// EditorMainScreen's main_screen_vbox — never owned here; NOTIFICATION_PREDELETE parks it
			// back under the hidden holder so get_control() keeps returning the live vbox (D11).
			ScreenHostDocument *shd = static_cast<ScreenHostDocument *>(p_document);
			Control *stack = shd->get_screen_stack();
			if (stack) {
				if (Node *stack_parent = stack->get_parent()) {
					stack_parent->remove_child(stack); // Un-park (re-summon after a tab close).
				}
				editor_surface = stack;
			}
		} break;
		case EditorDocument::TYPE_SCENE_2D: {
			if (CanvasItemEditor *canvas_editor = CanvasItemEditor::get_singleton()) {
				editor_surface = canvas_editor->create_view_bound_to(p_document);
			}
		} break;
		default: {
			if (Node3DEditor *spatial = Node3DEditor::get_singleton()) {
				editor_surface = spatial->create_view_bound_to(p_document);
			}
		} break;
	}
	// Parent + stretch the minted surface identically regardless of kind.
	if (editor_surface) {
		add_child(editor_surface);
		editor_surface->set_h_size_flags(SIZE_EXPAND_FILL);
		editor_surface->set_v_size_flags(SIZE_EXPAND_FILL);
	}
}

void DocumentView::_notification(int p_what) {
	if (p_what != NOTIFICATION_PREDELETE) {
		return;
	}
	// PREDELETE dispatches derived-first, so this runs BEFORE Node's handler frees the children —
	// the last moment editor_surface is guaranteed alive (the destructor is too late: children are
	// already freed by then).
	if (!editor_surface) {
		return;
	}
	// G2 S4: if this view hosted a script surface, drop it from the ScriptEditor open-scripts
	// registry before it is freed (idempotent belt-and-suspenders with tree_exiting).
	if (ScriptEditorBase *seb = Object::cast_to<ScriptEditorBase>(editor_surface)) {
		if (ScriptEditor *se = ScriptEditor::get_singleton()) {
			se->release_editor_view(seb);
		}
	}
	// G2 S5.5: the screen-host view does not own the legacy main-screen stack — park it back under
	// EditorMainScreen's hidden holder so get_control() stays live (D11). If the holder is already
	// gone (whole-editor teardown, children die last-first), leave the stack to be freed with this
	// view, matching the stock lifetime where the vbox died with the main-screen tree.
	EditorDocument *doc = doc_view ? doc_view->get_document() : nullptr;
	if (doc && doc->get_type() == EditorDocument::TYPE_SCREEN_HOST && editor_surface->get_parent() == this) {
		ScreenHostDocument *shd = static_cast<ScreenHostDocument *>(doc);
		Control *park = Object::cast_to<Control>(ObjectDB::get_instance(shd->get_park_holder_id()));
		if (park) {
			remove_child(editor_surface);
			park->add_child(editor_surface);
			editor_surface = nullptr; // No longer ours; Node's PREDELETE must not free it.
		}
	}
}

DocumentView::~DocumentView() {
	// doc_view is a plain C++ object we own; the children (editor_surface) were already freed by
	// Node's PREDELETE — surface-detach work lives in _notification(NOTIFICATION_PREDELETE) above.
	if (doc_view) {
		memdelete(doc_view);
		doc_view = nullptr;
	}
}
