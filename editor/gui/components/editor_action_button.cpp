/**************************************************************************/
/*  editor_action_button.cpp                                              */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                          */
/**************************************************************************/

#include "editor_action_button.h"

#include "core/core_string_names.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"

void EditorActionButton::_pressed() {
	if (updating || action.is_null()) {
		return;
	}
	action->trigger();
}

void EditorActionButton::_update_action() {
	updating = true;

	if (action.is_null()) {
		set_text(String());
		set_tooltip_text(String());
		set_button_icon(Ref<Texture2D>());
		set_shortcut(Ref<Shortcut>());
		set_toggle_mode(false);
		set_pressed_no_signal(false);
		set_disabled(true);
		hide();
		updating = false;
		return;
	}

	set_text(action->get_text());
	set_tooltip_text(action->get_tooltip());
	set_shortcut(action->get_shortcut());
	set_toggle_mode(action->is_checkable());
	set_pressed_no_signal(action->is_checked());
	set_disabled(!action->is_enabled());
	set_visible(action->is_visible());

	const StringName icon_name = action->get_icon_name();
	set_button_icon(icon_name.is_empty() ? Ref<Texture2D>() : get_editor_theme_icon(icon_name));

	updating = false;
}

void EditorActionButton::_notification(int p_what) {
	if (p_what == NOTIFICATION_THEME_CHANGED) {
		set_custom_minimum_size(Size2(get_theme_constant(SNAME("minimum_width")), get_theme_constant(SNAME("minimum_height"))));
	}
	if (p_what == NOTIFICATION_THEME_CHANGED || p_what == NOTIFICATION_TRANSLATION_CHANGED) {
		_update_action();
	}
}

void EditorActionButton::set_action(const Ref<EditorAction> &p_action) {
	if (action == p_action) {
		return;
	}

	const Callable changed_callable = callable_mp(this, &EditorActionButton::_update_action);
	if (action.is_valid() && action->is_connected(CoreStringName(changed), changed_callable)) {
		action->disconnect(CoreStringName(changed), changed_callable);
	}

	action = p_action;
	if (action.is_valid()) {
		action->connect(CoreStringName(changed), changed_callable);
	}
	_update_action();
}

Ref<EditorAction> EditorActionButton::get_action() const {
	return action;
}

void EditorActionButton::set_emphasized(bool p_emphasized) {
	if (emphasized == p_emphasized) {
		return;
	}
	emphasized = p_emphasized;
	set_theme_type_variation(emphasized ? SNAME("EditorActionButtonPrimary") : SNAME("EditorActionButtonFlat"));
}

bool EditorActionButton::is_emphasized() const {
	return emphasized;
}

void EditorActionButton::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_action", "action"), &EditorActionButton::set_action);
	ClassDB::bind_method(D_METHOD("get_action"), &EditorActionButton::get_action);
	ClassDB::bind_method(D_METHOD("set_emphasized", "emphasized"), &EditorActionButton::set_emphasized);
	ClassDB::bind_method(D_METHOD("is_emphasized"), &EditorActionButton::is_emphasized);

	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "action", PROPERTY_HINT_RESOURCE_TYPE, "EditorAction"), "set_action", "get_action");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "emphasized"), "set_emphasized", "is_emphasized");
}

EditorActionButton::EditorActionButton() {
	set_theme_type_variation(SNAME("EditorActionButtonFlat"));
	set_default_cursor_shape(CURSOR_POINTING_HAND);
	connect(SceneStringName(pressed), callable_mp(this, &EditorActionButton::_pressed));
	// Matches the state _update_action() produces for a null action, without
	// paying for a full pass that every construction site immediately redoes.
	set_disabled(true);
	hide();
}

EditorActionButton::~EditorActionButton() {
	if (action.is_valid()) {
		const Callable changed_callable = callable_mp(this, &EditorActionButton::_update_action);
		if (action->is_connected(CoreStringName(changed), changed_callable)) {
			action->disconnect(CoreStringName(changed), changed_callable);
		}
	}
}
