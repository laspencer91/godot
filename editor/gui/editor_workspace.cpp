/**************************************************************************/
/*  editor_workspace.cpp                                                  */
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

#include "editor_workspace.h"

#include "core/input/input_event.h"
#include "core/object/callable_mp.h"
#include "editor/editor_data.h"
#include "editor/editor_document.h"
#include "editor/editor_node.h"
#include "editor/editor_string_names.h"
#include "editor/gui/tabbed_document_host.h"
#include "editor/script/script_editor_plugin.h"
#include "editor/themes/editor_scale.h"
#include "scene/gui/label.h"
#include "scene/gui/panel_container.h"
#include "scene/gui/split_container.h"
#include "scene/main/viewport.h"

void WorkspacePane::set_content(Control *p_content) {
	if (content == p_content) {
		return;
	}
	if (content && content->get_parent() == this) {
		remove_child(content);
	}
	content = p_content;
	if (content) {
		if (content->get_parent()) {
			content->get_parent()->remove_child(content);
		}
		add_child(content);
		content->set_h_size_flags(SIZE_EXPAND_FILL);
		content->set_v_size_flags(SIZE_EXPAND_FILL);
	}
	set_process_input(content != nullptr);
}

WorkspacePane *WorkspacePane::split(bool p_vertical, Control *p_new_content, bool p_new_on_second) {
	if (!is_leaf()) {
		return nullptr;
	}

	// Detach current content; it will move into one of the two new child panes.
	Control *existing = content;
	if (existing && existing->get_parent() == this) {
		remove_child(existing);
	}
	content = nullptr;
	set_process_input(false);

	split_container = memnew(SplitContainer);
	split_container->set_vertical(p_vertical);
	split_container->set_h_size_flags(SIZE_EXPAND_FILL);
	split_container->set_v_size_flags(SIZE_EXPAND_FILL);
	add_child(split_container);

	first = workspace ? workspace->make_pane() : memnew(WorkspacePane);
	second = workspace ? workspace->make_pane() : memnew(WorkspacePane);
	split_container->add_child(first);
	split_container->add_child(second);

	WorkspacePane *existing_pane = p_new_on_second ? first : second;
	WorkspacePane *new_pane = p_new_on_second ? second : first;
	existing_pane->set_content(existing);
	new_pane->set_content(p_new_content);
	return new_pane;
}

void WorkspacePane::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_DRAW: {
			if (!workspace || workspace->get_focused_pane() != this || workspace->get_root_pane()->get_leaf_count() <= 1) {
				return;
			}

			// G2 M1.1: workspace focus is a pane-level concept; draw only when multiple panes exist.
			const Color accent_color = get_theme_color(SNAME("accent_color"), EditorStringName(Editor));
			draw_rect(Rect2(Vector2(), get_size()).grow(-1), accent_color, false, 2 * EDSCALE);
		} break;
	}
}

void WorkspacePane::input(const Ref<InputEvent> &p_event) {
	if (!workspace || !is_leaf() || !is_visible_in_tree()) {
		return;
	}

	Ref<InputEventMouseButton> mb = p_event;
	if (mb.is_null() || !mb->is_pressed()) {
		return;
	}

	if (get_global_rect().has_point(mb->get_position())) {
		workspace->set_focused_pane(this);
	}
}

int WorkspacePane::get_leaf_count() const {
	if (is_leaf()) {
		return 1;
	}

	int count = 0;
	if (first) {
		count += first->get_leaf_count();
	}
	if (second) {
		count += second->get_leaf_count();
	}
	return count;
}

WorkspacePane::WorkspacePane() {
	set_h_size_flags(SIZE_EXPAND_FILL);
	set_v_size_flags(SIZE_EXPAND_FILL);
	add_theme_constant_override("separation", 0);
}

void EditorWorkspace::_bind_methods() {
	ADD_SIGNAL(MethodInfo("focused_pane_changed", PropertyInfo(Variant::OBJECT, "pane", PROPERTY_HINT_NODE_TYPE, "WorkspacePane")));
}

WorkspacePane *EditorWorkspace::make_pane() {
	WorkspacePane *pane = memnew(WorkspacePane);
	pane->set_workspace(this);
	return pane;
}

void EditorWorkspace::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			set_process_unhandled_key_input(true);
		} break;
		case NOTIFICATION_READY: {
			Viewport *viewport = get_viewport();
			if (viewport) {
				viewport->connect("gui_focus_changed", callable_mp(this, &EditorWorkspace::_on_gui_focus_changed));
			}
		} break;
	}
}

void EditorWorkspace::_on_gui_focus_changed(Control *p_control) {
	for (Control *control = p_control; control; control = Object::cast_to<Control>(control->get_parent())) {
		WorkspacePane *pane = Object::cast_to<WorkspacePane>(control);
		if (pane && pane->get_workspace() == this && pane->is_leaf()) {
			set_focused_pane(pane);
			return;
		}
	}
}

void EditorWorkspace::unhandled_key_input(const Ref<InputEvent> &p_event) {
	// TEMPORARY (G2 scaffolding): Alt+Shift+Backslash splits the focused pane side
	// by side, Alt+Shift+Minus stacks it. Handled here (unhandled input) so normal
	// editor shortcuts always win; removed once panes host real tabbed content.
	Ref<InputEventKey> k = p_event;
	if (k.is_null() || !k->is_pressed() || k->is_echo()) {
		return;
	}
	if (!(k->is_alt_pressed() && k->is_shift_pressed())) {
		return;
	}
	if (k->get_keycode() == Key::BACKSLASH) {
		_debug_split_focused(false);
		accept_event();
	} else if (k->get_keycode() == Key::MINUS) {
		_debug_split_focused(true);
		accept_event();
	} else if (k->get_keycode() == Key::KEY_3) {
		// Drop a tabbed host of all open documents into a side-by-side split.
		_debug_split_focused_with_tabs(false);
		accept_event();
	}
}

