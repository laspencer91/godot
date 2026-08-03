/**************************************************************************/
/*  script_editor_plugin.h                                                */
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

#include "core/error/error_list.h"
#include "core/object/script_language.h"
#include "editor/plugins/editor_plugin.h"
#include "editor/script/script_editor_base.h"
#include "scene/gui/dialogs.h"
#include "scene/gui/panel_container.h"
#include "scene/resources/text_file.h"

class CodeTextEditor;
class DocumentView;
class EditorFileDialog;
class EditorHelp;
class EditorHelpSearch;
class FilterLineEdit;
class FindReplaceBar;
class HSplitContainer;
class ItemList;
class MenuButton;
class TabContainer;
class TextureRect;
class Tree;
class VSplitContainer;
class WindowWrapper;
class EditorSyntaxHighlighter;
class ScriptEditorBase;

class ScriptEditorQuickOpen : public ConfirmationDialog {
	GDCLASS(ScriptEditorQuickOpen, ConfirmationDialog);

	FilterLineEdit *search_box = nullptr;
	Tree *search_options = nullptr;
	String function;

	void _update_search();

	Vector<String> functions;

	void _confirmed();
	void _text_changed(const String &p_newtext);

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	void popup_dialog(const Vector<String> &p_functions, bool p_dontclear = false);
	ScriptEditorQuickOpen();
};

class DocumentOutline : public VBoxContainer {
	GDCLASS(DocumentOutline, VBoxContainer);

	ItemList *item_list = nullptr;
	HBoxContainer *buttons_hbox = nullptr;
	FilterLineEdit *filter = nullptr;
	Button *sort_button = nullptr;

	bool members_overview_enabled = false;
	bool help_overview_enabled = false;

	void _toggle_sort(bool p_alphabetic_sort);
	void _item_list_selected(int p_idx);

protected:
	void _notification(int p_what);

public:
	void update_editor_settings();
	void update_outline();
	void update_visibility();

	DocumentOutline();
};

class EditorScriptCodeCompletionCache;
class FindInFiles;

class ScriptEditor : public PanelContainer {
	GDCLASS(ScriptEditor, PanelContainer);

	enum MenuOptions {
		// File.
		FILE_MENU_NEW_SCRIPT,
		FILE_MENU_NEW_TEXTFILE,
		FILE_MENU_OPEN,
		FILE_MENU_REOPEN_CLOSED,
		FILE_MENU_OPEN_RECENT,

		FILE_MENU_SAVE,
		FILE_MENU_SAVE_AS,
		FILE_MENU_SAVE_ALL,

		FILE_MENU_SOFT_RELOAD_TOOL,
		FILE_MENU_COPY_PATH,
		FILE_MENU_COPY_UID,
		FILE_MENU_SHOW_IN_FILE_SYSTEM,

		FILE_MENU_HISTORY_PREV,
		FILE_MENU_HISTORY_NEXT,

		FILE_MENU_THEME_SUBMENU,

		FILE_MENU_CLOSE,
		FILE_MENU_CLOSE_ALL,
		FILE_MENU_CLOSE_OTHER_TABS,
		FILE_MENU_CLOSE_TABS_BELOW,
		FILE_MENU_CLOSE_DOCS,

		FILE_MENU_RUN,

		FILE_MENU_TOGGLE_FILES_PANEL,

		FILE_MENU_MOVE_UP,
		FILE_MENU_MOVE_DOWN,
		FILE_MENU_SORT,

		// Search.
		HELP_SEARCH_FIND,
		HELP_SEARCH_FIND_NEXT,
		HELP_SEARCH_FIND_PREVIOUS,

		SEARCH_IN_FILES,
		REPLACE_IN_FILES,

		SEARCH_HELP,
		SEARCH_WEBSITE,

		// Theme.
		THEME_IMPORT,
		THEME_RELOAD,
		THEME_SAVE_AS,
	};

	enum ScriptSortBy {
		SORT_BY_NAME,
		SORT_BY_PATH,
		SORT_BY_NONE,
	};

	enum ScriptListName {
		DISPLAY_NAME,
		DISPLAY_DIR_AND_NAME,
		DISPLAY_FULL_PATH,
	};

	HBoxContainer *menu_hb = nullptr;
	MenuButton *file_menu = nullptr;
	MenuButton *script_search_menu = nullptr;
	MenuButton *debug_menu = nullptr;
	Timer *autosave_timer = nullptr;
	LocalVector<Control *> editor_menus;

