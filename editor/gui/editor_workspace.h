/**************************************************************************/
/*  editor_workspace.h                                                    */
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

#include "scene/gui/box_container.h"

class EditorWorkspace;
class SplitContainer;

// Workspace layer for the dividable multi-document editor (G2).
//
// EditorWorkspace replaces the single fixed main-screen area with a tree of
// WorkspacePanes. A WorkspacePane is one region of the workspace and is either:
//   - a LEAF, hosting a single content Control (in v1: the shared main-screen
//     editor stack; later: per-pane tabs of DocumentViews), or
//   - a SPLIT, hosting a SplitContainer that divides the region between two
//     child WorkspacePanes (recursively).
// This increment adds the split mechanic (the tree can now divide) on top of the
// earlier shell; content hosting is still the shared main-screen stack, so real
// per-pane editors arrive later once the 3D/2D editors are instanceable.

class WorkspacePane : public VBoxContainer {
	GDCLASS(WorkspacePane, VBoxContainer);

	// The owning workspace, so a pane can report focus and mint child panes that
	// are also wired back to it. Set by EditorWorkspace when the pane is created.
	EditorWorkspace *workspace = nullptr;

	// LEAF state: the content this pane displays (fills the pane), reparented in
	// via set_content(). Null while this pane is a SPLIT.
	Control *content = nullptr;

	// SPLIT state: the divider and the two child panes. All null while this pane
	// is a LEAF. `first` is the left/top child, `second` the right/bottom child.
	SplitContainer *split_container = nullptr;
	WorkspacePane *first = nullptr;
	WorkspacePane *second = nullptr;

protected:
	static void _bind_methods() {}

public:
	void set_workspace(EditorWorkspace *p_workspace) { workspace = p_workspace; }
	EditorWorkspace *get_workspace() const { return workspace; }

	bool is_leaf() const { return split_container == nullptr; }

	void set_content(Control *p_content);
	Control *get_content() const { return content; }

	// Divide this LEAF pane in two. The current content stays on one side and
	// p_new_content is placed on the other (on the second/bottom-right side when
	// p_new_on_second is true). p_vertical stacks the children top/bottom; false
	// places them side by side. Returns the child pane that holds p_new_content,
	// or nullptr if this pane is not a leaf.
	WorkspacePane *split(bool p_vertical, Control *p_new_content, bool p_new_on_second = true);

	WorkspacePane *get_first() const { return first; }
	WorkspacePane *get_second() const { return second; }

	WorkspacePane();
};

class EditorWorkspace : public VBoxContainer {
	GDCLASS(EditorWorkspace, VBoxContainer);

	WorkspacePane *root_pane = nullptr;
	WorkspacePane *focused_pane = nullptr;

	// Running counter so temporary debug placeholder panes get distinct labels.
	int debug_pane_counter = 0;

protected:
	static void _bind_methods() {}
	void _notification(int p_what);
	virtual void unhandled_key_input(const Ref<InputEvent> &p_event) override;

	// TEMPORARY (G2 scaffolding): split the focused pane, dropping a labeled
	// placeholder into the new side, so the split mechanic can be exercised
	// before real per-pane content exists. Removed once tabs host real content.
	void _debug_split_focused(bool p_vertical);

	// TEMPORARY (G2 scaffolding): split the focused pane and drop a DocumentView for a
	// DIFFERENT open document into the new side, so two panes render two different live
	// documents at once. Exercises the per-pane DocumentView/EditorDocumentView path
	// before real tabs + drag-to-open exist. Removed once tabs host DocumentViews.
	void _debug_split_focused_with_document(bool p_vertical);

public:
	WorkspacePane *make_pane();

	WorkspacePane *get_root_pane() const { return root_pane; }
	WorkspacePane *get_focused_pane() const { return focused_pane ? focused_pane : root_pane; }
	void set_focused_pane(WorkspacePane *p_pane) { focused_pane = p_pane; }

	EditorWorkspace();
};
