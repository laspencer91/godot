/**************************************************************************/
/*  editor_toolbar.h                                                      */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                          */
/**************************************************************************/

#pragma once

#include "core/templates/hash_map.h"
#include "editor/gui/editor_responsive_row.h"

class EditorAction;
class EditorActionButton;
class HBoxContainer;

class EditorToolbar : public EditorResponsiveRow {
	GDCLASS(EditorToolbar, EditorResponsiveRow);

	HashMap<StringName, HBoxContainer *> groups;

	HBoxContainer *_get_or_create_group(const StringName &p_group);

protected:
	static void _bind_methods();

public:
	EditorActionButton *add_action(const Ref<EditorAction> &p_action, const StringName &p_group = SNAME("primary"));
	void add_separator(const StringName &p_group = SNAME("primary"));
	void add_custom_control(Control *p_control, const StringName &p_group = SNAME("primary"));
	void clear();

	EditorToolbar();
};
