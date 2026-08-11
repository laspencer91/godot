/**************************************************************************/
/*  editor_scene_actions_menu.cpp                                         */
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

#include "editor_scene_actions_menu.h"

#include "core/object/callable_mp.h"
#include "editor/docks/scene_tree_dock.h"
#include "editor/editor_node.h"
#include "editor/editor_string_names.h"
#include "editor/gui/components/editor_component_utils.h"
#include "editor/themes/editor_scale.h"
#include "scene/gui/box_container.h"
#include "scene/gui/label.h"
#include "scene/gui/popup.h"
#include "scene/gui/scroll_container.h"
#include "scene/gui/separator.h"

Node *EditorSceneActionsMenu::_resolve_node(ObjectID p_node_id) {
	Node *scene_root = EditorNode::get_singleton()->get_edited_scene();
	if (!scene_root) {
		return nullptr;
	}
	Node *node = ObjectDB::get_instance<Node>(p_node_id);
	if (!node) {
		return nullptr;
	}
	// The entry was discovered in a scene that may have been closed, reloaded or
	// edited since the popup opened.
	if (node != scene_root && !scene_root->is_ancestor_of(node)) {
		return nullptr;
	}
	return node;
}

void EditorSceneActionsMenu::_open_popup() {
	popup->set_position(get_screen_position() + Vector2(0, get_size().y));
	popup->reset_size();
	popup->popup();

	// Default geometric focus traversal then walks the rows with the arrow keys.
	if (first_enabled_button) {
		first_enabled_button->grab_focus();
	}
}

void EditorSceneActionsMenu::_rebuild() {
	editor_component_free_children(rows);
	entries.clear();
	first_enabled_button = nullptr;

	Node *scene_root = EditorNode::get_singleton()->get_edited_scene();
	EditorSceneActionRegistry *registry = EditorSceneActionRegistry::get_singleton();
	if (scene_root && registry) {
		registry->collect(scene_root, entries);
	}

	const bool has_entries = !entries.is_empty();
	empty_label->set_visible(!has_entries);
	scroll->set_visible(has_entries);

	const real_t min_width = 320 * EDSCALE;
	empty_label->set_custom_minimum_size(Size2(min_width, 0));
	if (!has_entries) {
		scroll->set_custom_minimum_size(Size2());
		return;
	}

	const Color dim_color = get_theme_color(SNAME("font_disabled_color"), EditorStringName(Editor));
	LocalVector<Button *> node_buttons;
	ObjectID previous_node_id;

	for (uint32_t i = 0; i < entries.size(); i++) {
		const EditorSceneActionEntry &entry = entries[i];

		// Rows are in scene-tree order, so a node's actions are adjacent; break
		// the groups apart whenever the owning node changes.
		if (i > 0 && entry.node_id != previous_node_id) {
			rows->add_child(memnew(HSeparator));
		}
		previous_node_id = entry.node_id;

		HBoxContainer *row = memnew(HBoxContainer);
		rows->add_child(row);

		Ref<EditorAction> action;
		action.instantiate();
		action->set_action_id(entry.action_id);
		action->set_text(entry.label);
		action->set_icon_name(entry.icon_name);
		action->set_tooltip(entry.enabled ? entry.tooltip : entry.disabled_reason);
		action->set_enabled(entry.enabled);
		action->set_callback(callable_mp(this, &EditorSceneActionsMenu::_activate).bind((int)i));

		EditorActionButton *action_button = editor_component_add_action(action, row);
		action_button->set_h_size_flags(SIZE_EXPAND_FILL);
		action_button->set_text_alignment(HORIZONTAL_ALIGNMENT_LEFT);
		if (entry.enabled && !first_enabled_button) {
			first_enabled_button = action_button;
		}

		row->add_child(memnew(VSeparator));

		Button *node_button = memnew(Button);
		node_button->set_theme_type_variation(SceneStringName(FlatButton));
		node_button->set_text(entry.node_name);
		node_button->set_tooltip_text(String(entry.node_path));
		node_button->set_accessibility_name(vformat(TTR("Select %s in the Scene dock"), entry.node_name));
		node_button->set_text_alignment(HORIZONTAL_ALIGNMENT_LEFT);
		node_button->add_theme_color_override(SceneStringName(font_color), dim_color);
		Node *node = ObjectDB::get_instance<Node>(entry.node_id);
		if (node) {
			node_button->set_button_icon(EditorNode::get_singleton()->get_object_icon(node));
		}
		node_button->connect(SceneStringName(pressed), callable_mp(this, &EditorSceneActionsMenu::_select_node).bind(entry.node_id));
		row->add_child(node_button);
		node_buttons.push_back(node_button);
	}

	// Straighten the VSeparator column: without this the divider is as ragged as
	// the node names are long.
	real_t max_node_button_width = 0;
	for (Button *node_button : node_buttons) {
		max_node_button_width = MAX(max_node_button_width, node_button->get_combined_minimum_size().width);
	}
	for (Button *node_button : node_buttons) {
		node_button->set_custom_minimum_size(Size2(max_node_button_width, 0));
	}

	// The ScrollContainer has no minimum height of its own once vertical
	// scrolling is on, so the popup height is capped here.
	const Size2 rows_min_size = rows->get_combined_minimum_size();
	scroll->set_custom_minimum_size(Size2(min_width, MIN(rows_min_size.height, 480 * EDSCALE)));
}

