/**************************************************************************/
/*  editor_status_panel.cpp                                               */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                          */
/**************************************************************************/

#include "editor_status_panel.h"

#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "editor/editor_string_names.h"
#include "editor/gui/components/editor_action_button.h"
#include "scene/gui/box_container.h"
#include "scene/gui/button.h"
#include "scene/gui/label.h"
#include "scene/gui/texture_rect.h"

void EditorStatusPanel::_dismiss_pressed() {
	hide();
	emit_signal(SNAME("dismissed"));
}

void EditorStatusPanel::_update_content() {
	title_label->set_text(title);
	title_label->set_visible(!title.is_empty());
	message_label->set_text(message);
	message_label->set_visible(!message.is_empty());
	dismiss_button->set_visible(dismissible);
}

void EditorStatusPanel::_update_theme() {
	StringName color_name;
	StringName icon_name;
	switch (severity) {
		case SEVERITY_INFO:
			color_name = SNAME("info_color");
			icon_name = SNAME("NodeInfo");
			break;
		case SEVERITY_SUCCESS:
			color_name = SNAME("success_color");
			icon_name = SNAME("StatusSuccess");
			break;
		case SEVERITY_WARNING:
			color_name = SNAME("warning_color");
			icon_name = SNAME("StatusWarning");
			break;
		case SEVERITY_ERROR:
			color_name = SNAME("error_color");
			icon_name = SNAME("StatusError");
			break;
	}

	title_label->add_theme_color_override(SceneStringName(font_color), get_theme_color(color_name, SNAME("EditorStatusPanel")));
	icon_rect->set_texture(get_editor_theme_icon(icon_name));
	const int icon_size = get_theme_constant(SNAME("icon_size"), SNAME("EditorStatusPanel"));
	icon_rect->set_custom_minimum_size(Size2(icon_size, icon_size));
	message_indent->set_custom_minimum_size(Size2(icon_size, 0));
	dismiss_button->set_button_icon(get_editor_theme_icon(SNAME("Close")));
}

void EditorStatusPanel::_notification(int p_what) {
	if (p_what == NOTIFICATION_THEME_CHANGED) {
		_update_theme();
	}
}

void EditorStatusPanel::set_title(const String &p_title) {
	title = p_title;
	_update_content();
}

String EditorStatusPanel::get_title() const {
	return title;
}

void EditorStatusPanel::set_message(const String &p_message) {
	message = p_message;
	_update_content();
}

String EditorStatusPanel::get_message() const {
	return message;
}

void EditorStatusPanel::set_severity(Severity p_severity) {
	ERR_FAIL_INDEX(p_severity, SEVERITY_ERROR + 1);
	severity = p_severity;
	_update_theme();
}

EditorStatusPanel::Severity EditorStatusPanel::get_severity() const {
	return severity;
}

void EditorStatusPanel::set_dismissible(bool p_dismissible) {
	dismissible = p_dismissible;
	dismiss_button->set_visible(dismissible);
}

bool EditorStatusPanel::is_dismissible() const {
	return dismissible;
}

void EditorStatusPanel::set_action(const Ref<EditorAction> &p_action) {
	if (p_action.is_null()) {
		action_button->hide();
		action_button->set_action(Ref<EditorAction>());
		return;
	}
	action_button->set_action(p_action);
	action_button->show();
}

void EditorStatusPanel::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_title", "title"), &EditorStatusPanel::set_title);
	ClassDB::bind_method(D_METHOD("get_title"), &EditorStatusPanel::get_title);
	ClassDB::bind_method(D_METHOD("set_message", "message"), &EditorStatusPanel::set_message);
	ClassDB::bind_method(D_METHOD("get_message"), &EditorStatusPanel::get_message);
	ClassDB::bind_method(D_METHOD("set_severity", "severity"), &EditorStatusPanel::set_severity);
	ClassDB::bind_method(D_METHOD("get_severity"), &EditorStatusPanel::get_severity);
	ClassDB::bind_method(D_METHOD("set_dismissible", "dismissible"), &EditorStatusPanel::set_dismissible);
	ClassDB::bind_method(D_METHOD("is_dismissible"), &EditorStatusPanel::is_dismissible);
	ClassDB::bind_method(D_METHOD("set_action", "action"), &EditorStatusPanel::set_action);

	ADD_PROPERTY(PropertyInfo(Variant::STRING, "title"), "set_title", "get_title");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "message", PROPERTY_HINT_MULTILINE_TEXT), "set_message", "get_message");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "severity", PROPERTY_HINT_ENUM, "Info,Success,Warning,Error"), "set_severity", "get_severity");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "dismissible"), "set_dismissible", "is_dismissible");

	ADD_SIGNAL(MethodInfo("dismissed"));

	BIND_ENUM_CONSTANT(SEVERITY_INFO);
	BIND_ENUM_CONSTANT(SEVERITY_SUCCESS);
	BIND_ENUM_CONSTANT(SEVERITY_WARNING);
	BIND_ENUM_CONSTANT(SEVERITY_ERROR);
}

EditorStatusPanel::EditorStatusPanel() {
	HBoxContainer *row = memnew(HBoxContainer);
	row->set_theme_type_variation(SNAME("EditorStatusPanelRow"));
	add_child(row);

	VBoxContainer *text_container = memnew(VBoxContainer);
	text_container->set_theme_type_variation(SNAME("EditorStatusText"));
	text_container->set_h_size_flags(SIZE_EXPAND_FILL);
	row->add_child(text_container);

	HBoxContainer *title_row = memnew(HBoxContainer);
	title_row->set_theme_type_variation(SNAME("EditorStatusTitleRow"));
	text_container->add_child(title_row);

	icon_rect = memnew(TextureRect);
	icon_rect->set_stretch_mode(TextureRect::STRETCH_KEEP_ASPECT_CENTERED);
	icon_rect->set_v_size_flags(SIZE_SHRINK_CENTER);
	title_row->add_child(icon_rect);

	title_label = memnew(Label);
	title_label->set_theme_type_variation(SNAME("EditorStatusTitle"));
	title_label->set_h_size_flags(SIZE_EXPAND_FILL);
	title_label->set_v_size_flags(SIZE_SHRINK_CENTER);
	title_row->add_child(title_label);

	dismiss_button = memnew(Button);
	dismiss_button->set_flat(true);
	dismiss_button->set_v_size_flags(SIZE_SHRINK_CENTER);
	dismiss_button->set_default_cursor_shape(CURSOR_POINTING_HAND);
	dismiss_button->set_tooltip_text(TTRC("Dismiss."));
	dismiss_button->connect(SceneStringName(pressed), callable_mp(this, &EditorStatusPanel::_dismiss_pressed));
	title_row->add_child(dismiss_button);

	HBoxContainer *message_row = memnew(HBoxContainer);
	message_row->set_theme_type_variation(SNAME("EditorStatusMessageRow"));
	text_container->add_child(message_row);

	message_indent = memnew(Control);
	message_row->add_child(message_indent);

	message_label = memnew(Label);
	message_label->set_theme_type_variation(SNAME("EditorStatusMessage"));
	message_label->set_h_size_flags(SIZE_EXPAND_FILL);
	message_label->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	message_row->add_child(message_label);

	action_button = memnew(EditorActionButton);
	action_button->set_v_size_flags(SIZE_SHRINK_CENTER);
	action_button->hide();
	row->add_child(action_button);

	_update_content();
}
