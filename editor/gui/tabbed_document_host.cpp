/**************************************************************************/
/*  tabbed_document_host.cpp                                              */
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

#include "tabbed_document_host.h"

#include "core/object/callable_mp.h"
#include "editor/gui/document_view.h"
#include "scene/gui/margin_container.h"
#include "scene/gui/tab_bar.h"

TabbedDocumentHost::TabbedDocumentHost() {
	set_h_size_flags(SIZE_EXPAND_FILL);
	set_v_size_flags(SIZE_EXPAND_FILL);
	add_theme_constant_override("separation", 0);

	tab_bar = memnew(TabBar);
	tab_bar->set_h_size_flags(SIZE_EXPAND_FILL);
	tab_bar->set_tab_close_display_policy(TabBar::CLOSE_BUTTON_SHOW_ALWAYS);
	tab_bar->connect("tab_selected", callable_mp(this, &TabbedDocumentHost::_on_tab_selected));
	tab_bar->connect("tab_close_pressed", callable_mp(this, &TabbedDocumentHost::_on_tab_close));
	add_child(tab_bar);

	content_host = memnew(MarginContainer);
	content_host->set_h_size_flags(SIZE_EXPAND_FILL);
	content_host->set_v_size_flags(SIZE_EXPAND_FILL);
	add_child(content_host);
}

int TabbedDocumentHost::add_document(EditorDocument *p_document, const String &p_title) {
	const int idx = documents.size();
	documents.push_back(p_document);
	views.push_back(nullptr);
	tab_bar->add_tab(p_title);
	if (current < 0) {
		set_current(idx);
	}
	return idx;
}

DocumentView *TabbedDocumentHost::_ensure_view(int p_idx) {
	ERR_FAIL_INDEX_V(p_idx, documents.size(), nullptr);
	if (!views[p_idx]) {
		DocumentView *view = memnew(DocumentView(documents[p_idx]));
		views.write[p_idx] = view;
		content_host->add_child(view);
		view->set_visible(false); // _show() reveals exactly one.
	}
	return views[p_idx];
}

void TabbedDocumentHost::_show(int p_idx) {
	if (p_idx < 0 || p_idx >= documents.size()) {
		return;
	}
	_ensure_view(p_idx);
	// Reveal the selected view, hide the rest. Hidden SubViewports stop rendering
	// (UPDATE_WHEN_VISIBLE), so only the active tab draws.
	for (int i = 0; i < views.size(); i++) {
		if (views[i]) {
			views[i]->set_visible(i == p_idx);
		}
	}
	current = p_idx;
}

void TabbedDocumentHost::set_current(int p_idx) {
	if (p_idx < 0 || p_idx >= documents.size()) {
		return;
	}
	_show(p_idx);
	if (tab_bar->get_current_tab() != p_idx) {
		tab_bar->set_current_tab(p_idx); // May re-emit tab_selected; _show is idempotent.
	}
}

void TabbedDocumentHost::_on_tab_selected(int p_idx) {
	_show(p_idx);
}

void TabbedDocumentHost::_on_tab_close(int p_idx) {
	if (p_idx < 0 || p_idx >= documents.size()) {
		return;
	}
	if (views[p_idx]) {
		// Child of content_host: memdelete removes it from the tree and frees it (with
		// its Node3DEditorView, whose gizmo layer is returned to its world's freelist).
		memdelete(views[p_idx]);
	}
	views.remove_at(p_idx);
	documents.remove_at(p_idx);
	tab_bar->remove_tab(p_idx);

	// Reselect a neighbour so a view is always shown while tabs remain.
	current = -1;
	if (documents.size() > 0) {
		set_current(MIN(p_idx, documents.size() - 1));
	}
}
