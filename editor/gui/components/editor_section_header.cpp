/**************************************************************************/
/*  editor_section_header.cpp                                             */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                          */
/**************************************************************************/

#include "editor_section_header.h"

#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/input/input_event.h"
#include "editor/gui/components/editor_action_button.h"
#include "editor/gui/components/editor_toolbar.h"
#include "scene/gui/button.h"
#include "scene/gui/label.h"
#include "scene/gui/texture_rect.h"
#include "scene/scene_string_names.h"

void EditorSectionHeader::_collapse_pressed() {
	set_collapsed(!collapsed);
	emit_signal(SceneStringName(toggled), collapsed);
}

void EditorSectionHeader::_update_icon() {
	if (!collapsible || !is_inside_tree()) {
		return;
	}
	const StringName icon_name = collapsed ? SNAME("GuiTreeArrowRight") : SNAME("GuiTreeArrowDown");
	collapse_button->set_button_icon(get_editor_theme_icon(icon_name));
	collapse_button->set_tooltip_text(collapsed ? TTRC("Expand section.") : TTRC("Collapse section."));
}

void EditorSectionHeader::_update_interaction() {
	const CursorShape cursor = collapsible ? CURSOR_POINTING_HAND : CURSOR_ARROW;
	set_default_cursor_shape(cursor);
	title_row->set_default_cursor_shape(cursor);
	description_label->set_default_cursor_shape(cursor);
	set_mouse_filter(collapsible ? MOUSE_FILTER_STOP : MOUSE_FILTER_PASS);
}

void EditorSectionHeader::_update_presentation() {
	StringName header_variation = SNAME("EditorSectionHeader");
	StringName title_variation = SNAME("EditorSectionTitle");
	switch (visual_role) {
		case VISUAL_ROLE_CATEGORY:
			header_variation = SNAME("EditorSectionHeaderCategory");
			title_variation = SNAME("EditorSectionTitleCategory");
			break;
		case VISUAL_ROLE_DIVIDER:
			header_variation = SNAME("EditorSectionHeaderDivider");
			title_variation = SNAME("EditorSectionTitleDivider");
			break;
		case VISUAL_ROLE_SECTION:
			header_variation = SNAME("EditorSectionHeaderSection");
			title_variation = SNAME("EditorSectionTitleSection");
			break;
		case VISUAL_ROLE_SUBSECTION:
			header_variation = SNAME("EditorSectionHeaderSubsection");
			title_variation = SNAME("EditorSectionTitleSubsection");
			break;
		case VISUAL_ROLE_DEFAULT:
			break;
	}

	set_theme_type_variation(header_variation);
	title_label->set_theme_type_variation(title_variation);
	status_label->set_theme_type_variation(SNAME("EditorSectionStatus"));
	if (!is_inside_tree()) {
		return;
	}

	const int hierarchy_step = get_theme_constant(SNAME("hierarchy_step"), header_variation);
	indent_spacer->set_custom_minimum_size(Size2(hierarchy_depth * hierarchy_step, 0));

	const int icon_size = get_theme_constant(SNAME("icon_size"), header_variation);
	leading_icon->set_custom_minimum_size(Size2(icon_size, icon_size));

	const StringName row_separation_name = compact ? SNAME("compact_row_separation") : SNAME("row_separation");
	title_row->add_theme_constant_override(SNAME("separation"), get_theme_constant(row_separation_name, header_variation));
}

void EditorSectionHeader::gui_input(const Ref<InputEvent> &p_event) {
	if (!collapsible) {
		return;
	}

	const Ref<InputEventMouseButton> mouse_button = p_event;
	if (mouse_button.is_valid() && mouse_button->get_button_index() == MouseButton::LEFT && mouse_button->is_pressed()) {
		_collapse_pressed();
		accept_event();
	}
}

void EditorSectionHeader::_notification(int p_what) {
	if (p_what == NOTIFICATION_ENTER_TREE || p_what == NOTIFICATION_THEME_CHANGED || p_what == NOTIFICATION_LAYOUT_DIRECTION_CHANGED || p_what == NOTIFICATION_TRANSLATION_CHANGED) {
		_update_icon();
		_update_presentation();
	}
}

void EditorSectionHeader::set_visual_role(VisualRole p_role) {
	if (visual_role == p_role) {
		return;
	}
	visual_role = p_role;
	_update_presentation();
}

EditorSectionHeader::VisualRole EditorSectionHeader::get_visual_role() const {
	return visual_role;
}

void EditorSectionHeader::set_hierarchy_depth(int p_depth) {
	p_depth = MAX(0, p_depth);
	if (hierarchy_depth == p_depth) {
		return;
	}
	hierarchy_depth = p_depth;
	_update_presentation();
}

