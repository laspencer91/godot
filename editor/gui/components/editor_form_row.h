/**************************************************************************/
/*  editor_form_row.h                                                     */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                          */
/**************************************************************************/

#pragma once

#include "scene/gui/box_container.h"

class EditorResponsiveRow;
class HBoxContainer;
class Label;

class EditorFormRow : public VBoxContainer {
	GDCLASS(EditorFormRow, VBoxContainer);

public:
	enum Status {
		STATUS_NONE,
		STATUS_INFO,
		STATUS_SUCCESS,
		STATUS_WARNING,
		STATUS_ERROR,
	};

private:
	EditorResponsiveRow *main_row = nullptr;
	HBoxContainer *label_container = nullptr;
	Label *label_control = nullptr;
	Label *required_indicator = nullptr;
	VBoxContainer *editor_slot = nullptr;
	Label *description_label = nullptr;
	Label *status_label = nullptr;
	Control *editor_control = nullptr;

	String label;
	String description;
	String status_text;
	Status status = STATUS_NONE;
	bool required = false;

	void _update_content();
	void _update_theme();

protected:
	static void _bind_methods();
	void _notification(int p_what);

public:
	void set_label(const String &p_label);
	String get_label() const;

	void set_description(const String &p_description);
	String get_description() const;

	void set_required(bool p_required);
	bool is_required() const;

	void set_editor(Control *p_editor);
	Control *get_editor() const;

	void set_status(Status p_status, const String &p_text);
	Status get_status() const;
	String get_status_text() const;
	void clear_status();

	EditorFormRow();
};

VARIANT_ENUM_CAST(EditorFormRow::Status)
