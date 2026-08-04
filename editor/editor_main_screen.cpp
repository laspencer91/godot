/**************************************************************************/
/*  editor_main_screen.cpp                                                */
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

#include "editor_main_screen.h"

#include "core/io/config_file.h"
#include "core/object/callable_mp.h"
#include "editor/editor_node.h"
#include "editor/editor_string_names.h"
#include "editor/gui/editor_workspace.h"
#include "editor/gui/tabbed_document_host.h"
#include "editor/plugins/editor_plugin.h"
#include "editor/script/script_editor_plugin.h" // G2 S6b: focus_editor("Script") over the workspace.
#include "editor/settings/editor_settings.h"
#include "scene/gui/box_container.h"
#include "scene/gui/button.h"

void EditorMainScreen::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_READY: {
			set_accessibility_region(true);
			if (EDITOR_3D < buttons.size() && buttons[EDITOR_3D]->is_visible()) {
				// If the 3D editor is enabled, use this as the default.
				select(EDITOR_3D);
				return;
			}

			// Switch to the first main screen plugin that is enabled. Usually this is
			// 2D, but may be subsequent ones if 2D is disabled in the feature profile.
			for (int i = 0; i < buttons.size(); i++) {
				Button *editor_button = buttons[i];
				if (editor_button->is_visible()) {
					select(i);
					return;
				}
			}

			// G4: with 2D/3D/Script retired and Game/AssetLib on-demand, the strip can legitimately have
			// no visible button at boot. Leave the main-screen backdrop empty (selected_plugin stays null)
			// rather than latching onto whatever happens to be first — previously the Game screen, which
			// surfaced an unwanted embedded-player tab on load. The workspace panes carry the real editing
			// surfaces; a screen is summoned on demand (reveal_main_plugin) or by opening a scene/script.
		} break;
		case NOTIFICATION_THEME_CHANGED: {
			for (int i = 0; i < buttons.size(); i++) {
				Button *tb = buttons[i];
				EditorPlugin *p_editor = editor_table[i];
				Ref<Texture2D> icon = p_editor->get_plugin_icon();

				if (icon.is_valid()) {
					tb->set_button_icon(icon);
				} else if (has_theme_icon(p_editor->get_plugin_name(), EditorStringName(EditorIcons))) {
					tb->set_button_icon(get_theme_icon(p_editor->get_plugin_name(), EditorStringName(EditorIcons)));
				}
			}
		} break;
	}
}

void EditorMainScreen::set_button_container(HBoxContainer *p_button_hb) {
	button_hb = p_button_hb;
}

void EditorMainScreen::save_layout_to_config(Ref<ConfigFile> p_config_file, const String &p_section) const {
	int selected_main_editor_idx = -1;
	for (int i = 0; i < buttons.size(); i++) {
		if (buttons[i]->is_pressed()) {
			selected_main_editor_idx = i;
			break;
		}
	}
	if (selected_main_editor_idx != -1) {
		p_config_file->set_value(p_section, "selected_main_editor_idx", selected_main_editor_idx);
	} else {
		p_config_file->set_value(p_section, "selected_main_editor_idx", Variant());
	}

	// G2 M6.2: persist the whole workspace as one blob so restore is a single unit.
	if (workspace) {
		p_config_file->set_value(p_section, "workspace", get_workspace_blob());
	}
}

Dictionary EditorMainScreen::get_workspace_blob() const {
	// G2 M6.2/M6.3: the workspace session — split-tree geometry (M6.1) plus, per leaf, the tab documents
	// (by path) and which is current. Exposed so the named-layout store can capture it without a scratch
	// ConfigFile round-trip.
	Dictionary session;
	if (workspace) {
		session["geometry"] = workspace->save_geometry();
		Dictionary tabs;
		_collect_pane_tabs(tabs);
		session["tabs"] = tabs;
	}
	return session;
}