int EditorSectionHeader::get_hierarchy_depth() const {
	return hierarchy_depth;
}

void EditorSectionHeader::set_compact(bool p_compact) {
	if (compact == p_compact) {
		return;
	}
	compact = p_compact;
	_update_presentation();
}

bool EditorSectionHeader::is_compact() const {
	return compact;
}

void EditorSectionHeader::set_leading_icon(const Ref<Texture2D> &p_icon) {
	leading_icon->set_texture(p_icon);
	leading_icon->set_visible(p_icon.is_valid());
}

Ref<Texture2D> EditorSectionHeader::get_leading_icon() const {
	return leading_icon->get_texture();
}

void EditorSectionHeader::set_title(const String &p_title) {
	if (title == p_title) {
		return;
	}
	title = p_title;
	title_label->set_text(title);
}

String EditorSectionHeader::get_title() const {
	return title;
}

void EditorSectionHeader::set_description(const String &p_description) {
	if (description == p_description) {
		return;
	}
	description = p_description;
	description_label->set_text(description);
}

String EditorSectionHeader::get_description() const {
	return description;
}

void EditorSectionHeader::set_badge(const String &p_badge) {
	if (badge == p_badge) {
		return;
	}
	badge = p_badge;
	badge_label->set_text(badge);
	badge_label->set_visible(!badge.is_empty());
}

String EditorSectionHeader::get_badge() const {
	return badge;
}

void EditorSectionHeader::set_status(const String &p_status) {
	if (status == p_status) {
		return;
	}
	status = p_status;
	status_label->set_text(status);
	status_label->set_visible(!status.is_empty());
}

String EditorSectionHeader::get_status() const {
	return status;
}

void EditorSectionHeader::set_collapsible(bool p_collapsible) {
	if (collapsible == p_collapsible) {
		return;
	}
	collapsible = p_collapsible;
	if (!collapsible) {
		collapsed = false;
	}
	collapse_button->set_visible(collapsible);
	_update_icon();
	_update_interaction();
}

bool EditorSectionHeader::is_collapsible() const {
	return collapsible;
}

void EditorSectionHeader::set_collapsed(bool p_collapsed) {
	const bool new_collapsed = collapsible && p_collapsed;
	if (collapsed == new_collapsed) {
		return;
	}
	collapsed = new_collapsed;
	_update_icon();
}

bool EditorSectionHeader::is_collapsed() const {
	return collapsed;
}

EditorActionButton *EditorSectionHeader::add_action(const Ref<EditorAction> &p_action) {
	return action_toolbar->add_action(p_action);
}

void EditorSectionHeader::clear_actions() {
	action_toolbar->clear();
}

void EditorSectionHeader::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_visual_role", "role"), &EditorSectionHeader::set_visual_role);
	ClassDB::bind_method(D_METHOD("get_visual_role"), &EditorSectionHeader::get_visual_role);
	ClassDB::bind_method(D_METHOD("set_hierarchy_depth", "depth"), &EditorSectionHeader::set_hierarchy_depth);
	ClassDB::bind_method(D_METHOD("get_hierarchy_depth"), &EditorSectionHeader::get_hierarchy_depth);
	ClassDB::bind_method(D_METHOD("set_compact", "compact"), &EditorSectionHeader::set_compact);
	ClassDB::bind_method(D_METHOD("is_compact"), &EditorSectionHeader::is_compact);
	ClassDB::bind_method(D_METHOD("set_leading_icon", "icon"), &EditorSectionHeader::set_leading_icon);
	ClassDB::bind_method(D_METHOD("get_leading_icon"), &EditorSectionHeader::get_leading_icon);
	ClassDB::bind_method(D_METHOD("set_title", "title"), &EditorSectionHeader::set_title);
	ClassDB::bind_method(D_METHOD("get_title"), &EditorSectionHeader::get_title);
	ClassDB::bind_method(D_METHOD("set_description", "description"), &EditorSectionHeader::set_description);
	ClassDB::bind_method(D_METHOD("get_description"), &EditorSectionHeader::get_description);
	ClassDB::bind_method(D_METHOD("set_badge", "badge"), &EditorSectionHeader::set_badge);
	ClassDB::bind_method(D_METHOD("get_badge"), &EditorSectionHeader::get_badge);
	ClassDB::bind_method(D_METHOD("set_status", "status"), &EditorSectionHeader::set_status);
	ClassDB::bind_method(D_METHOD("get_status"), &EditorSectionHeader::get_status);
	ClassDB::bind_method(D_METHOD("set_collapsible", "collapsible"), &EditorSectionHeader::set_collapsible);
	ClassDB::bind_method(D_METHOD("is_collapsible"), &EditorSectionHeader::is_collapsible);
	ClassDB::bind_method(D_METHOD("set_collapsed", "collapsed"), &EditorSectionHeader::set_collapsed);
	ClassDB::bind_method(D_METHOD("is_collapsed"), &EditorSectionHeader::is_collapsed);
	ClassDB::bind_method(D_METHOD("add_action", "action"), &EditorSectionHeader::add_action);
	ClassDB::bind_method(D_METHOD("clear_actions"), &EditorSectionHeader::clear_actions);

	BIND_ENUM_CONSTANT(VISUAL_ROLE_DEFAULT);
	BIND_ENUM_CONSTANT(VISUAL_ROLE_CATEGORY);
	BIND_ENUM_CONSTANT(VISUAL_ROLE_DIVIDER);
	BIND_ENUM_CONSTANT(VISUAL_ROLE_SECTION);
	BIND_ENUM_CONSTANT(VISUAL_ROLE_SUBSECTION);

	ADD_PROPERTY(PropertyInfo(Variant::INT, "visual_role", PROPERTY_HINT_ENUM, "Default,Category,Divider,Section,Subsection"), "set_visual_role", "get_visual_role");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "hierarchy_depth", PROPERTY_HINT_RANGE, "0,16,1,or_greater"), "set_hierarchy_depth", "get_hierarchy_depth");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "compact"), "set_compact", "is_compact");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "leading_icon", PROPERTY_HINT_RESOURCE_TYPE, "Texture2D"), "set_leading_icon", "get_leading_icon");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "title"), "set_title", "get_title");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "description", PROPERTY_HINT_MULTILINE_TEXT), "set_description", "get_description");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "badge"), "set_badge", "get_badge");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "status"), "set_status", "get_status");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "collapsible"), "set_collapsible", "is_collapsible");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "collapsed"), "set_collapsed", "is_collapsed");

	ADD_SIGNAL(MethodInfo("toggled", PropertyInfo(Variant::BOOL, "collapsed")));
}