	PopupMenu *recent_scripts = nullptr;
	PopupMenu *theme_submenu = nullptr;

	Button *help_search = nullptr;
	Button *site_search = nullptr;
	Button *make_floating = nullptr;
	bool is_floating = false;
	EditorHelpSearch *help_search_dialog = nullptr;

	// G2 S7: script_list, the members/help overviews (upstream's DocumentOutline), list_split,
	// script_split, and the internal TabContainer are GONE — the workspace tab bar IS the script
	// list; views live in workspace-tab DocumentViews.

	// G2 S1: the authoritative set of open script VIEWS. Appended in open (edit()) order and released
	// by the owning DocumentView's PREDELETE hook. It deliberately survives scene-tree exit/entry:
	// workspace tab moves and pane splits reparent the live view without closing it.
	Vector<ScriptEditorBase *> registered_views;
	// G2 S6a: the current script view, following the workspace (the focused pane's active script
	// tab, pushed by TabbedDocumentHost/EditorWorkspace via set_current_surface). ObjectID so a
	// closed tab's freed view degrades to "no current view" instead of dangling.
	ObjectID current_view_id;
	// G2 S6b: the current script-or-help surface (superset of current_view_id — EditorHelp is not
	// a ScriptEditorBase). Drives navigation history and the help/search menu state.
	ObjectID current_surface_id;
	// G2 S6b: the authoritative set of open help VIEWS (mirror of registered_views for EditorHelp,
	// which the S1 registry can't hold). It has the same PREDELETE lifetime as script views.
	Vector<EditorHelp *> registered_help_views;
	EditorFileDialog *file_dialog = nullptr;
	AcceptDialog *error_dialog = nullptr;
	ConfirmationDialog *erase_tab_confirm = nullptr;
	ScriptCreateDialog *script_create_dialog = nullptr;
	FindReplaceBar *find_replace_bar = nullptr;

	float zoom_factor = 1.0f;

	HBoxContainer *script_name_button_hbox = nullptr;
	Control *script_name_button_left_spacer = nullptr;
	Control *script_name_button_right_spacer = nullptr;
	Button *script_name_button = nullptr;
	int script_name_width = 0;

	Button *script_back = nullptr;
	Button *script_forward = nullptr;

	FindInFiles *find_in_files = nullptr;

	WindowWrapper *window_wrapper = nullptr;

#ifdef ANDROID_ENABLED
	Control *virtual_keyboard_spacer = nullptr;
	int last_kb_height = -1;
#endif

	enum {
		SCRIPT_EDITOR_FUNC_MAX = 32,
	};

	static int script_editor_func_count;
	static CreateScriptEditorFunc script_editor_funcs[SCRIPT_EDITOR_FUNC_MAX];

	Vector<Ref<EditorSyntaxHighlighter>> syntax_highlighters;

	struct ScriptHistory {
		// G2 S6b: the view by ObjectID — workspace tabs can be closed out from under the history,
		// so records must degrade to "skip" instead of dangling.
		ObjectID control_id;
		Dictionary state;
	};

	Vector<ScriptHistory> history;
	int history_pos;

	List<String> previous_scripts;
	List<ObjectID> script_close_queue; // G2 S7: surfaces by ObjectID (freed views skip silently).

	List<String> _get_recognized_extensions();

	void _menu_option(int p_option);
	void _theme_option(int p_option);
	void _show_save_theme_as_dialog();
	bool _has_docs_tab() const;
	bool _has_script_tab() const;
	void _prepare_file_menu();
	void _file_menu_closed();

	Tree *disk_changed_list = nullptr;
	ConfirmationDialog *disk_changed = nullptr;

	bool restoring_layout;

	void _resave_scripts(const String &p_str);

	bool _test_script_times_on_disk(Ref<Resource> p_for_script = Ref<Resource>());
	bool _script_exists(const String &p_path) const;

	void _add_recent_script(const String &p_path);
	void _update_recent_scripts();
	void _open_recent_script(int p_idx);

	void _show_error_dialog(const String &p_path);

	void _register_view(ScriptEditorBase *p_view);
	void _unregister_view(ScriptEditorBase *p_view);

	// G2 S6a: summon p_resource's workspace tab via EditorMainScreen::reveal (minting the
	// ScriptDocument + view on first open) and return the wired view from the registry.
	ScriptEditorBase *_reveal_script_view(const Ref<Resource> &p_resource, bool p_grab_focus);

