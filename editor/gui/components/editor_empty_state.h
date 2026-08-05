/**************************************************************************/
/*  editor_empty_state.h                                                  */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                          */
/**************************************************************************/

#pragma once

#include "editor/gui/components/editor_action.h"
#include "scene/gui/center_container.h"

class EditorActionButton;
class HBoxContainer;
class Label;
class TextureRect;
class VBoxContainer;

class EditorEmptyState : public CenterContainer {
	GDCLASS(EditorEmptyState, CenterContainer);

	VBoxContainer *content = nullptr;
	TextureRect *icon_rect = nullptr;
	Label *title_label = nullptr;
	Label *description_label = nullptr;
	HBoxContainer *action_container = nullptr;
	EditorActionButton *primary_button = nullptr;
	EditorActionButton *secondary_button = nullptr;

	StringName icon_name;
	String title;
	String description;

	void _update_icon();
	void _set_action(EditorActionButton *p_button, const Ref<EditorAction> &p_action);

protected:
	static void _bind_methods();
	void _notification(int p_what);

public:
	void set_icon_name(const StringName &p_icon_name);
	StringName get_icon_name() const;

	void set_title(const String &p_title);
	String get_title() const;

	void set_description(const String &p_description);
	String get_description() const;

	void set_primary_action(const Ref<EditorAction> &p_action);
	void set_secondary_action(const Ref<EditorAction> &p_action);

	EditorEmptyState();
};