void EditorMainScreen::load_layout_from_config(Ref<ConfigFile> p_config_file, const String &p_section) {
	// G2 M6.2 — restore PHASE 2 (tabs). Runs from _load_central_editor_layout_from_config, AFTER scenes
	// are restored, so scene/script/help documents resolve here. The geometry + screen-host were already
	// rebuilt in phase 1 (begin_workspace_restore, before any scene loaded — see below); this only fills
	// the panes with their remaining tabs and drives focus. When there's no saved session, the legacy
	// strip select() path runs instead.
	if (_workspace_restore_pending) {
		_collapse_transient_only_panes(_pending_tabs);
		_populate_pane_tabs(_pending_tabs);
		_set_workspace_focus_after_restore();
		_pending_tabs = Dictionary();
		_workspace_restore_pending = false; // Resume the deferred active-scene reveal.
		return;
	}

	int selected_main_editor_idx = p_config_file->get_value(p_section, "selected_main_editor_idx", -1);
	if (selected_main_editor_idx >= 0 && selected_main_editor_idx < buttons.size()) {
		callable_mp(this, &EditorMainScreen::select).call_deferred(selected_main_editor_idx);
	}
}

bool EditorMainScreen::begin_workspace_restore(Ref<ConfigFile> p_config_file, const String &p_section) {
	// G2 M6.2 — restore PHASE 1 (geometry + screen-host), called BEFORE any scene is opened. This timing
	// is essential: rebuilding the tree re-enters main_screen_vbox (the live 2D/3D editors) into a pane,
	// and re-entering the 3D editor while multiple scene worlds are live exhausts the per-world gizmo
	// layer budget and crashes. At 0 open scenes the move is clean. Phase 2 (load_layout_from_config)
	// then adds the scene/script tabs once the scenes exist. Returns true if a session was found.
	if (!workspace || !p_config_file->has_section_key(p_section, "workspace")) {
		return false;
	}
	Dictionary session = p_config_file->get_value(p_section, "workspace");
	if (!workspace->load_geometry(session.get("geometry", Dictionary()))) {
		return false; // Bad geometry: leave the default tree; legacy/auto-reveal path takes over.
	}

	Dictionary tabs = session.get("tabs", Dictionary());

	// G4: the screen-host ("Editor") tab is on-demand now, so it is NOT restored as a tab. Its stack
	// (main_screen_vbox) stays parked under the hidden in-tree holder (see the constructor); an AssetLib /
	// Game / plugin reveal re-summons the tab from there. A stale screen-host entry in a pre-G4 saved
	// session is harmless — _restore_workspace_tabs already skips it, and _collect_pane_tabs no longer
	// records it, so it drops out on the next save.

	// Hand the tab set to phase 2 and stand the M7.1 scene auto-reveal down until then. Keep an explicit
	// pending bit because a screen-host-only session correctly produces an empty persistent tab set.
	_pending_tabs = tabs;
	_workspace_restore_pending = true;
	return true;
}

void EditorMainScreen::_collect_pane_tabs(Dictionary &r_tabs) const {
	// G2 M6.2: record each tabbed leaf's documents (by path) + current doc path, keyed by pane_id. Docs
	// without a persistable path (unsaved scenes) are skipped; a leaf with none contributes no entry.
	for (WorkspacePane *pane : workspace->get_tabbed_leaves()) {
		TabbedDocumentHost *host = Object::cast_to<TabbedDocumentHost>(pane->get_content()); // Non-null by get_tabbed_leaves.
		Array docs;
		String current_path;
		for (int i = 0; i < host->get_document_count(); i++) {
			EditorDocument *doc = host->get_document(i);
			if (!doc || doc->get_path().is_empty()) {
				continue;
			}
			if (doc == screen_host_document) {
				continue; // G4: on-demand screen-host is never persisted as a tab.
			}
			if (i == host->get_current()) {
				current_path = doc->get_path();
			}
			docs.push_back(doc->get_path());
		}
		if (docs.is_empty()) {
			continue;
		}
		Dictionary entry;
		entry["docs"] = docs;
		entry["cur"] = current_path;
		r_tabs[itos(pane->get_pane_id())] = entry;
	}
}

EditorDocument *EditorMainScreen::_resolve_session_document(const String &p_path) {
	// G2 M6.2: the legacy-screens host doc is ours (not in EditorData); everything else — scenes,
	// scripts, help — resolves through EditorNode against the already-restored open-document set.
	if (p_path.is_empty()) {
		return nullptr;
	}
	if (screen_host_document && p_path == screen_host_document->get_path()) {
		return screen_host_document;
	}
	return EditorNode::get_editor_data().get_or_create_document_for_path(p_path);
}