void EditorWorkspace::_debug_split_focused(bool p_vertical) {
	WorkspacePane *target = get_focused_pane();
	if (!target || !target->is_leaf()) {
		return;
	}

	debug_pane_counter++;
	PanelContainer *placeholder = memnew(PanelContainer);
	Label *label = memnew(Label);
	label->set_text(vformat("Pane %d", debug_pane_counter));
	label->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);
	label->set_vertical_alignment(VERTICAL_ALIGNMENT_CENTER);
	label->set_h_size_flags(SIZE_EXPAND_FILL);
	label->set_v_size_flags(SIZE_EXPAND_FILL);
	placeholder->add_child(label);

	WorkspacePane *new_pane = target->split(p_vertical, placeholder);
	if (new_pane) {
		set_focused_pane(new_pane);
	}
}

void EditorWorkspace::_debug_split_focused_with_tabs(bool p_vertical) {
	WorkspacePane *target = get_focused_pane();
	if (!target || !target->is_leaf()) {
		return;
	}
	EditorNode *en = EditorNode::get_singleton();
	if (!en) {
		return;
	}
	EditorData &ed = en->get_editor_data();

	// Build a tabbed host with a tab per open document, so the new pane can switch which
	// document it renders. The active document's tab is selected first.
	TabbedDocumentHost *host = memnew(TabbedDocumentHost);
	EditorDocument *active = ed.get_active_document();
	int active_tab = 0;
	const int count = ed.get_edited_scene_count();
	for (int i = 0; i < count; i++) {
		EditorDocument *d = ed.get_document(i);
		if (!d) {
			continue;
		}
		const int tab = host->add_document(d, ed.get_scene_title(i));
		if (d == active) {
			active_tab = tab;
		}
	}

	// G2 S4: also surface the first open script as a tab, so script-document hosting is exercisable
	// through the debug split. The real reveal()-driven open path lands in S5/S6.
	if (ScriptEditor *se = ScriptEditor::get_singleton()) {
		Vector<Ref<Script>> open_scripts = se->get_open_scripts();
		if (!open_scripts.is_empty() && open_scripts[0].is_valid()) {
			ScriptDocument *sd = ed.get_or_create_script_document(open_scripts[0]);
			host->add_document(sd, open_scripts[0]->get_path().get_file());
		}
	}

	if (host->get_document_count() == 0) {
		memdelete(host);
		return;
	}
	host->set_current(active_tab);

	WorkspacePane *new_pane = target->split(p_vertical, host);
	if (new_pane) {
		set_focused_pane(new_pane);
	}
}

void EditorWorkspace::set_focused_pane(WorkspacePane *p_pane) {
	if (!p_pane) {
		return;
	}

	WorkspacePane *old_focused = focused_pane;
	if (old_focused == p_pane) {
		return;
	}

	focused_pane = p_pane;
	if (Object::cast_to<TabbedDocumentHost>(focused_pane->get_content())) {
		last_tabbed_pane = focused_pane;
	}

	if (old_focused) {
		old_focused->queue_redraw();
	}
	focused_pane->queue_redraw();
	emit_signal(SNAME("focused_pane_changed"), focused_pane);
}

WorkspacePane *EditorWorkspace::_find_pane_showing(WorkspacePane *p_pane, EditorDocument *p_document) const {
	if (!p_pane) {
		return nullptr;
	}
	if (p_pane->is_leaf()) {
		if (TabbedDocumentHost *host = Object::cast_to<TabbedDocumentHost>(p_pane->get_content())) {
			if (host->has_document(p_document)) {
				return p_pane;
			}
		}
		return nullptr;
	}
	if (WorkspacePane *found = _find_pane_showing(p_pane->get_first(), p_document)) {
		return found;
	}
	return _find_pane_showing(p_pane->get_second(), p_document);
}

WorkspacePane *EditorWorkspace::find_pane_showing(EditorDocument *p_document) const {
	if (!p_document) {
		return nullptr;
	}
	return _find_pane_showing(root_pane, p_document);
}

WorkspacePane *EditorWorkspace::resolve_target_pane_for_documents() {
	// (a) The focused pane, if it already hosts tabs — this is what keeps "open a script from a
	// script" in the SAME pane (the focused pane IS a TabbedDocumentHost while you edit a script).
	WorkspacePane *focused = get_focused_pane();
	if (focused && Object::cast_to<TabbedDocumentHost>(focused->get_content())) {
		return focused;
	}
	// (b) The most-recently-focused tabbed leaf.
	if (last_tabbed_pane && last_tabbed_pane->is_leaf() && Object::cast_to<TabbedDocumentHost>(last_tabbed_pane->get_content())) {
		return last_tabbed_pane;
	}
	// (c) Split the focused leaf, minting a fresh tabbed host on the new (second/right) side.
	if (!focused || !focused->is_leaf()) {
		focused = root_pane;
	}
	TabbedDocumentHost *host = memnew(TabbedDocumentHost);
	WorkspacePane *new_pane = focused->split(false, host, true);
	if (new_pane) {
		last_tabbed_pane = new_pane;
		return new_pane;
	}
	memdelete(host); // split failed (target not a leaf); nothing hosts the orphan.
	return nullptr;
}

EditorWorkspace::EditorWorkspace() {
	set_h_size_flags(SIZE_EXPAND_FILL);
	set_v_size_flags(SIZE_EXPAND_FILL);
	add_theme_constant_override("separation", 0);

	root_pane = make_pane();
	add_child(root_pane);
	focused_pane = root_pane;
}