	// G2 S6b: help twin of _reveal_script_view — summon p_class's help tab via reveal (the
	// DocumentView mints the EditorHelp through create_help_view) and return the view.
	EditorHelp *_reveal_help_view(const String &p_class);
	void _unregister_help_view(EditorHelp *p_view);

	// G2 S6b: the current script-or-help surface (see current_surface_id), legacy tab fallback.
	Control *_get_current_surface() const;

	// G2 S7: THE close path — close a script/help surface's workspace tab. Saving (if p_save and
	// unsaved) happens here; the remaining side effects (state cache, previous_scripts,
	// notify_script_close) run in notify_surface_closing, invoked by TabbedDocumentHost for every
	// user-driven tab close (menu Close and the tab X both converge there).
	void _close_surface(Control *p_surface, bool p_save);
	void _update_find_replace_bar();

	void _close_current_tab(bool p_save = true);
	void _close_discard_current_tab(const String &p_str);
	void _close_docs_tab();
	void _close_other_tabs();
	void _close_tabs_below();
	void _close_all_tabs();
	void _queue_close_surfaces();

	// G2 S7: all open script+help surfaces in registration order (scripts first) — the successor
	// of the script_list ordering for cycling, close-others/below, and menu enablement.
	Vector<Control *> _get_open_surfaces() const;
	void _cycle_script_surface(int p_dir);

	// G2 simplify: registry lookups + the shared save body, each previously duplicated inline.
	ScriptEditorBase *_find_view_for_resource(const Ref<Resource> &p_resource) const;
	EditorHelp *_find_help_view(const String &p_class) const;
	void _save_view(ScriptEditorBase *p_view);

	// G2 simplify: set while _queue_close_surfaces drains, so notify_surface_closing skips its
	// per-surface refresh/layout-save (the queue does ONE refresh after the batch).
	bool closing_surface_batch = false;

	void _copy_script_path();
	void _copy_script_uid();

	void _ask_close_current_unsaved_tab(ScriptEditorBase *current);

	bool grab_focus_block;

	bool pending_auto_reload;
	bool auto_reload_running_scripts;
	Vector<String> script_paths_to_reload;
	void _live_auto_reload_running_scripts();

	void _update_selected_editor_menu();

	int edit_pass;

	void _add_callback(Object *p_obj, const String &p_function, const PackedStringArray &p_args);
	void _res_saved_callback(const Ref<Resource> &p_res);
	void _scene_saved_callback(const String &p_path);
	void _mark_built_in_scripts_as_saved(const String &p_parent_path);

	bool trim_trailing_whitespace_on_save;
	bool trim_final_newlines_on_save;
	bool convert_indent_on_save;
	bool external_editor_active;
	bool highlight_scene_scripts = false;

	void _goto_script_line2(int p_line);
	void _goto_script_line(Ref<RefCounted> p_script, int p_line);
	void _change_execution(Ref<RefCounted> p_script, int p_line = -1, bool p_set = false);
	void _set_execution(Ref<RefCounted> p_script, int p_line) { _change_execution(p_script, p_line, true); }
	void _clear_execution(Ref<RefCounted> p_script) { _change_execution(p_script); }
	String _get_debug_tooltip(const String &p_text, Node *p_se);
	void _script_created(Ref<Script> p_script);
	void _set_breakpoint(Ref<RefCounted> p_script, int p_line, bool p_enabled);
	void _clear_breakpoints();
	Array _get_cached_breakpoints_for_script(const String &p_path) const;

	ScriptEditorBase *_get_current_editor() const;
	TypedArray<ScriptEditorBase> _get_open_script_editors() const;

	Ref<ConfigFile> script_editor_cache;
	void _save_editor_state(ScriptEditorBase *p_editor);
	void _save_layout();
	void _apply_editor_settings();
	void _filesystem_changed();
	void _files_moved(const String &p_old_file, const String &p_new_file);
	void _file_removed(const String &p_file);
	void _autosave_scripts();
	void _update_autosave_timer();
	void _reload_scripts(bool p_refresh_only = false);
	void _auto_format_text(ScriptEditorBase *p_seb);

	// Upstream (bf898d1bb7): scene-script highlighting refresh hooks — replaced the old
	// `tree_changed` connection. Harmless in the workspace model (kept for the READY wiring;
	// _update_script_names only refreshes the toolbar name button now).
	void _connect_to_scene();
	void _connect_to_scene_recursive(Node *p_current, Node *p_base);
	void _queue_update_script_names();
	void _update_script_names();

