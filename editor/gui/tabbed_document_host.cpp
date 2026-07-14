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
#include "editor/gui/pane_drop_overlay.h" // G6: drag-to-split compass.
#include "editor/script/script_editor_plugin.h" // G2 S6a: current-script-view sync.
#include "editor/shader/shader_editor_plugin.h" // G-Shader: current-shader-view sync.
#include "editor/editor_string_names.h"
#include "editor/themes/editor_scale.h"
#include "scene/gui/margin_container.h"
#include "scene/gui/panel_container.h"
#include "scene/gui/popup_menu.h"
#include "scene/gui/tab_bar.h"
#include "scene/resources/style_box_flat.h"

void TabbedDocumentHost::_notification(int p_what) {
	if (p_what != NOTIFICATION_THEME_CHANGED || !tabbar_panel) {
		return;
	}
	// The native tab-bar background, minus its top content-margin — that inset was showing a lighter
	// band above the tabs.
	Ref<StyleBox> tab_bg = get_theme_stylebox(SNAME("tabbar_background"), SNAME("TabContainer"));
	if (tab_bg.is_valid()) {
		Ref<StyleBox> bg = tab_bg->duplicate();
		bg->set_content_margin(SIDE_TOP, 0);
		bg->set_content_margin(SIDE_BOTTOM, 0);
		tabbar_panel->add_theme_style_override(SceneStringName(panel), bg);
	}
	// Tabs: keep the editor's native tab styleboxes (their colors + corners already read well) and add
	// only the subtle top-edge highlight to the selected tab, matching the dock-section cards. Hand-
	// rolling the colors looked muddy, so we build on the theme's own stylebox instead.
	Ref<StyleBox> sel_src = get_theme_stylebox(SNAME("tab_selected"), SNAME("TabBar"));
	if (sel_src.is_valid()) {
		Ref<StyleBoxFlat> sel = sel_src->duplicate();
		if (sel.is_valid()) {
			sel->set_border_width(SIDE_TOP, MAX(1, int(EDSCALE)));
			sel->set_border_color(sel->get_bg_color().lightened(0.30));
			tab_bar->add_theme_style_override(SNAME("tab_selected"), sel);
		}
	}
}

TabbedDocumentHost::TabbedDocumentHost() {
	set_h_size_flags(SIZE_EXPAND_FILL);
	set_v_size_flags(SIZE_EXPAND_FILL);
	add_theme_constant_override("separation", 0);

	// G2 styling: host the tab bar in a panel so it gets the editor's tab-bar background (applied on
	// NOTIFICATION_THEME_CHANGED), matching the native scene-tab strip.
	tabbar_panel = memnew(PanelContainer);
	add_child(tabbar_panel);

	tab_bar = memnew(TabBar);
	tab_bar->set_h_size_flags(SIZE_EXPAND_FILL);
	tab_bar->set_tab_close_display_policy(TabBar::CLOSE_BUTTON_SHOW_ALWAYS);
	tab_bar->set_drag_to_rearrange_enabled(true);
	tab_bar->connect("tab_selected", callable_mp(this, &TabbedDocumentHost::_on_tab_selected));
	tab_bar->connect("tab_close_pressed", callable_mp(this, &TabbedDocumentHost::_on_tab_close));
	tab_bar->connect("tab_rmb_clicked", callable_mp(this, &TabbedDocumentHost::_on_tab_rmb)); // G2 S8
	tabbar_panel->add_child(tab_bar);

	// G2 S8: pane management context menu (built per popup in _on_tab_rmb).
	tab_menu = memnew(PopupMenu);
	add_child(tab_menu);
	tab_menu->connect("id_pressed", callable_mp(this, &TabbedDocumentHost::_on_menu_pressed));

	content_host = memnew(MarginContainer);
	content_host->set_h_size_flags(SIZE_EXPAND_FILL);
	content_host->set_v_size_flags(SIZE_EXPAND_FILL);
	// G2 styling: no inset around the pane content — the default MarginContainer theme margins read as
	// a thin gray band between the tab bar / toolbar and the viewport.
	content_host->add_theme_constant_override("margin_left", 0);
	content_host->add_theme_constant_override("margin_right", 0);
	content_host->add_theme_constant_override("margin_top", 0);
	content_host->add_theme_constant_override("margin_bottom", 0);
	add_child(content_host);

	// G6: the drag-to-split compass overlays the content area. It is mouse-transparent until a tab drag
	// arms it, and must stay the topmost child so it paints over (and intercepts drops before) the views.
	drop_overlay = memnew(PaneDropOverlay);
	drop_overlay->set_owner_host(this);
	content_host->add_child(drop_overlay);
}