void EditorMainScreen::_collapse_transient_only_panes(const Dictionary &p_tabs) {
	// Decide the final topology before creating any DocumentViews. Collapsing a populated leaf reparents
	// its host through an out-of-tree interval, which is unsafe for scene cameras. A leaf whose only saved
	// document is the deliberately non-restored screen host can be identified without loading anything.
	const String screen_host_path = screen_host_document ? screen_host_document->get_path() : String();
	while (workspace && !workspace->get_root_pane()->is_leaf()) {
		WorkspacePane *transient_only_pane = nullptr;
		for (WorkspacePane *pane : workspace->get_tabbed_leaves()) {
			bool has_persistent_document = false;
			const String key = itos(pane->get_pane_id());
			if (p_tabs.has(key)) {
				Dictionary entry = p_tabs[key];
				Array docs = entry.get("docs", Array());
				for (int i = 0; i < docs.size(); i++) {
					const String path = docs[i];
					if (!path.is_empty() && path != screen_host_path) {
						has_persistent_document = true;
						break;
					}
				}
			}
			if (!has_persistent_document) {
				transient_only_pane = pane;
				break;
			}
		}
		if (!transient_only_pane) {
			break;
		}
		workspace->close_pane(transient_only_pane);
	}
}

void EditorMainScreen::_populate_pane_tabs(const Dictionary &p_tabs) {
	// G2 M6.2 (phase 2): fill each tabbed leaf with its saved documents. The screen-host is skipped
	// (G4: on-demand only — it stays parked, never restored as a tab; a stale saved entry is ignored).
	for (WorkspacePane *pane : workspace->get_tabbed_leaves()) {
		const String key = itos(pane->get_pane_id());
		if (!p_tabs.has(key)) {
			continue;
		}
		TabbedDocumentHost *host = Object::cast_to<TabbedDocumentHost>(pane->get_content()); // Non-null by get_tabbed_leaves.
		Dictionary entry = p_tabs[key];
		Array docs = entry.get("docs", Array());
		for (int i = 0; i < docs.size(); i++) {
			EditorDocument *doc = _resolve_session_document(docs[i]);
			if (doc && doc != screen_host_document) {
				host->ensure_document(doc); // Appends a (hidden) tab + mints the pane-bound view; no focus steal.
			}
		}
		// Select the saved current tab now that this pane's documents exist.
		if (EditorDocument *current = _resolve_session_document(entry.get("cur", String()))) {
			if (host->has_document(current)) {
				host->focus_document(current);
			}
		}
	}
}

void EditorMainScreen::_set_workspace_focus_after_restore() {
	// G2 M6.2: now that every pane is filled, drive focus once through the real path (load_geometry left
	// focused_pane null) so the focused pane's script surface / scene-pane toolbar bind correctly. Prefer
	// the pane showing the active scene, so the editor opens on it just like a normal boot.
	WorkspacePane *focus = nullptr;
	if (EditorNode *en = EditorNode::get_singleton()) {
		if (EditorDocument *active = en->get_editor_data().get_active_document()) {
			focus = workspace->find_pane_showing(active);
		}
	}
	if (!focus) {
		focus = workspace->get_last_tabbed_pane();
	}
	workspace->set_focused_pane(focus ? focus : workspace->get_root_pane());
}

void EditorMainScreen::set_button_enabled(int p_index, bool p_enabled) {
	ERR_FAIL_INDEX(p_index, buttons.size());
	Button *b = buttons[p_index];
	// G2 M7.2b: a retired strip button (2D/3D/Script) stays hidden regardless of profile enable state
	// — its editing surface lives in the workspace panes now, not the strip.
	// G4: an on-demand button (Game/AssetLib) is never *shown* by the profile sync — only
	// reveal_main_plugin() gives it strip presence — but a profile *disable* may still pull it off.
	if (b->has_meta("_g2_strip_retired")) {
		// Always hidden; leave as-is.
	} else if (b->has_meta("_g2_on_demand")) {
		if (!p_enabled) {
			b->hide();
		}
	} else {
		b->set_visible(p_enabled);
	}
	if (!p_enabled && b->is_pressed()) {
		select(EDITOR_2D);
	}
}

