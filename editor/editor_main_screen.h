/**************************************************************************/
/*  editor_main_screen.h                                                  */
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

#include "editor/editor_document.h"
#include "scene/gui/panel_container.h"

class Button;
class ConfigFile;
class EditorDocument;
class DocumentView;
class EditorPlugin;
class EditorWorkspace;
class HBoxContainer;
class VBoxContainer;
class WorkspacePane;

class EditorMainScreen : public PanelContainer {
	GDCLASS(EditorMainScreen, PanelContainer);

public:
	enum EditorTable {
		EDITOR_2D = 0,
		EDITOR_3D,
		EDITOR_SCRIPT,
		EDITOR_GAME,
		EDITOR_ASSETLIB,
	};

private:
	EditorWorkspace *workspace = nullptr;
	VBoxContainer *main_screen_vbox = nullptr;
	EditorPlugin *selected_plugin = nullptr;

	// G2 S5.5: the one screen-host document (seam #5) — tab 0 of the root pane's tab host,
	// whose view carries main_screen_vbox. Owned here (plain C++ object, not in EditorData).
	ScreenHostDocument *screen_host_document = nullptr;
	// Hidden holder main_screen_vbox parks under whenever no screen-host view exists, keeping
	// get_control() live (D11). Added as a child BEFORE the workspace: teardown frees children
	// last-first, so the workspace (and its views) dies while the holder is still alive.
	Control *screen_park_holder = nullptr;

	HBoxContainer *button_hb = nullptr;
	Vector<Button *> buttons;
	Vector<EditorPlugin *> editor_table;
	HashMap<String, EditorPlugin *> main_editor_plugins;

	int _get_current_main_editor() const;
	void _select_index(int p_index);

	// G2 M6.2: workspace-session persistence, restored in two phases (see begin_workspace_restore). Phase 1
	// stashes the saved tab set here for phase 2. The explicit pending bit is intentionally independent
	// of the Dictionary: a valid session containing only a transient screen-host has no persistent tabs,
	// but its geometry still needs phase-2 empty-pane cleanup before scene auto-reveal resumes.
	Dictionary _pending_tabs;
	bool _workspace_restore_pending = false;
	// _collect_pane_tabs walks the live tree and records each leaf's tab doc-paths + current (keyed by
	// pane_id) into r_tabs; _populate_pane_tabs (phase 2) fills each rebuilt leaf; _resolve_session_document
	// maps a saved path back to a live document (the screen-host doc locally, else via EditorData);
	// _set_workspace_focus_after_restore drives focus once the panes are filled.
	void _collect_pane_tabs(Dictionary &r_tabs) const;
	void _collapse_transient_only_panes(const Dictionary &p_tabs);
	void _populate_pane_tabs(const Dictionary &p_tabs);
	EditorDocument *_resolve_session_document(const String &p_path);
	void _set_workspace_focus_after_restore();

protected:
	void _notification(int p_what);

public:
	void set_button_container(HBoxContainer *p_button_hb);

	void save_layout_to_config(Ref<ConfigFile> p_config_file, const String &p_section) const;
	void load_layout_from_config(Ref<ConfigFile> p_config_file, const String &p_section);

	// G2 M6.3: the current workspace session blob (geometry + per-pane tabs), for the named-layout store.
	Dictionary get_workspace_blob() const;

	// G2 M6.2: restore phase 1 — rebuild the saved pane geometry + re-home the screen-host BEFORE any
	// scene opens (re-entering the 2D/3D editors with multiple live scene worlds would crash). Must be
	// called ahead of scene restore; load_layout_from_config then runs phase 2 (the scene/script tabs).
	// Returns true if a saved workspace session was found and applied.
	bool begin_workspace_restore(Ref<ConfigFile> p_config_file, const String &p_section);

	// G2 M6.2: true between phase 1 and phase 2 of a workspace-session restore. The M7.1 scene auto-reveal
	// checks this and stands down so the session (not the default reveal) decides each scene's pane.
	bool is_workspace_restore_pending() const { return _workspace_restore_pending; }

	void set_button_enabled(int p_index, bool p_enabled);
	bool is_button_enabled(int p_index) const;
	// Hide an on-demand singleton screen. If it is still the selected screen, also deactivate it and
	// dismiss the transient screen-host tab; another selected singleton screen keeps the host alive.
	void dismiss_main_plugin(int p_index);

	void select_next();
	void select_prev();
	void select_by_name(const String &p_name);
	void select(int p_index);
	// G4: summon an on-demand singleton screen (Game/AssetLib) that isn't kept on the permanent
	// strip -- gives its button strip presence, homes to the screen-host tab, then selects it.
	void reveal_main_plugin(int p_index);
	// G2 S6a: p_grab_focus=false makes sure the document has a (hidden) tab + view without
	// changing the current tab or pane focus — the background-open path.
	void reveal(EditorDocument *p_document, DocumentViewKind p_kind = DocumentViewKind::DEFAULT, bool p_grab_focus = true);

	// G2 S7: close p_document's workspace tab wherever it lives (side effects included via the
	// host's close pipeline). False if no pane shows it.
	bool close_document(EditorDocument *p_document);

	// Mechanical tab removal (no close routing/prompt) — used while p_document is being
	// destroyed (EditorData::remove_scene), so views never outlive the document they bind.
	void drop_document_tabs(EditorDocument *p_document);
	DocumentView *get_document_view(EditorDocument *p_document) const;
	// The command target: current tab in the focused workspace pane, never the legacy active scene.
	DocumentView *get_focused_document_view() const;
	void focus_editor(const StringName &p_name);
	int get_selected_index() const;
	int get_plugin_index(EditorPlugin *p_editor) const;
	EditorPlugin *get_selected_plugin() const;
	EditorPlugin *get_plugin_by_name(const String &p_plugin_name) const;
	bool can_auto_switch_screens() const;

	VBoxContainer *get_control() const;

	void add_main_plugin(EditorPlugin *p_editor);
	void remove_main_plugin(EditorPlugin *p_editor);

	EditorMainScreen();
	~EditorMainScreen();
};
