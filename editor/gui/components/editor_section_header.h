/**************************************************************************/
/*  editor_section_header.h                                               */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                          */
/**************************************************************************/

#pragma once

#include "editor/gui/components/editor_action.h"
#include "scene/gui/box_container.h"

class Button;
class EditorActionButton;
class EditorToolbar;
class HBoxContainer;
class InputEvent;
class Label;

class EditorSectionHeader : public VBoxContainer {
	GDCLASS(EditorSectionHeader, VBoxContainer);

	Button *collapse_button = nullptr;
	HBoxContainer *title_row = nullptr;
	Label *title_label = nullptr;
	Label *badge_label = nullptr;
	EditorToolbar *action_toolbar = nullptr;
	Label *description_label = nullptr;

	String title;
	String description;
	String badge;
	bool collapsible = false;
	bool collapsed = false;

	void _collapse_pressed();
	void _update_icon();
	void _update_interaction();

protected:
	static void _bind_methods();
	void _notification(int p_what);
	virtual void gui_input(const Ref<InputEvent> &p_event) override;

public:
	void set_title(const String &p_title);
	String get_title() const;

	void set_description(const String &p_description);
	String get_description() const;

	void set_badge(const String &p_badge);
	String get_badge() const;

	void set_collapsible(bool p_collapsible);
	bool is_collapsible() const;

	void set_collapsed(bool p_collapsed);
	bool is_collapsed() const;

	EditorActionButton *add_action(const Ref<EditorAction> &p_action);
	void clear_actions();

	EditorSectionHeader();
};
