/**************************************************************************/
/*  editor_form_section.h                                                 */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                          */
/**************************************************************************/

#pragma once

#include "scene/gui/box_container.h"

class EditorFormRow;
class EditorCard;
class EditorSectionHeader;

class EditorFormSection : public VBoxContainer {
	GDCLASS(EditorFormSection, VBoxContainer);

	EditorCard *card = nullptr;
	VBoxContainer *body = nullptr;

	void _card_toggled(bool p_collapsed);

protected:
	static void _bind_methods();

public:
	void set_title(const String &p_title);
	String get_title() const;

	void set_description(const String &p_description);
	String get_description() const;

	void set_collapsible(bool p_collapsible);
	bool is_collapsible() const;

	void set_collapsed(bool p_collapsed);
	bool is_collapsed() const;

	EditorFormRow *add_row(const String &p_label, Control *p_editor = nullptr);
	void add_custom_control(Control *p_control);
	void clear();

	EditorSectionHeader *get_header() const;

	EditorFormSection();
};
