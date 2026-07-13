/**************************************************************************/
/*  document_view.h                                                       */
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

#include "scene/gui/margin_container.h"

class EditorDocument;
class EditorDocumentView;
class FoldableContainer;
class GroupsDock;
class InspectorDock;
class SceneTreeDock;
class SignalsDock;
class VBoxContainer;

// DocumentView is the per-pane presentation of one open document (G2). A
// WorkspacePane hosts a DocumentView; the DocumentView binds to a specific
// EditorDocument and hosts the editor surface that renders it -- for a scene
// document, a Node3DEditorView pointed at the document's isolated World3D.
//
// This is what makes two panes show two DIFFERENT documents at once: each
// DocumentView renders its own document's world, independent of the globally
// "active" document. It owns the model-side per-pane binding (EditorDocumentView)
// and the editor surface Control.
//
// The surface is a per-document editor view minted by the matching services
// singleton: a Node3DEditorView (3D) or a CanvasItemEditorView (2D, via
// CanvasItemEditor::create_view_bound_to). Selection/gizmos are still global, so a
// non-active DocumentView shows its scene but edits/selection only while active.
class DocumentView : public MarginContainer {
	GDCLASS(DocumentView, MarginContainer);

	// Model-side per-pane binding (owned; not a Node). Holds which document this
	// view presents plus per-pane view state (camera/pan/zoom, active flag).
	EditorDocumentView *doc_view = nullptr;

	// The editor surface rendering the document (a Node3DEditorView in v1). A child
	// Control, so the scene tree frees it with this node.
	Control *editor_surface = nullptr;

	// G2 D7a: for a scene document, its own Scene Tree dock embedded to the left of the surface,
	// bound to this document. Null for non-scene (script/help/resource) views. bound_scene_document
	// is kept only to refresh the tree's root once the view is in-tree (root may load late).
	SceneTreeDock *scene_tree_dock = nullptr;
	// Per-pane Inspector. Scene views drive it from selection; resource views use it as their body.
	InspectorDock *inspector_dock = nullptr;
	// G2 G3: per-pane Signals (ConnectionsDock) + Groups docks, also driven from the doc selection.
	SignalsDock *signals_dock = nullptr;
	GroupsDock *groups_dock = nullptr;
	EditorDocument *bound_scene_document = nullptr;
	// G2 M7.2a: the slot above this scene pane's viewport where the shared 2D/3D toolbar mounts while
	// this pane is focused. Null for non-scene views.
	Control *toolbar_host = nullptr;
	bool context_active = false;

	void _bound_selection_changed();

	// G2 styling: build one accordion dock "card" for p_dock — a styled FoldableContainer (rounded
	// header + leading icon), initial fold state, the fold→expand-flag binding — and add it to p_column.
	// _on_section_folded keeps expanded sections sharing the column while collapsed ones shrink to their
	// header, so folding one frees space for the rest.
	void _add_accordion_section(VBoxContainer *p_column, Control *p_dock, const String &p_title, const StringName &p_icon, bool p_expanded);
	void _on_section_folded(bool p_folded, FoldableContainer *p_section);

	// G2 S7 (seam #8): the vertical stack hosting [shared chrome | surface | find bar]. The
	// ScriptEditor mounts its menu strip / find bar here while this view's tab is current.
	VBoxContainer *content_vbox = nullptr;

protected:
	static void _bind_methods() {}
	void _notification(int p_what);

public:
	EditorDocumentView *get_document_view() const { return doc_view; }
	Control *get_editor_surface() const { return editor_surface; }

	// G2 S7 (seam #8): where the focused tab's shared chrome (ScriptEditor menu strip + find
	// bar) mounts. Null cast target for non-hosting views is fine — callers null-check.
	Control *get_chrome_host() const;

	// G2 M7.2a: the header slot above the viewport where the focused pane's 2D/3D toolbar mounts.
	Control *get_toolbar_host() const { return toolbar_host; }
	void set_context_active(bool p_active);

	DocumentView(EditorDocument *p_document);
	~DocumentView();
};
