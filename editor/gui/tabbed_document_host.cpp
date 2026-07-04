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
#include "editor/editor_data.h"
#include "editor/editor_document.h"
#include "editor/editor_node.h"
#include "editor/gui/document_view.h"
#include "editor/script/script_editor_plugin.h" // G2 S6a: current-script-view sync.
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

bool TabbedDocumentHost::has_document(EditorDocument *p_document) const {
	return documents.find(p_document) >= 0;
}

void TabbedDocumentHost::focus_document(EditorDocument *p_document) {
	// G2 S5: if the document already has a tab here, select it; otherwise append one (titled from the
	// document's path filename, e.g. "player.gd" or "Node2D") and select that.
	if (!p_document) {
		return;
	}
	set_current(ensure_document(p_document));
}

int TabbedDocumentHost::ensure_document(EditorDocument *p_document) {
	// G2 S6a: add-if-missing without selecting, so a background open (dominant script during a
	// scene change) doesn't steal the current tab. The view is minted eagerly (hidden) so the
	// document's editor surface — and for scripts its ScriptEditor registration — exists even
	// before the tab is first shown.
	ERR_FAIL_NULL_V(p_document, -1);
	int idx = documents.find(p_document);
	if (idx < 0) {
		String title = p_document->get_path().get_file();
		if (title.is_empty()) {
			title = "Document";
		}
		idx = add_document(p_document, title);
	}
	_ensure_view(idx);
	return idx;
}

DocumentView *TabbedDocumentHost::get_current_view() const {
	if (current < 0 || current >= views.size()) {
		return nullptr;
	}
	return views[current];
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

void TabbedDocumentHost::_activate_document(int p_idx) {
	// Make this tab's document the editor's active edited scene, so the docks / inspector /
	// scene-tree follow the pane's selection. Resolve the scene index by document (robust to
	// tabs/scenes being reordered or closed) rather than caching an index.
	if (p_idx < 0 || p_idx >= documents.size() || !documents[p_idx]) {
		return;
	}
	EditorNode *en = EditorNode::get_singleton();
	if (!en) {
		return;
	}
	const int idx = en->get_editor_data().find_document_index(documents[p_idx]);
	if (idx >= 0) {
		en->set_edited_scene_index(idx);
	}
}

void TabbedDocumentHost::_sync_current_script_view(int p_idx) {
	// G2 S6a: the "current script" the ScriptEditor services act on (save, run, breakpoints)
	// follows the workspace: a script tab becoming current makes its view the current one; any
	// other kind of tab clears it (mirrors stock behavior when a help tab is current).
	ScriptEditor *se = ScriptEditor::get_singleton();
	if (!se) {
		return;
	}
	DocumentView *view = (p_idx >= 0 && p_idx < views.size()) ? views[p_idx] : nullptr;
	se->set_current_surface(view ? view->get_editor_surface() : nullptr);
}

void TabbedDocumentHost::_on_tab_selected(int p_idx) {
	_show(p_idx);
	// Only a genuine, in-tree selection drives the global active scene -- not the programmatic
	// set_current() done while the host is being built and placed into a pane.
	if (is_inside_tree()) {
		_activate_document(p_idx);
		_sync_current_script_view(p_idx); // G2 S6a
	}
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