	void _update_online_doc();

	virtual void input(const Ref<InputEvent> &p_event) override;
	virtual void shortcut_input(const Ref<InputEvent> &p_event) override;

	void _calculate_script_name_button_size();
	void _calculate_script_name_button_ratio();

	void _help_search(const String &p_text);

	void _history_forward();
	void _history_back();

	bool script_names_update_queued = false;

	void _help_class_open(const String &p_class);
	void _help_class_goto(const String &p_desc);
	bool _help_tab_goto(const String &p_name, const String &p_desc);
	void _update_history_arrows();
	// Upstream (902035ee81) history engine ported to the workspace: records reference views by
	// ObjectID (G2 S6b) instead of raw Control pointers; _go_to_tab/_roll_back_to_pre_tab stayed
	// behind with the retired internal TabContainer.
	void _save_history(Control *p_control);
	void _save_new_history(const Dictionary &p_state, Control *p_control);
	void _save_previous_state(const Dictionary &p_state, Control *p_control);
	void _compress_history_patterns(bool p_once);
	void _update_history_pos(int p_new_pos);
	void _update_modified_scripts_for_external_editor(Ref<Script> p_for_script = Ref<Script>());

	// G2 S7 (seam #8): the shared chrome (menu_hb strip + find_replace_bar) reparents into the
	// focused script/help tab's DocumentView, parked back home (menu_home / find_bar_home) when
	// no script/help tab is current. Shortcut contexts travel with the mount so menu shortcuts
	// fire from inside the workspace tab.
	Node *menu_home = nullptr;
	Node *find_bar_home = nullptr;
	void _mount_chrome(DocumentView *p_view);
	void _park_chrome();
	void _set_chrome_shortcut_context(Node *p_context);

	void _script_changed();
	int file_dialog_option;
	void _file_dialog_action(const String &p_file);

	Ref<Script> _get_current_script();
	TypedArray<Script> _get_open_scripts() const;

	HashSet<String> textfile_extensions;
	Ref<TextFile> _load_text_file(const String &p_path, Error *r_error) const;
	Error _save_text_file(Ref<TextFile> p_text_file, const String &p_path);

	void _on_find_in_files_result_selected(const String &p_path, int p_line_number, int p_begin, int p_end);
	void _on_find_in_files_modified_files();

	void _set_script_zoom_factor(float p_zoom_factor);
	void _update_code_editor_zoom_factor(CodeTextEditor *p_code_text_editor);

	void _window_changed(bool p_visible);

	void _close_builtin_scripts_from_scene(const String &p_scene);

	static ScriptEditor *script_editor;

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	static ScriptEditor *get_singleton() { return script_editor; }

	bool toggle_files_panel();
	bool is_files_panel_toggled();
	void apply_scripts() const;
	void reload_scripts(bool p_refresh_only = false);
	void open_find_in_files_dialog(const String &p_initial_text = "", bool p_replace = false);
	void open_script_create_dialog(const String &p_base_name, const String &p_base_path);
	void open_text_file_create_dialog(const String &p_base_path, const String &p_base_name = "");
	Ref<Resource> open_file(const String &p_file);
	Error close_file(const String &p_file);

	void ensure_select_current();

	bool is_editor_floating();

	_FORCE_INLINE_ bool edit(const Ref<Resource> &p_resource, bool p_grab_focus = true) { return edit(p_resource, -1, 0, p_grab_focus); }
	bool edit(const Ref<Resource> &p_resource, int p_line, int p_col, bool p_grab_focus = true);

	// G2 S2: create + fully wire a script/text view for p_resource, registered in the open-scripts
	// registry but NOT parented into any container. The caller hosts it (edit() adds it to
	// tab_container; a workspace DocumentView adds it to its pane).
	ScriptEditorBase *create_editor_view(const Ref<Resource> &p_resource);

	// G2 S4: a host (e.g. a workspace DocumentView being torn down) releasing a view it hosted.
	// Drops it from the open-scripts registry; the caller frees the Control.
	void release_editor_view(ScriptEditorBase *p_view) { _unregister_view(p_view); }

