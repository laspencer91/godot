/**************************************************************************/
/*  editor_toolbar.cpp                                                    */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                          */
/**************************************************************************/

#include "editor_toolbar.h"

#include "core/object/class_db.h"
#include "editor/gui/components/editor_component_utils.h"
#include "scene/gui/box_container.h"
#include "scene/gui/separator.h"

HBoxContainer *EditorToolbar::_get_or_create_group(const StringName &p_group) {
	const StringName group_name = p_group.is_empty() ? SNAME("primary") : p_group;
	if (groups.has(group_name)) {
		return groups[group_name];
	}

	HBoxContainer *group = memnew(HBoxContainer);
	group->set_theme_type_variation(SNAME("EditorToolbarGroup"));
	add_child(group);
	groups[group_name] = group;
	return group;
}

EditorActionButton *EditorToolbar::add_action(const Ref<EditorAction> &p_action, const StringName &p_group) {
	ERR_FAIL_COND_V(p_action.is_null(), nullptr);
	return editor_component_add_action(p_action, _get_or_create_group(p_group));
}

void EditorToolbar::add_separator(const StringName &p_group) {
	_get_or_create_group(p_group)->add_child(memnew(VSeparator));
}

void EditorToolbar::add_custom_control(Control *p_control, const StringName &p_group) {
	ERR_FAIL_NULL(p_control);
	ERR_FAIL_COND(p_control->get_parent() != nullptr);
	_get_or_create_group(p_group)->add_child(p_control);
}

void EditorToolbar::clear() {
	editor_component_free_children(this);
	groups.clear();
}

void EditorToolbar::_bind_methods() {
	ClassDB::bind_method(D_METHOD("add_action", "action", "group"), &EditorToolbar::add_action, DEFVAL(SNAME("primary")));
	ClassDB::bind_method(D_METHOD("add_separator", "group"), &EditorToolbar::add_separator, DEFVAL(SNAME("primary")));
	ClassDB::bind_method(D_METHOD("add_custom_control", "control", "group"), &EditorToolbar::add_custom_control, DEFVAL(SNAME("primary")));
	ClassDB::bind_method(D_METHOD("clear"), &EditorToolbar::clear);
}

EditorToolbar::EditorToolbar() {}
