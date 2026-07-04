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

			select(-1);
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
}

void EditorMainScreen::load_layout_from_config(Ref<ConfigFile> p_config_file, const String &p_section) {
	int selected_main_editor_idx = p_config_file->get_value(p_section, "selected_main_editor_idx", -1);
	if (selected_main_editor_idx >= 0 && selected_main_editor_idx < buttons.size()) {
		callable_mp(this, &EditorMainScreen::select).call_deferred(selected_main_editor_idx);
	}
}

void EditorMainScreen::set_button_enabled(int p_index, bool p_enabled) {
	ERR_FAIL_INDEX(p_index, buttons.size());
	buttons[p_index]->set_visible(p_enabled);
	if (!p_enabled && buttons[p_index]->is_pressed()) {
		select(EDITOR_2D);
	}
}

bool EditorMainScreen::is_button_enabled(int p_index) const {
	ERR_FAIL_INDEX_V(p_index, buttons.size(), false);
	return buttons[p_index]->is_visible();
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

	// G2 S5: script/help documents open as WORKSPACE TABS, not a main-screen switch. If the document
	// already has a tab in any pane, focus it (no-duplicate rule); otherwise resolve a target pane —
	// which, when a script is opened from within a script, is the focused tabbed pane, so the new
	// script lands as a new tab in the SAME pane. G2 S5.5: the screen-host document rides the same
	// path (its tab normally already exists in the root host; after a tab close this re-summons it).
	if (document_type == EditorDocument::TYPE_SCRIPT || document_type == EditorDocument::TYPE_HELP || document_type == EditorDocument::TYPE_SCREEN_HOST) {
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

	Ref<Shortcut> shortcut = EditorSettings::get_singleton()->get_shortcut("editor/editor_" + p_editor->get_plugin_name().to_lower());
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

	// G2 S5.5: the main editor area is the workspace tree, and the root pane hosts a TAB HOST from
	// startup: tab 0 is the screen-host document, whose view carries the legacy main-screen stack
	// (main_screen_vbox) — so the first script/scene reveal opens as a sibling tab in this pane,
	// never a split. Plugins/addons still parent into get_control() == main_screen_vbox.
	workspace = memnew(EditorWorkspace);
	add_child(workspace);

	screen_host_document = memnew(ScreenHostDocument);
	screen_host_document->set_screen_stack(main_screen_vbox);
	screen_host_document->set_park_holder_id(screen_park_holder->get_instance_id());

	TabbedDocumentHost *root_host = memnew(TabbedDocumentHost);
	workspace->get_root_pane()->set_content(root_host);
	root_host->add_document(screen_host_document, screen_host_document->get_path().get_file());
}

EditorMainScreen::~EditorMainScreen() {
	// Plain C++ model object (not registered in EditorData) — owned here.
	if (screen_host_document) {
		memdelete(screen_host_document);
		screen_host_document = nullptr;
	}
}
