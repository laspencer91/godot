/**************************************************************************/
/*  editor_empty_state.cpp                                                */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                          */
/**************************************************************************/

#include "editor_empty_state.h"

#include "core/object/class_db.h"
#include "editor/gui/components/editor_action_button.h"
#include "editor/gui/components/editor_component_utils.h"
#include "scene/gui/box_container.h"
#include "scene/gui/label.h"
#include "scene/gui/texture_rect.h"

void EditorEmptyState::_update_icon() {
	editor_component_update_icon(this, icon_rect, icon_name);
}

void EditorEmptyState::_set_action(EditorActionButton *p_button, const Ref<EditorAction> &p_action) {
	// The button hides itself when the action is null, so the container only
	// has to follow whether either of them ended up visible.
	p_button->set_action(p_action);
	action_container->set_visible(primary_button->is_visible() || secondary_button->is_visible());
}

void EditorEmptyState::_notification(int p_what) {
	if (p_what == NOTIFICATION_THEME_CHANGED) {
		const int icon_size = get_theme_constant(SNAME("icon_size"), SNAME("EditorEmptyState"));
		icon_rect->set_custom_minimum_size(Size2(icon_size, icon_size));
		content->set_custom_minimum_size(Size2(get_theme_constant(SNAME("content_minimum_width"), SNAME("EditorEmptyState")), 0));
		_update_icon();
	}
}

void EditorEmptyState::set_icon_name(const StringName &p_icon_name) {
	if (icon_name == p_icon_name) {
		return;
	}
	icon_name = p_icon_name;
	_update_icon();
}

StringName EditorEmptyState::get_icon_name() const {
	return icon_name;
}

void EditorEmptyState::set_title(const String &p_title) {
	if (title == p_title) {
		return;
	}
	title = p_title;
	title_label->set_text(title);
	title_label->set_visible(!title.is_empty());
}

String EditorEmptyState::get_title() const {
	return title;
}

void EditorEmptyState::set_description(const String &p_description) {
	if (description == p_description) {
		return;
	}
	description = p_description;
	description_label->set_text(description);
	description_label->set_visible(!description.is_empty());
}

String EditorEmptyState::get_description() const {
	return description;
}

void EditorEmptyState::set_primary_action(const Ref<EditorAction> &p_action) {
	_set_action(primary_button, p_action);
}

void EditorEmptyState::set_secondary_action(const Ref<EditorAction> &p_action) {
	_set_action(secondary_button, p_action);
}

void EditorEmptyState::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_icon_name", "icon_name"), &EditorEmptyState::set_icon_name);
	ClassDB::bind_method(D_METHOD("get_icon_name"), &EditorEmptyState::get_icon_name);
	ClassDB::bind_method(D_METHOD("set_title", "title"), &EditorEmptyState::set_title);
	ClassDB::bind_method(D_METHOD("get_title"), &EditorEmptyState::get_title);
	ClassDB::bind_method(D_METHOD("set_description", "description"), &EditorEmptyState::set_description);
	ClassDB::bind_method(D_METHOD("get_description"), &EditorEmptyState::get_description);
	ClassDB::bind_method(D_METHOD("set_primary_action", "action"), &EditorEmptyState::set_primary_action);
	ClassDB::bind_method(D_METHOD("set_secondary_action", "action"), &EditorEmptyState::set_secondary_action);

	ADD_PROPERTY(PropertyInfo(Variant::STRING_NAME, "icon_name"), "set_icon_name", "get_icon_name");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "title"), "set_title", "get_title");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "description", PROPERTY_HINT_MULTILINE_TEXT), "set_description", "get_description");
}

EditorEmptyState::EditorEmptyState() {
	set_h_size_flags(SIZE_EXPAND_FILL);
	set_v_size_flags(SIZE_EXPAND_FILL);

	content = memnew(VBoxContainer);
	content->set_theme_type_variation(SNAME("EditorEmptyStateContent"));
	content->set_alignment(BoxContainer::ALIGNMENT_CENTER);
	add_child(content);

	icon_rect = memnew(TextureRect);
	icon_rect->set_stretch_mode(TextureRect::STRETCH_KEEP_ASPECT_CENTERED);
	content->add_child(icon_rect);

	title_label = memnew(Label);
	title_label->set_theme_type_variation(SNAME("EditorEmptyStateTitle"));
	title_label->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);
	title_label->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	content->add_child(title_label);

	description_label = memnew(Label);
	description_label->set_theme_type_variation(SNAME("EditorEmptyStateDescription"));
	description_label->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);
	description_label->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	content->add_child(description_label);

	action_container = memnew(HBoxContainer);
	action_container->set_theme_type_variation(SNAME("EditorEmptyStateActions"));
	action_container->set_alignment(BoxContainer::ALIGNMENT_CENTER);
	action_container->hide();
	content->add_child(action_container);

	// Both buttons exist for the component's lifetime; they hide themselves
	// while their action is unset, which also fixes their relative order.
	primary_button = memnew(EditorActionButton);
	primary_button->set_emphasized(true);
	action_container->add_child(primary_button);

	secondary_button = memnew(EditorActionButton);
	action_container->add_child(secondary_button);

	title_label->hide();
	description_label->hide();
}