void EditorSceneActionsMenu::_activate(int p_index) {
	ERR_FAIL_INDEX(p_index, (int)entries.size());
	if (!entries[p_index].enabled) {
		return;
	}

	// Actions pop EditorProgress dialogs and warning modals; running them
	// synchronously underneath a closing popup is a focus/parenting hazard.
	popup->hide();
	callable_mp(this, &EditorSceneActionsMenu::_run).call_deferred(p_index);
}

void EditorSceneActionsMenu::_run(int p_index) {
	ERR_FAIL_INDEX(p_index, (int)entries.size());
	const EditorSceneActionEntry entry = entries[p_index];
	if (!entry.enabled) {
		return;
	}
	if (!_resolve_node(entry.node_id)) {
		return; // The node went away between opening the popup and pressing.
	}

	EditorSceneActionRegistry *registry = EditorSceneActionRegistry::get_singleton();
	ERR_FAIL_NULL(registry);
	registry->invoke(entry);
}

void EditorSceneActionsMenu::_select_node(ObjectID p_node_id) {
	popup->hide();

	Node *node = _resolve_node(p_node_id);
	if (!node || !SceneTreeDock::get_singleton()) {
		return;
	}
	SceneTreeDock::get_singleton()->set_selected(node, true);
}

void EditorSceneActionsMenu::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_THEME_CHANGED: {
			set_button_icon(get_editor_theme_icon(SNAME("Tools")));
			empty_label->add_theme_color_override(SceneStringName(font_color), get_theme_color(SNAME("font_disabled_color"), EditorStringName(Editor)));
		} break;
	}
}

EditorSceneActionsMenu::EditorSceneActionsMenu() {
	set_theme_type_variation(SceneStringName(FlatButton));
	set_text(TTRC("Actions"));
	set_tooltip_text(TTRC("Run an action exposed by a node of the edited scene."));
	set_accessibility_name(TTRC("Scene Actions"));
	connect(SceneStringName(pressed), callable_mp(this, &EditorSceneActionsMenu::_open_popup));

	popup = memnew(PopupPanel);
	add_child(popup, false, INTERNAL_MODE_FRONT);
	popup->connect(SNAME("about_to_popup"), callable_mp(this, &EditorSceneActionsMenu::_rebuild));

	VBoxContainer *content = memnew(VBoxContainer);
	popup->add_child(content);

	empty_label = memnew(Label);
	empty_label->set_text(TTRC("No actions in this scene."));
	empty_label->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);
	empty_label->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	content->add_child(empty_label);

	scroll = memnew(ScrollContainer);
	scroll->set_horizontal_scroll_mode(ScrollContainer::SCROLL_MODE_DISABLED);
	scroll->set_follow_focus(true);
	content->add_child(scroll);

	rows = memnew(VBoxContainer);
	rows->set_h_size_flags(SIZE_EXPAND_FILL);
	rows->add_theme_constant_override(SNAME("separation"), 0);
	scroll->add_child(rows);
}