bool EditorMainScreen::is_button_enabled(int p_index) const {
	ERR_FAIL_INDEX_V(p_index, buttons.size(), false);
	// G2 M7.2b / G4: retired (2D/3D/Script) and on-demand (Game/AssetLib) buttons have no permanent
	// strip presence, but their features remain reachable (via the workspace / an on-demand reveal),
	// so report them enabled — callers gate features on this, not on strip visibility.
	if (buttons[p_index]->has_meta("_g2_strip_retired") || buttons[p_index]->has_meta("_g2_on_demand")) {
		return true;
	}
	return buttons[p_index]->is_visible();
}

void EditorMainScreen::dismiss_main_plugin(int p_index) {
	ERR_FAIL_INDEX(p_index, editor_table.size());
	buttons[p_index]->hide();

	// A different singleton screen (for example Asset Store, if it was open before Play) still needs
	// the shared screen-host document. Only tear the host down when the dismissed plugin owns it.
	if (selected_plugin != editor_table[p_index]) {
		return;
	}

	buttons[p_index]->set_pressed_no_signal(false);
	selected_plugin->make_visible(false);
	selected_plugin = nullptr;
	set_accessibility_name(String());
	if (screen_host_document) {
		close_document(screen_host_document);
	}
	EditorNode::get_singleton()->update_distraction_free_mode();
}

int EditorMainScreen::_get_current_main_editor() const {
	for (int i = 0; i < editor_table.size(); i++) {
		if (editor_table[i] == selected_plugin) {
			return i;
		}
	}

	return 0;
}

void EditorMainScreen::select_next() {
	int editor = _get_current_main_editor();

	do {
		if (editor == editor_table.size() - 1) {
			editor = 0;
		} else {
			editor++;
		}
	} while (!buttons[editor]->is_visible());

	select(editor);
}

void EditorMainScreen::select_prev() {
	int editor = _get_current_main_editor();

	do {
		if (editor == 0) {
			editor = editor_table.size() - 1;
		} else {
			editor--;
		}
	} while (!buttons[editor]->is_visible());

	select(editor);
}

void EditorMainScreen::select_by_name(const String &p_name) {
	focus_editor(StringName(p_name));
}

void EditorMainScreen::select(int p_index) {
	// G2 D1 shim: platform/mono callers must keep working - do not remove.
	_select_index(p_index);
}

void EditorMainScreen::reveal_main_plugin(int p_index) {
	// G4: summon an on-demand singleton screen (Game while an embedded game runs; AssetLib from the
	// Editor menu). Give its button strip presence so _select_index accepts it (and so it can be
	// tabbed back to), home to the screen-host tab, then select it.
	ERR_FAIL_INDEX(p_index, editor_table.size());
	buttons[p_index]->show();
	if (screen_host_document) {
		reveal(screen_host_document);
	}
	_select_index(p_index);
}

void EditorMainScreen::_select_index(int p_index) {
	if (EditorNode::get_singleton()->is_changing_scene()) {
		return;
	}

	ERR_FAIL_INDEX(p_index, editor_table.size());

	if (!buttons[p_index]->is_visible()) { // Button hidden, no editor.
		return;
	}

	for (int i = 0; i < buttons.size(); i++) {
		buttons[i]->set_pressed_no_signal(i == p_index);
	}

	EditorPlugin *new_editor = editor_table[p_index];
	ERR_FAIL_NULL(new_editor);

	if (selected_plugin == new_editor) {
		return;
	}

	// G2 D3: this is the ONLY driver of main-screen make_visible. Per the EditorPlugin::make_visible
	// contract, these calls toggle the outgoing/incoming plugin's own singleton screen surface only;
	// pane-hosted document views are visibility-independent and are never affected here.
	if (selected_plugin) {
		selected_plugin->make_visible(false);
	}

	selected_plugin = new_editor;
	selected_plugin->make_visible(true);
	selected_plugin->selected_notify();
	set_accessibility_name(selected_plugin->get_plugin_name());

	EditorData &editor_data = EditorNode::get_editor_data();
	int plugin_count = editor_data.get_editor_plugin_count();
	for (int i = 0; i < plugin_count; i++) {
		editor_data.get_editor_plugin(i)->notify_main_screen_changed(selected_plugin->get_plugin_name());
	}

	EditorNode::get_singleton()->update_distraction_free_mode();
}

