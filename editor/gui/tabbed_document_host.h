/**************************************************************************/
/*  tabbed_document_host.h                                                */
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

#include "core/templates/vector.h"
#include "scene/gui/box_container.h"

class Control;
class DocumentView;
class EditorDocument;
class TabBar;

// TabbedDocumentHost is the tabbed content of one workspace pane (G2): a TabBar
// of open documents over a content area that shows the active document's
// DocumentView. Selecting a tab swaps which document the pane renders; this is
// the first non-debug piece of the multi-document workspace UX.
//
// Ownership/lifecycle: every DocumentView is a child of the content host (so the
// scene tree frees them all), created lazily on first selection. Only the active
// tab's view is visible; hidden views' SubViewports stop rendering on their own
// (SubViewport UPDATE_WHEN_VISIBLE), which is the cheap half of the inactive-doc
// cost policy (ARCHITECTURE.md open decision #1 covers pausing physics too).
class TabbedDocumentHost : public VBoxContainer {
	GDCLASS(TabbedDocumentHost, VBoxContainer);

	TabBar *tab_bar = nullptr;
	Control *content_host = nullptr; // Fills the pane; parent of every DocumentView.

	Vector<EditorDocument *> documents; // One per tab, index-aligned with the TabBar. Not owned.
	Vector<DocumentView *> views; // Parallel to documents; lazily created, owned via content_host.
	int current = -1;

	DocumentView *_ensure_view(int p_idx); // Create (hidden) if absent; returns the view.
	void _show(int p_idx); // Reveal tab p_idx's view, hide the rest.
	void _activate_document(int p_idx); // Make tab p_idx's document the editor's active edited scene.
	void _on_tab_selected(int p_idx);
	void _on_tab_close(int p_idx);

protected:
	static void _bind_methods() {}

public:
	// Append a tab for p_document titled p_title; returns its index. The first
	// document added becomes the active tab.
	int add_document(EditorDocument *p_document, const String &p_title);

	void set_current(int p_idx);
	int get_current() const { return current; }
	int get_document_count() const { return documents.size(); }

	TabbedDocumentHost();
};