void TabbedDocumentHost::_raise_overlay() {
	if (drop_overlay && drop_overlay->get_parent() == content_host) {
		content_host->move_child(drop_overlay, content_host->get_child_count() - 1);
	}
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
		_raise_overlay(); // G6: keep the drop compass above the newly added view.
	}
	return views[p_idx];
}

void TabbedDocumentHost::_show(int p_idx) {
	if (p_idx < 0 || p_idx >= documents.size()) {
		return;
	}
	if (current >= 0 && current < views.size() && current != p_idx && views[current]) {
		views[current]->set_context_active(false);
	}
	_ensure_view(p_idx);
	// G2 styling: refresh the tab title — a scene document's name resolves once its root/scene-file is
	// set, which can be after the (background) tab was first added.
	if (documents[p_idx]) {
		tab_bar->set_tab_title(p_idx, documents[p_idx]->get_title());
	}
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
	EditorDocument *scene_context = documents[p_idx]->get_scene_context_document();
	const int idx = en->get_editor_data().find_document_index(scene_context);
	if (idx >= 0) {
		en->set_edited_scene_index(idx);
	}
}

void TabbedDocumentHost::set_context_active(bool p_active) {
	DocumentView *view = get_current_view();
	if (view) {
		view->set_context_active(p_active);
	}
}

void TabbedDocumentHost::_sync_current_script_view(int p_idx) {
	// G2 S6a: the "current script" the ScriptEditor services act on (save, run, breakpoints)
	// follows the workspace: a script tab becoming current makes its view the current one; any
	// other kind of tab clears it (mirrors stock behavior when a help tab is current). The shader
	// File menu (G-Shader) and the scene 2D/3D toolbar (M7.2a) follow the same tab the same way —
	// each guarded independently so a missing subscriber can't starve the others.
	DocumentView *view = (p_idx >= 0 && p_idx < views.size()) ? views[p_idx] : nullptr;
	if (ScriptEditor *se = ScriptEditor::get_singleton()) {
		se->set_current_surface(view);
	}
	if (ShaderEditorPlugin *sep = ShaderEditorPlugin::get_singleton()) {
		sep->set_current_surface(view);
	}
	if (EditorNode *en = EditorNode::get_singleton()) {
		en->update_scene_pane_toolbar(view);
	}
}