void EditorMainScreen::focus_editor(const StringName &p_name) {
	ERR_FAIL_COND(String(p_name).is_empty());

	const String plugin_name = String(p_name);
	ERR_FAIL_COND_MSG(!main_editor_plugins.has(plugin_name), "The editor name '" + plugin_name + "' was not found.");

	// G2 S6b: "focus the Script editor" means the workspace now — land on the current (else most
	// recent) script/help tab when any is open; the empty legacy Script screen only when none are.
	if (p_name == SNAME("Script")) {
		ScriptEditor *script_editor = ScriptEditor::get_singleton();
		if (script_editor && script_editor->reveal_recent_script_or_help()) {
			return;
		}
	}

	EditorPlugin *plugin = main_editor_plugins[plugin_name];

	// G2 S5.5: singleton screens live inside the screen-host tab — summon/focus that tab first,
	// so the make_visible dance below runs inside a visible view (e.g. strip button pressed while
	// a script tab is current).
	if (screen_host_document) {
		reveal(screen_host_document);
	}
	_select_index(get_plugin_index(plugin));
}

void EditorMainScreen::reveal(EditorDocument *p_document, DocumentViewKind p_kind, bool p_grab_focus) {
	ERR_FAIL_NULL(p_document);

	const EditorDocument::Type document_type = p_document->get_type();

	// G2 S5: tab documents (script/help/screen-host) open as WORKSPACE TABS, not a main-screen
	// switch. If the document already has a tab in any pane, focus it (no-duplicate rule);
	// otherwise resolve a target pane — which, when a script is opened from within a script, is
	// the focused tabbed pane, so the new script lands as a new tab in the SAME pane.
	if (p_document->opens_as_workspace_tab()) {
		if (!workspace) {
			return;
		}
		WorkspacePane *target = workspace->find_pane_showing(p_document);
		if (!target) {
			target = workspace->resolve_target_pane_for_documents();
		}
		if (!target) {
			return;
		}
		if (TabbedDocumentHost *host = Object::cast_to<TabbedDocumentHost>(target->get_content())) {
			if (p_grab_focus) {
				host->focus_document(p_document);
				workspace->set_focused_pane(target);
				host->activate_current_document();
				host->set_context_active(true);
			} else {
				host->ensure_document(p_document); // G2 S6a: background open — tab + hidden view, no focus change.
			}
		}
		return;
	}

	if (document_type != EditorDocument::TYPE_SCENE_2D && document_type != EditorDocument::TYPE_SCENE_3D && document_type != EditorDocument::TYPE_SCENE_MIXED) {
		WARN_PRINT_ONCE("EditorMainScreen::reveal() only handles scene, script, and help documents in this rollout.");
		return;
	}

	EditorNode *editor_node = EditorNode::get_singleton();
	ERR_FAIL_NULL(editor_node);

	const int document_index = editor_node->get_editor_data().find_document_index(p_document);
	if (document_index >= 0) {
		editor_node->set_edited_scene_index(document_index);
	}

	if (p_kind == DocumentViewKind::SCENE_2D) {
		focus_editor(SNAME("2D"));
	} else if (p_kind == DocumentViewKind::SCENE_3D) {
		focus_editor(SNAME("3D"));
	} else if (document_type == EditorDocument::TYPE_SCENE_2D) {
		focus_editor(SNAME("2D"));
	} else {
		focus_editor(SNAME("3D"));
	}
}

bool EditorMainScreen::close_document(EditorDocument *p_document) {
	// G2 S7: route through the owning pane's tab host so the close side effects run.
	if (!workspace || !p_document) {
		return false;
	}
	WorkspacePane *pane = workspace->find_pane_showing(p_document);
	if (!pane) {
		return false;
	}
	TabbedDocumentHost *host = Object::cast_to<TabbedDocumentHost>(pane->get_content());
	return host && host->close_document(p_document);
}

void EditorMainScreen::drop_document_tabs(EditorDocument *p_document) {
	if (!workspace || !p_document) {
		return;
	}
	for (WorkspacePane *pane : workspace->get_tabbed_leaves()) {
		TabbedDocumentHost *host = Object::cast_to<TabbedDocumentHost>(pane->get_content());
		if (host && host->drop_document_tab(p_document)) {
			return; // A document lives in at most one pane.
		}
	}
}

