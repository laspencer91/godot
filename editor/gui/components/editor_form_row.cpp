/**************************************************************************/
/*  editor_form_row.cpp                                                   */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                          */
/**************************************************************************/

#include "editor_form_row.h"

#include "core/object/class_db.h"
#include "editor/editor_string_names.h"
#include "editor/gui/editor_responsive_row.h"
#include "scene/gui/box_container.h"
#include "scene/gui/label.h"

void EditorFormRow::_update_content() {
	label_control->set_text(label);
	required_indicator->set_visible(required);
	description_label->set_text(description);
	description_label->set_visible(!description.is_empty());
	status_label->set_text(status_text);
	status_label->set_visible(status != STATUS_NONE && !status_text.is_empty());
}

void EditorFormRow::_update_theme() {
	label_container->set_custom_minimum_size(Size2(get_theme_constant(SNAME("label_minimum_width"), SNAME("EditorFormRow")), 0));

	StringName color_name;
	switch (status) {
		case STATUS_INFO:
			color_name = SNAME("info_color");
			break;
		case STATUS_SUCCESS:
			color_name = SNAME("success_color");
			break;
		case STATUS_WARNING:
			color_name = SNAME("warning_color");
			break;
		case STATUS_ERROR:
			color_name = SNAME("error_color");
			break;
		case STATUS_NONE:
			status_label->remove_theme_color_override(SceneStringName(font_color));
			return;
	}
	status_label->add_theme_color_override(SceneStringName(font_color), get_theme_color(color_name, SNAME("EditorFormRow")));
}

void EditorFormRow::_notification(int p_what) {
	if (p_what == NOTIFICATION_THEME_CHANGED) {
		_update_theme();
	}
}

void EditorFormRow::set_label(const String &p_label) {
	label = p_label;
	_update_content();
}

String EditorFormRow::get_label() const {
	return label;
}

void EditorFormRow::set_description(const String &p_description) {
	description = p_description;
	_update_content();
}

String EditorFormRow::get_description() const {
	return description;
}

void EditorFormRow::set_required(bool p_required) {
	required = p_required;
	_update_content();
}

bool EditorFormRow::is_required() const {
	return required;
}

void EditorFormRow::set_editor(Control *p_editor) {
	ERR_FAIL_NULL(p_editor);
	ERR_FAIL_COND(p_editor->get_parent() != nullptr && p_editor != editor_control);
	ERR_FAIL_COND_MSG(editor_control != nullptr && editor_control != p_editor, "EditorFormRow already has an editor control.");
	if (editor_control == p_editor) {
		return;
	}
	editor_control = p_editor;
	editor_control->set_h_size_flags(SIZE_EXPAND_FILL);
	if (editor_control->get_theme_type_variation().is_empty()) {
		if (editor_control->is_class("LineEdit")) {
			editor_control->set_theme_type_variation(SNAME("EditorFieldLineEdit"));
		} else if (editor_control->is_class("OptionButton")) {
			editor_control->set_theme_type_variation(SNAME("EditorFieldOptionButton"));
		}
	}
	if (editor_control->get_accessibility_name().is_empty()) {
		editor_control->set_accessibility_name(label);
	}
	editor_slot->add_child(editor_control);
}

Control *EditorFormRow::get_editor() const {
	return editor_control;
}

void EditorFormRow::set_status(Status p_status, const String &p_text) {
	ERR_FAIL_INDEX(p_status, STATUS_ERROR + 1);
	status = p_status;
	status_text = p_text;
	_update_content();
	_update_theme();
}

EditorFormRow::Status EditorFormRow::get_status() const {
	return status;
}

String EditorFormRow::get_status_text() const {
	return status_text;
}

void EditorFormRow::clear_status() {
	set_status(STATUS_NONE, String());
}

void EditorFormRow::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_label", "label"), &EditorFormRow::set_label);
	ClassDB::bind_method(D_METHOD("get_label"), &EditorFormRow::get_label);
	ClassDB::bind_method(D_METHOD("set_description", "description"), &EditorFormRow::set_description);
	ClassDB::bind_method(D_METHOD("get_description"), &EditorFormRow::get_description);
	ClassDB::bind_method(D_METHOD("set_required", "required"), &EditorFormRow::set_required);
	ClassDB::bind_method(D_METHOD("is_required"), &EditorFormRow::is_required);
	ClassDB::bind_method(D_METHOD("set_editor", "editor"), &EditorFormRow::set_editor);
	ClassDB::bind_method(D_METHOD("get_editor"), &EditorFormRow::get_editor);
	ClassDB::bind_method(D_METHOD("set_status", "status", "text"), &EditorFormRow::set_status);
	ClassDB::bind_method(D_METHOD("get_status"), &EditorFormRow::get_status);
	ClassDB::bind_method(D_METHOD("get_status_text"), &EditorFormRow::get_status_text);
	ClassDB::bind_method(D_METHOD("clear_status"), &EditorFormRow::clear_status);

	ADD_PROPERTY(PropertyInfo(Variant::STRING, "label"), "set_label", "get_label");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "description", PROPERTY_HINT_MULTILINE_TEXT), "set_description", "get_description");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "required"), "set_required", "is_required");
	BIND_ENUM_CONSTANT(STATUS_NONE);
	BIND_ENUM_CONSTANT(STATUS_INFO);
	BIND_ENUM_CONSTANT(STATUS_SUCCESS);
	BIND_ENUM_CONSTANT(STATUS_WARNING);
	BIND_ENUM_CONSTANT(STATUS_ERROR);
}

EditorFormRow::EditorFormRow() {
	main_row = memnew(EditorResponsiveRow);
	main_row->set_theme_type_variation(SNAME("EditorFormMainRow"));
	add_child(main_row);

	label_container = memnew(HBoxContainer);
	label_container->set_theme_type_variation(SNAME("EditorFormLabelContainer"));
	main_row->add_child(label_container);

	label_control = memnew(Label);
	label_control->set_theme_type_variation(SNAME("EditorFormLabel"));
	label_control->set_vertical_alignment(VERTICAL_ALIGNMENT_CENTER);
	label_container->add_child(label_control);

	required_indicator = memnew(Label);
	required_indicator->set_theme_type_variation(SNAME("EditorFormRequired"));
	required_indicator->set_text("*");
	required_indicator->set_vertical_alignment(VERTICAL_ALIGNMENT_CENTER);
	required_indicator->hide();
	label_container->add_child(required_indicator);

	editor_slot = memnew(VBoxContainer);
	editor_slot->set_theme_type_variation(SNAME("EditorFormEditorSlot"));
	editor_slot->set_h_size_flags(SIZE_EXPAND_FILL);
	main_row->add_child(editor_slot);

	description_label = memnew(Label);
	description_label->set_theme_type_variation(SNAME("EditorFormDescription"));
	description_label->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	add_child(description_label);

	status_label = memnew(Label);
	status_label->set_theme_type_variation(SNAME("EditorFormStatus"));
	status_label->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	add_child(status_label);

	_update_content();
}