void TabbedDocumentHost::_on_tab_selected(int p_idx) {
	_show(p_idx);
	// Only a genuine, in-tree selection drives the global active scene -- not the programmatic
	// set_current() done while the host is being built and placed into a pane, and not the
	// reselect that follows dropping a tab mid scene-close (suppress_activation).
	if (is_inside_tree() && !suppress_activation) {
		_activate_document(p_idx);
		_sync_current_script_view(p_idx); // G2 S6a
		WorkspacePane *pane = _owning_pane();
		EditorWorkspace *workspace = pane ? pane->get_workspace() : nullptr;
		if (workspace && workspace->get_focused_pane() == pane) {
			set_context_active(true);
		}
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
	// A scene tab close must close the SCENE (unsaved prompt, EditorData removal) — not just
	// drop the view, which would leave the scene resident and invisible. The tab itself is
	// removed by drop_document_tab() when the scene actually closes (EditorData::remove_scene);
	// a cancelled prompt leaves it untouched.
	EditorDocument *doc = documents[p_idx];
	if (doc) {
		switch (doc->get_type()) {
			case EditorDocument::TYPE_SCENE_2D:
			case EditorDocument::TYPE_SCENE_3D:
			case EditorDocument::TYPE_SCENE_MIXED:
			case EditorDocument::TYPE_LEVEL: {
				EditorNode *en = EditorNode::get_singleton();
				const int scene_idx = en ? en->get_editor_data().find_document_index(doc) : -1;
				if (scene_idx >= 0) {
					en->request_close_scene_tab(scene_idx);
					return;
				}
			} break;
			default:
				break;
		}
	}
	_drop_tab_at(p_idx);
}

bool TabbedDocumentHost::drop_document_tab(EditorDocument *p_document) {
	// Mechanical removal (no close routing) — used when the document is being destroyed.
	const int idx = documents.find(p_document);
	if (idx < 0) {
		return false;
	}
	_drop_tab_at(idx);
	return true;
}

void TabbedDocumentHost::_drop_tab_at(int p_idx) {
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
	// The reselect inside _remove_tab_entry must not re-enter the global scene switch —
	// we may be deep inside EditorData::remove_scene (drop_document_tab) with the scene
	// list mid-mutation. The deferred _ensure_active_scene_tab reveal self-corrects focus.
	suppress_activation = true;
	_remove_tab_entry(p_idx);
	suppress_activation = false;

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
	_raise_overlay(); // G6: keep the drop compass above the adopted view.
	tab_bar->add_tab(p_document->get_title());
	set_current(idx);
	return idx;
}

WorkspacePane *TabbedDocumentHost::_owning_pane() const {
	return WorkspacePane::of(get_parent()); // G6: shared parent-chain walk.
}

TabbedDocumentHost *TabbedDocumentHost::_host_from_drag_data(const Variant &p_data, int &r_tab, const Node *p_ref) {
	// G6: decode a native TabBar rearrange payload ({type:"tab", tab_type:"tab_bar_tab", tab_index,
	// from_path}) into the source host + tab index. Returns null for any non-workspace-tab drag.
	r_tab = -1;
	if (p_data.get_type() != Variant::DICTIONARY || !p_ref) {
		return nullptr;
	}
	const Dictionary d = p_data;
	if (String(d.get("type", String())) != "tab" || String(d.get("tab_type", String())) != "tab_bar_tab") {
		return nullptr;
	}
	if (!d.has("from_path") || !d.has("tab_index")) {
		return nullptr;
	}
	Node *from = p_ref->get_node_or_null(d["from_path"]);
	for (Node *n = from; n; n = n->get_parent()) {
		if (TabbedDocumentHost *host = Object::cast_to<TabbedDocumentHost>(n)) {
			const int tab = d["tab_index"];
			if (tab < 0 || tab >= host->get_document_count()) {
				return nullptr; // Stale/out-of-range index -> not a valid drop.
			}
			r_tab = tab;
			return host;
		}
	}
	return nullptr;
}

bool TabbedDocumentHost::can_accept_tab_drop(const Variant &p_data) const {
	int tab = -1;
	return _host_from_drag_data(p_data, tab, this) != nullptr;
}

void TabbedDocumentHost::accept_tab_drop(const Variant &p_data, bool p_center, bool p_vertical, bool p_new_on_second) {
	int tab = -1;
	TabbedDocumentHost *src = _host_from_drag_data(p_data, tab, this);
	if (!src) {
		return;
	}
	WorkspacePane *pane = _owning_pane();
	EditorWorkspace *ws = pane ? pane->get_workspace() : nullptr;
	if (!ws) {
		return;
	}
	ws->move_tab_into_pane(src, tab, pane, p_center, p_vertical, p_new_on_second);
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
