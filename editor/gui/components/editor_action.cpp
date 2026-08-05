/**************************************************************************/
/*  editor_action.cpp                                                     */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                          */
/**************************************************************************/

#include "editor_action.h"

#include "core/object/class_db.h"

void EditorAction::_changed() {
	emit_signal(SNAME("changed"));
}

void EditorAction::set_action_id(const StringName &p_action_id) {
	if (action_id == p_action_id) {
		return;
	}
	action_id = p_action_id;
	_changed();
}

StringName EditorAction::get_action_id() const {
	return action_id;
}

void EditorAction::set_text(const String &p_text) {
	if (text == p_text) {
		return;
	}
	text = p_text;
	_changed();
}

String EditorAction::get_text() const {
	return text;
}

void EditorAction::set_tooltip(const String &p_tooltip) {
	if (tooltip == p_tooltip) {
		return;
	}
	tooltip = p_tooltip;
	_changed();
}

String EditorAction::get_tooltip() const {
	return tooltip;
}

void EditorAction::set_icon_name(const StringName &p_icon_name) {
	if (icon_name == p_icon_name) {
		return;
	}
	icon_name = p_icon_name;
	_changed();
}

StringName EditorAction::get_icon_name() const {
	return icon_name;
}

void EditorAction::set_shortcut(const Ref<Shortcut> &p_shortcut) {
	if (shortcut == p_shortcut) {
		return;
	}
	shortcut = p_shortcut;
	_changed();
}

Ref<Shortcut> EditorAction::get_shortcut() const {
	return shortcut;
}

void EditorAction::set_callback(const Callable &p_callback) {
	callback = p_callback;
}

Callable EditorAction::get_callback() const {
	return callback;
}

void EditorAction::set_enabled(bool p_enabled) {
	if (enabled == p_enabled) {
		return;
	}
	enabled = p_enabled;
	_changed();
}

bool EditorAction::is_enabled() const {
	return enabled;
}

void EditorAction::set_visible(bool p_visible) {
	if (visible == p_visible) {
		return;
	}
	visible = p_visible;
	_changed();
}

bool EditorAction::is_visible() const {
	return visible;
}

void EditorAction::set_checkable(bool p_checkable) {
	if (checkable == p_checkable) {
		return;
	}
	checkable = p_checkable;
	if (!checkable) {
		checked = false;
	}
	_changed();
}

bool EditorAction::is_checkable() const {
	return checkable;
}

void EditorAction::set_checked(bool p_checked) {
	if (checked == p_checked) {
		return;
	}
	checked = p_checked;
	_changed();
}

bool EditorAction::is_checked() const {
	return checked;
}

void EditorAction::trigger() {
	if (!enabled || !visible) {
		return;
	}

	if (checkable) {
		checked = !checked;
		_changed();
	}

	emit_signal(SNAME("triggered"));
	if (callback.is_valid()) {
		callback.call();
	}
}

void EditorAction::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_action_id", "action_id"), &EditorAction::set_action_id);
	ClassDB::bind_method(D_METHOD("get_action_id"), &EditorAction::get_action_id);
	ClassDB::bind_method(D_METHOD("set_text", "text"), &EditorAction::set_text);
	ClassDB::bind_method(D_METHOD("get_text"), &EditorAction::get_text);
	ClassDB::bind_method(D_METHOD("set_tooltip", "tooltip"), &EditorAction::set_tooltip);
	ClassDB::bind_method(D_METHOD("get_tooltip"), &EditorAction::get_tooltip);
	ClassDB::bind_method(D_METHOD("set_icon_name", "icon_name"), &EditorAction::set_icon_name);
	ClassDB::bind_method(D_METHOD("get_icon_name"), &EditorAction::get_icon_name);
	ClassDB::bind_method(D_METHOD("set_shortcut", "shortcut"), &EditorAction::set_shortcut);
	ClassDB::bind_method(D_METHOD("get_shortcut"), &EditorAction::get_shortcut);
	ClassDB::bind_method(D_METHOD("set_callback", "callback"), &EditorAction::set_callback);
	ClassDB::bind_method(D_METHOD("get_callback"), &EditorAction::get_callback);
	ClassDB::bind_method(D_METHOD("set_enabled", "enabled"), &EditorAction::set_enabled);
	ClassDB::bind_method(D_METHOD("is_enabled"), &EditorAction::is_enabled);
	ClassDB::bind_method(D_METHOD("set_visible", "visible"), &EditorAction::set_visible);
	ClassDB::bind_method(D_METHOD("is_visible"), &EditorAction::is_visible);
	ClassDB::bind_method(D_METHOD("set_checkable", "checkable"), &EditorAction::set_checkable);
	ClassDB::bind_method(D_METHOD("is_checkable"), &EditorAction::is_checkable);
	ClassDB::bind_method(D_METHOD("set_checked", "checked"), &EditorAction::set_checked);
	ClassDB::bind_method(D_METHOD("is_checked"), &EditorAction::is_checked);
	ClassDB::bind_method(D_METHOD("trigger"), &EditorAction::trigger);

	ADD_PROPERTY(PropertyInfo(Variant::STRING_NAME, "action_id"), "set_action_id", "get_action_id");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "text"), "set_text", "get_text");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "tooltip"), "set_tooltip", "get_tooltip");
	ADD_PROPERTY(PropertyInfo(Variant::STRING_NAME, "icon_name"), "set_icon_name", "get_icon_name");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "shortcut", PROPERTY_HINT_RESOURCE_TYPE, "Shortcut"), "set_shortcut", "get_shortcut");
	ADD_PROPERTY(PropertyInfo(Variant::CALLABLE, "callback"), "set_callback", "get_callback");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "enabled"), "set_enabled", "is_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "visible"), "set_visible", "is_visible");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "checkable"), "set_checkable", "is_checkable");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "checked"), "set_checked", "is_checked");

	ADD_SIGNAL(MethodInfo("changed"));
	ADD_SIGNAL(MethodInfo("triggered"));
}
