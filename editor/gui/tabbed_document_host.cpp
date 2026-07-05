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
#include "editor/gui/editor_workspace.h" // G2 S8: pane split/close from the tab bar.
#include "editor/script/script_editor_plugin.h" // G2 S6a: current-script-view sync.
#include "scene/gui/margin_container.h"
#include "scene/gui/popup_menu.h"
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
	tab_bar->connect("tab_rmb_clicked", callable_mp(this, &TabbedDocumentHost::_on_tab_rmb)); // G2 S8
	add_child(tab_bar);

	// G2 S8: pane management context menu (built per popup in _on_tab_rmb).
	tab_menu = memnew(PopupMenu);
	add_child(tab_menu);
	tab_menu->connect("id_pressed", callable_mp(this, &TabbedDocumentHost::_on_menu_pressed));

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
		idx = add_document(p_document, p_document->get_title());
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
	se->set_current_surface(view);
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

bool TabbedDocumentHost::close_document(EditorDocument *p_document) {
	// G2 S7: programmatic close (File menu paths) — same pipeline as the tab X.
	const int idx = documents.find(p_document);
	if (idx < 0) {
		return false;
	}
	_on_tab_close(idx);
	return true;
}

void TabbedDocumentHost::_remove_tab_entry(int p_idx) {
	// G2 S8: drop the tab row and reselect — keep the previously current tab when it
	// survives (closing/moving a background tab must not steal the selection), else the
	// nearest neighbour.
	int reselect = current;
	views.remove_at(p_idx);
	documents.remove_at(p_idx);
	tab_bar->remove_tab(p_idx);
	if (p_idx < reselect) {
		reselect--;
	}
	current = -1;
	if (documents.size() > 0) {
		set_current(CLAMP(reselect, 0, documents.size() - 1));
	}
}

void TabbedDocumentHost::_on_tab_close(int p_idx) {
	if (p_idx < 0 || p_idx >= documents.size()) {
		return;
	}
	// G2 S7: single choke point for script/help close side effects (state cache,
	// previous-scripts, notify_script_close) — must run while the surface is still alive.
	if (views[p_idx]) {
		if (ScriptEditor *se = ScriptEditor::get_singleton()) {
			se->notify_surface_closing(views[p_idx]->get_editor_surface());
		}
	}
	if (views[p_idx]) {
		// Child of content_host: memdelete removes it from the tree and frees it (with
		// its Node3DEditorView, whose gizmo layer is returned to its world's freelist).
		memdelete(views[p_idx]);
	}
	_remove_tab_entry(p_idx);

	// G2 S8: the last tab closing in a non-root pane closes the pane. Deferred — we may
	// be deep inside this host's own signal emission, and the close frees this host.
	if (documents.is_empty()) {
		WorkspacePane *pane = _owning_pane();
		EditorWorkspace *ws = pane ? pane->get_workspace() : nullptr;
		if (ws && pane != ws->get_root_pane()) {
			ws->queue_close_pane(pane);
		}
	}
}

EditorDocument *TabbedDocumentHost::get_document(int p_idx) const {
	if (p_idx < 0 || p_idx >= documents.size()) {
		return nullptr;
	}
	return documents[p_idx];
}

DocumentView *TabbedDocumentHost::detach_tab(int p_idx) {
	// G2 S8: remove the tab and hand back its live view WITHOUT close side effects —
	// the caller re-homes it in another host, so editing state stays untouched.
	ERR_FAIL_INDEX_V(p_idx, documents.size(), nullptr);
	DocumentView *view = _ensure_view(p_idx);
	content_host->remove_child(view);
	_remove_tab_entry(p_idx);
	return view;
}

int TabbedDocumentHost::adopt_tab(EditorDocument *p_document, DocumentView *p_view) {
	// G2 S8: receive a tab detached from another host — the view is reparented and the
	// new tab selected.
	ERR_FAIL_NULL_V(p_document, -1);
	ERR_FAIL_NULL_V(p_view, -1);
	const int idx = documents.size();
	documents.push_back(p_document);
	views.push_back(p_view);
	content_host->add_child(p_view);
	p_view->set_visible(false); // set_current reveals it.
	tab_bar->add_tab(p_document->get_title());
	set_current(idx);
	return idx;
}

WorkspacePane *TabbedDocumentHost::_owning_pane() const {
	for (Node *n = get_parent(); n; n = n->get_parent()) {
		if (WorkspacePane *pane = Object::cast_to<WorkspacePane>(n)) {
			return pane;
		}
	}
	return nullptr;
}

void TabbedDocumentHost::_on_tab_rmb(int p_idx) {
	// G2 S8: pane management entry point — split/close from the tab bar.
	menu_tab = p_idx;
	tab_menu->clear();
	tab_menu->add_item(TTR("Split Right"), MENU_SPLIT_RIGHT);
	tab_menu->add_item(TTR("Split Down"), MENU_SPLIT_DOWN);
	tab_menu->add_separator();
	tab_menu->add_item(TTR("Close Tab"), MENU_CLOSE_TAB);
	tab_menu->add_item(TTR("Close Pane"), MENU_CLOSE_PANE);

	// Splitting out the only tab would leave a dead pane behind; the root pane never closes.
	const bool can_split = documents.size() > 1;
	tab_menu->set_item_disabled(tab_menu->get_item_index(MENU_SPLIT_RIGHT), !can_split);
	tab_menu->set_item_disabled(tab_menu->get_item_index(MENU_SPLIT_DOWN), !can_split);
	WorkspacePane *pane = _owning_pane();
	EditorWorkspace *ws = pane ? pane->get_workspace() : nullptr;
	const bool can_close_pane = ws && pane != ws->get_root_pane();
	tab_menu->set_item_disabled(tab_menu->get_item_index(MENU_CLOSE_PANE), !can_close_pane);

	tab_menu->set_position(get_screen_position() + get_local_mouse_position());
	tab_menu->reset_size();
	tab_menu->popup();
}

void TabbedDocumentHost::_on_menu_pressed(int p_id) {
	WorkspacePane *pane = _owning_pane();
	EditorWorkspace *ws = pane ? pane->get_workspace() : nullptr;
	switch (p_id) {
		case MENU_SPLIT_RIGHT:
		case MENU_SPLIT_DOWN: {
			if (ws) {
				ws->split_pane_with_tab(pane, menu_tab, p_id == MENU_SPLIT_DOWN);
			}
		} break;
		case MENU_CLOSE_TAB: {
			close_tab(menu_tab);
		} break;
		case MENU_CLOSE_PANE: {
			// Deferred: closing frees this host (and the menu emitting this signal).
			if (ws) {
				ws->queue_close_pane(pane);
			}
		} break;
	}
}