DocumentView *EditorMainScreen::get_document_view(EditorDocument *p_document) const {
	if (!workspace || !p_document) {
		return nullptr;
	}
	WorkspacePane *pane = workspace->find_pane_showing(p_document);
	TabbedDocumentHost *host = pane ? Object::cast_to<TabbedDocumentHost>(pane->get_content()) : nullptr;
	return host ? host->get_document_view(p_document) : nullptr;
}

DocumentView *EditorMainScreen::get_focused_document_view() const {
	if (!workspace) {
		return nullptr;
	}
	WorkspacePane *pane = workspace->get_focused_pane();
	TabbedDocumentHost *host = pane ? Object::cast_to<TabbedDocumentHost>(pane->get_content()) : nullptr;
	return host ? host->get_current_view() : nullptr;
}

int EditorMainScreen::get_selected_index() const {
	for (int i = 0; i < editor_table.size(); i++) {
		if (selected_plugin == editor_table[i]) {
			return i;
		}
	}
	return -1;
}

int EditorMainScreen::get_plugin_index(EditorPlugin *p_editor) const {
	int screen = -1;
	for (int i = 0; i < editor_table.size(); i++) {
		if (p_editor == editor_table[i]) {
			screen = i;
			break;
		}
	}
	return screen;
}

EditorPlugin *EditorMainScreen::get_selected_plugin() const {
	return selected_plugin;
}

EditorPlugin *EditorMainScreen::get_plugin_by_name(const String &p_plugin_name) const {
	ERR_FAIL_COND_V(!main_editor_plugins.has(p_plugin_name), nullptr);
	return main_editor_plugins[p_plugin_name];
}

bool EditorMainScreen::can_auto_switch_screens() const {
	if (selected_plugin == nullptr) {
		return true;
	}
	// Only allow auto-switching if the selected button is to the left of the Script button.
	for (int i = 0; i < button_hb->get_child_count(); i++) {
		Button *button = Object::cast_to<Button>(button_hb->get_child(i));
		if (button->get_text() == "Script") {
			// Selected button is at or after the Script button.
			return false;
		}
		if (button->get_text() == selected_plugin->get_plugin_name()) {
			// Selected button is before the Script button.
			return true;
		}
	}
	return false;
}

VBoxContainer *EditorMainScreen::get_control() const {
	return main_screen_vbox;
}

void EditorMainScreen::add_main_plugin(EditorPlugin *p_editor) {
	Button *tb = memnew(Button);
	tb->set_toggle_mode(true);
	tb->set_theme_type_variation("MainScreenButton");
	tb->set_name(p_editor->get_plugin_name());
	tb->set_text(p_editor->get_plugin_name());

	Ref<Shortcut> shortcut = EditorSettings::get_singleton()->get_shortcut("editor/editor_" + p_editor->get_plugin_name().to_lower().replace_char(' ', '_'));
	if (shortcut.is_valid()) {
		tb->set_shortcut(shortcut);
	}

	Ref<Texture2D> icon = p_editor->get_plugin_icon();
	if (icon.is_null() && has_theme_icon(p_editor->get_plugin_name(), EditorStringName(EditorIcons))) {
		icon = get_editor_theme_icon(p_editor->get_plugin_name());
	}
	if (icon.is_valid()) {
		tb->set_button_icon(icon);
		// Make sure the control is updated if the icon is reimported.
		icon->connect_changed(callable_mp((Control *)tb, &Control::update_minimum_size));
	}

	tb->connect(SceneStringName(pressed), callable_mp(this, &EditorMainScreen::select).bind(buttons.size()));

	buttons.push_back(tb);
	button_hb->add_child(tb);
	editor_table.push_back(p_editor);
	main_editor_plugins.insert(p_editor->get_plugin_name(), p_editor);

	// G2 M7.2b: scenes and scripts are the workspace's job now (panes + tabs), so the 2D / 3D / Script
	// strip buttons are redundant switchers. Hide them — the strip carries only the singleton screens
	// (Game / AssetLib / third-party). The buttons stay in buttons[]/editor_table so the index-based
	// legacy shims (select(EDITOR_2D/3D/SCRIPT) from platform/mono) and feature-profile bookkeeping
	// keep working; only their visibility changes. focus_editor("2D"/"3D"/"Script") already routes to
	// the workspace, not these buttons.
	const String pname = p_editor->get_plugin_name();
	if (pname == "2D" || pname == "3D" || pname == "Script") {
		tb->hide();
		tb->set_meta("_g2_strip_retired", true); // marker so set_button_enabled keeps it hidden.
	} else if (pname == "Game" || pname == "AssetLib" || pname == "Asset Store") {
		// G4: on-demand singleton screens — kept OFF the permanent strip. Game gets strip presence
		// only while an embedded game is running (GameView drives it); the asset library opens from
		// Editor → Open Asset Store. reveal_main_plugin() is the only thing that shows them.
		// ("Asset Store" is upstream's 4.8 rename of the AssetLib plugin; both names kept matched.)
		tb->hide();
		tb->set_meta("_g2_on_demand", true);
	}
}