	// G2 S6b: help twin of create_editor_view — create + wire an EditorHelp view for p_class,
	// registered in the open-help registry but NOT parented (a workspace DocumentView hosts it).
	EditorHelp *create_help_view(const String &p_class);
	void release_help_view(EditorHelp *p_view) { _unregister_help_view(p_view); }

	// Upstream: the active script/help surface (used by DocumentOutline) — the workspace's current
	// surface here.
	Control *get_active_editor() const;

	Vector<String> _get_breakpoints();
	void get_breakpoints(List<String> *p_breakpoints);

	void reload_open_files();
	PackedStringArray get_unsaved_files() const;
	PackedStringArray get_unsaved_scripts() const;
	void save_current_script();
	void save_all_scripts();
	void update_script_times();

	void set_window_layout(Ref<ConfigFile> p_layout);
	void get_window_layout(Ref<ConfigFile> p_layout);

	void set_scene_root_script(Ref<Script> p_script);
	Vector<Ref<Script>> get_open_scripts() const;

	ScriptEditorBase *get_current_editor() const { return _get_current_editor(); }

	// G2 S6a/S6b/S7: pushed by the workspace (tab selection / pane focus) with the active tab's
	// DocumentView so "act on current script" operations follow the focused pane. Classifies the
	// view's surface internally: ScriptEditorBase -> current view+surface, EditorHelp -> surface
	// only, anything else (scene view, screen host, null) clears both. Also mounts/parks the
	// shared chrome (seam #8).
	void set_current_surface(DocumentView *p_view);

	// G2 S7: single choke point for workspace-tab close side effects (state cache, previous
	// scripts, notify_script_close). Called by TabbedDocumentHost just before it frees the
	// closed tab's view — both the tab X and the File menu close paths land here.
	void notify_surface_closing(Control *p_surface);

	// G2 S7: if the shared chrome is currently mounted under p_host (a DocumentView about to be
	// freed), park it back home first so it isn't freed with the host.
	void park_chrome_if_hosted_by(Node *p_host);

	// G2 S6b: focus_editor("Script") intent over the workspace — reveal the current (else most
	// recently opened) script/help tab. False when nothing is open (caller falls back to the
	// legacy Script screen).
	bool reveal_recent_script_or_help();

	bool script_goto_method(Ref<Script> p_script, const String &p_method);

	virtual void edited_scene_changed();

	void notify_script_close(const Ref<Script> &p_script);
	void notify_script_changed(const Ref<Script> &p_script);

	void goto_help(const String &p_desc) { _help_class_goto(p_desc); }
	void update_doc(const String &p_name);
	void clear_docs_from_script(const Ref<Script> &p_script);
	void update_docs_from_script(const Ref<Script> &p_script);

	void trigger_live_script_reload(const String &p_script_path);

	void set_live_auto_reload_running_scripts(bool p_enabled);

	void register_syntax_highlighter(const Ref<EditorSyntaxHighlighter> &p_syntax_highlighter);
	void unregister_syntax_highlighter(const Ref<EditorSyntaxHighlighter> &p_syntax_highlighter);

	static void register_create_script_editor_function(CreateScriptEditorFunc p_func);

	ScriptEditor(WindowWrapper *p_wrapper);
	~ScriptEditor();
};

class ScriptEditorPlugin : public EditorPlugin {
	GDCLASS(ScriptEditorPlugin, EditorPlugin);

	ScriptEditor *script_editor = nullptr;
	WindowWrapper *window_wrapper = nullptr;

	String last_editor;

	void _focus_another_editor();

	void _save_last_editor(const String &p_editor);
	void _window_visibility_changed(bool p_visible);

protected:
	void _notification(int p_what);

public:
	static bool open_in_external_editor(const String &p_path, int p_line, int p_col, bool p_ignore_project = false);

	virtual String get_plugin_name() const override { return TTRC("Script"); }
	bool has_main_screen() const override { return true; }
	virtual void edit(Object *p_object) override;
	virtual bool handles(Object *p_object) const override;
	virtual void make_visible(bool p_visible) override;
	virtual void selected_notify() override;

	virtual String get_unsaved_status(const String &p_for_scene) const override;
	virtual void save_external_data() override;
	virtual void apply_changes() override;

	virtual void set_window_layout(Ref<ConfigFile> p_layout) override;
	virtual void get_window_layout(Ref<ConfigFile> p_layout) override;

	virtual void get_breakpoints(List<String> *p_breakpoints) override;

	virtual void edited_scene_changed() override;

	ScriptEditorPlugin();
};
