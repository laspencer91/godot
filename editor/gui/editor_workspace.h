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

// Workspace layer for the dividable multi-document editor (G2).
//
// EditorWorkspace replaces the single fixed main-screen area with a tree of
// WorkspacePanes. A WorkspacePane is one splittable region that hosts editor
// content (in v1: the shared main-screen editor stack; later: per-pane tabs of
// DocumentViews). This first increment establishes the structure only — a single
// root pane wrapping the existing main-screen area, so behavior is unchanged —
// and gives later increments the seams for splitting, tabs, and drag-drop.

class WorkspacePane : public VBoxContainer {
	GDCLASS(WorkspacePane, VBoxContainer);

	// The content this pane displays (fills the pane). Reparented in via set_content().
	Control *content = nullptr;

protected:
	static void _bind_methods() {}

public:
	void set_content(Control *p_content);
	Control *get_content() const { return content; }

	WorkspacePane();
};

class EditorWorkspace : public VBoxContainer {
	GDCLASS(EditorWorkspace, VBoxContainer);

	WorkspacePane *root_pane = nullptr;
	WorkspacePane *focused_pane = nullptr;

protected:
	static void _bind_methods() {}

public:
	WorkspacePane *get_root_pane() const { return root_pane; }
	WorkspacePane *get_focused_pane() const { return focused_pane; }
	void set_focused_pane(WorkspacePane *p_pane) { focused_pane = p_pane; }

	EditorWorkspace();
};