void EditorMainScreen::remove_main_plugin(EditorPlugin *p_editor) {
	// Remove the main editor button and update the bindings of
	// all buttons behind it to point to the correct main window.
	for (int i = buttons.size() - 1; i >= 0; i--) {
		if (p_editor->get_plugin_name() == buttons[i]->get_text()) {
			if (buttons[i]->is_pressed()) {
				select(EDITOR_SCRIPT);
			}

			memdelete(buttons[i]);
			buttons.remove_at(i);

			break;
		} else {
			buttons[i]->disconnect(SceneStringName(pressed), callable_mp(this, &EditorMainScreen::select));
			buttons[i]->connect(SceneStringName(pressed), callable_mp(this, &EditorMainScreen::select).bind(i - 1));
		}
	}

	if (selected_plugin == p_editor) {
		selected_plugin = nullptr;
	}

	editor_table.erase(p_editor);
	main_editor_plugins.erase(p_editor->get_plugin_name());
}

EditorMainScreen::EditorMainScreen() {
	main_screen_vbox = memnew(VBoxContainer);
	main_screen_vbox->set_name("MainScreen");
	main_screen_vbox->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	main_screen_vbox->add_theme_constant_override("separation", 0);

	// G2 S5.5: hidden holder main_screen_vbox parks under whenever no screen-host view hosts it
	// (D11: get_control() must return the live vbox at every instant). Added BEFORE the workspace
	// on purpose — teardown frees children last-first, so the screen-host view can park the vbox
	// here while the holder is still alive.
	screen_park_holder = memnew(Control);
	screen_park_holder->set_name("ScreenParkHolder");
	screen_park_holder->set_visible(false);
	add_child(screen_park_holder);

	// G4: the screen-host ("Editor") tab is NOT opened at startup — it carries only the legacy
	// singleton screens (AssetLib / Game / third-party main-screen plugins), which are all on-demand
	// now, so an always-present empty tab was just clutter next to the user's scenes. It is summoned
	// on demand by reveal_main_plugin() (Game while an embedded game runs; AssetLib from the Editor
	// menu). Until then main_screen_vbox parks under the hidden (but IN-TREE) holder, so get_control()
	// stays live AND the main-screen plugins parented into it — notably the Node3DEditor singleton,
	// whose gizmo services require SceneTree membership (the _spatial_editor_group call target) — keep
	// working. The first scene/script reveal lands in the root pane's (initially empty) tab host; the
	// workspace's focused_pane already defaults to the root pane, so it never splits.
	workspace = memnew(EditorWorkspace);
	add_child(workspace);

	screen_host_document = memnew(ScreenHostDocument);
	screen_host_document->set_screen_stack(main_screen_vbox);
	screen_host_document->set_park_holder_id(screen_park_holder->get_instance_id());
	screen_park_holder->add_child(main_screen_vbox); // Park it in-tree; no screen-host view hosts it yet.

	TabbedDocumentHost *root_host = memnew(TabbedDocumentHost);
	workspace->get_root_pane()->set_content(root_host);
}

EditorMainScreen::~EditorMainScreen() {
	// Plain C++ model object (not registered in EditorData) — owned here.
	if (screen_host_document) {
		memdelete(screen_host_document);
		screen_host_document = nullptr;
	}
}