EditorSectionHeader::EditorSectionHeader() {
	title_row = memnew(HBoxContainer);
	title_row->set_theme_type_variation(SNAME("EditorSectionHeaderRow"));
	add_child(title_row);

	indent_spacer = memnew(Control);
	indent_spacer->set_mouse_filter(MOUSE_FILTER_IGNORE);
	title_row->add_child(indent_spacer);

	collapse_button = memnew(Button);
	collapse_button->set_flat(true);
	collapse_button->set_focus_mode(FOCUS_ACCESSIBILITY);
	collapse_button->set_default_cursor_shape(CURSOR_POINTING_HAND);
	collapse_button->connect(SceneStringName(pressed), callable_mp(this, &EditorSectionHeader::_collapse_pressed));
	collapse_button->hide();
	title_row->add_child(collapse_button);

	leading_icon = memnew(TextureRect);
	leading_icon->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
	leading_icon->set_stretch_mode(TextureRect::STRETCH_KEEP_ASPECT_CENTERED);
	leading_icon->set_mouse_filter(MOUSE_FILTER_IGNORE);
	leading_icon->hide();
	title_row->add_child(leading_icon);

	title_label = memnew(Label);
	title_label->set_theme_type_variation(SNAME("EditorSectionTitle"));
	title_label->set_v_size_flags(SIZE_SHRINK_CENTER);
	title_label->set_mouse_filter(MOUSE_FILTER_IGNORE);
	title_row->add_child(title_label);

	description_label = memnew(Label);
	description_label->set_theme_type_variation(SNAME("EditorSectionDescription"));
	description_label->set_h_size_flags(SIZE_EXPAND_FILL);
	description_label->set_v_size_flags(SIZE_SHRINK_CENTER);
	description_label->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
	description_label->set_mouse_filter(MOUSE_FILTER_IGNORE);
	title_row->add_child(description_label);

	status_label = memnew(Label);
	status_label->set_theme_type_variation(SNAME("EditorSectionStatus"));
	status_label->set_v_size_flags(SIZE_SHRINK_CENTER);
	status_label->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
	status_label->set_mouse_filter(MOUSE_FILTER_IGNORE);
	status_label->hide();
	title_row->add_child(status_label);

	badge_label = memnew(Label);
	badge_label->set_theme_type_variation(SNAME("EditorSectionBadge"));
	badge_label->set_v_size_flags(SIZE_SHRINK_CENTER);
	badge_label->set_mouse_filter(MOUSE_FILTER_IGNORE);
	badge_label->hide();
	title_row->add_child(badge_label);

	action_toolbar = memnew(EditorToolbar);
	action_toolbar->set_v_size_flags(SIZE_SHRINK_CENTER);
	title_row->add_child(action_toolbar);

	_update_interaction();
	_update_presentation();
}
