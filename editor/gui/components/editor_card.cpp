/**************************************************************************/
/*  editor_card.cpp                                                      */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                          */
/**************************************************************************/

#include "editor_card.h"

#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "editor/gui/components/editor_action_button.h"
#include "editor/gui/components/editor_component_utils.h"
#include "editor/gui/components/editor_section_header.h"
#include "scene/gui/box_container.h"
#include "scene/gui/margin_container.h"
#include "scene/gui/separator.h"

void EditorCard::_header_toggled(bool p_collapsed) {
	_update_regions();
	emit_signal(SNAME("toggled"), p_collapsed);
}

void EditorCard::_update_regions() {
	const bool expanded = !header->is_collapsed();
	header_divider->set_visible(expanded);
	body_margin->set_visible(expanded);
	footer_panel->set_visible(expanded && footer->get_child_count() > 0);
}

void EditorCard::set_title(const String &p_title) {
	header->set_title(p_title);
}

String EditorCard::get_title() const {
	return header->get_title();
}

void EditorCard::set_description(const String &p_description) {
	header->set_description(p_description);
}

String EditorCard::get_description() const {
	return header->get_description();
}

void EditorCard::set_badge(const String &p_badge) {
	header->set_badge(p_badge);
}

String EditorCard::get_badge() const {
	return header->get_badge();
}

void EditorCard::set_collapsible(bool p_collapsible) {
	header->set_collapsible(p_collapsible);
	_update_regions();
}

bool EditorCard::is_collapsible() const {
	return header->is_collapsible();
}

void EditorCard::set_collapsed(bool p_collapsed) {
	header->set_collapsed(p_collapsed);
	_update_regions();
}

bool EditorCard::is_collapsed() const {
	return header->is_collapsed();
}

EditorSectionHeader *EditorCard::get_header() const {
	return header;
}

VBoxContainer *EditorCard::get_body() const {
	return body;
}

HBoxContainer *EditorCard::get_footer() const {
	return footer;
}

void EditorCard::add_body_control(Control *p_control) {
	ERR_FAIL_NULL(p_control);
	ERR_FAIL_COND(p_control->get_parent() != nullptr);
	body->add_child(p_control);
}

void EditorCard::add_footer_control(Control *p_control) {
	ERR_FAIL_NULL(p_control);
	ERR_FAIL_COND(p_control->get_parent() != nullptr);
	footer->add_child(p_control);
	_update_regions();
}

EditorActionButton *EditorCard::add_header_action(const Ref<EditorAction> &p_action) {
	return header->add_action(p_action);
}

EditorActionButton *EditorCard::add_footer_action(const Ref<EditorAction> &p_action, bool p_emphasized) {
	EditorActionButton *button = editor_component_add_action(p_action, footer, p_emphasized);
	ERR_FAIL_NULL_V(button, nullptr);
	_update_regions();
	return button;
}

void EditorCard::clear_body() {
	editor_component_free_children(body);
}

void EditorCard::clear_footer() {
	editor_component_free_children(footer);
	_update_regions();
}

void EditorCard::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_title", "title"), &EditorCard::set_title);
	ClassDB::bind_method(D_METHOD("get_title"), &EditorCard::get_title);
	ClassDB::bind_method(D_METHOD("set_description", "description"), &EditorCard::set_description);
	ClassDB::bind_method(D_METHOD("get_description"), &EditorCard::get_description);
	ClassDB::bind_method(D_METHOD("set_badge", "badge"), &EditorCard::set_badge);
	ClassDB::bind_method(D_METHOD("get_badge"), &EditorCard::get_badge);
	ClassDB::bind_method(D_METHOD("set_collapsible", "collapsible"), &EditorCard::set_collapsible);
	ClassDB::bind_method(D_METHOD("is_collapsible"), &EditorCard::is_collapsible);
	ClassDB::bind_method(D_METHOD("set_collapsed", "collapsed"), &EditorCard::set_collapsed);
	ClassDB::bind_method(D_METHOD("is_collapsed"), &EditorCard::is_collapsed);
	ClassDB::bind_method(D_METHOD("get_header"), &EditorCard::get_header);
	ClassDB::bind_method(D_METHOD("get_body"), &EditorCard::get_body);
	ClassDB::bind_method(D_METHOD("get_footer"), &EditorCard::get_footer);
	ClassDB::bind_method(D_METHOD("add_body_control", "control"), &EditorCard::add_body_control);
	ClassDB::bind_method(D_METHOD("add_footer_control", "control"), &EditorCard::add_footer_control);
	ClassDB::bind_method(D_METHOD("add_header_action", "action"), &EditorCard::add_header_action);
	ClassDB::bind_method(D_METHOD("add_footer_action", "action", "emphasized"), &EditorCard::add_footer_action, DEFVAL(false));
	ClassDB::bind_method(D_METHOD("clear_body"), &EditorCard::clear_body);
	ClassDB::bind_method(D_METHOD("clear_footer"), &EditorCard::clear_footer);

	ADD_PROPERTY(PropertyInfo(Variant::STRING, "title"), "set_title", "get_title");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "description", PROPERTY_HINT_MULTILINE_TEXT), "set_description", "get_description");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "badge"), "set_badge", "get_badge");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "collapsible"), "set_collapsible", "is_collapsible");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "collapsed"), "set_collapsed", "is_collapsed");

	ADD_SIGNAL(MethodInfo("toggled", PropertyInfo(Variant::BOOL, "collapsed")));
}

EditorCard::EditorCard() {
	VBoxContainer *layout = memnew(VBoxContainer);
	layout->set_theme_type_variation(SNAME("EditorCardLayout"));
	add_child(layout);

	PanelContainer *header_panel = memnew(PanelContainer);
	header_panel->set_theme_type_variation(SNAME("EditorCardHeader"));
	layout->add_child(header_panel);

	header = memnew(EditorSectionHeader);
	header->connect(SNAME("toggled"), callable_mp(this, &EditorCard::_header_toggled));
	header_panel->add_child(header);

	header_divider = memnew(HSeparator);
	header_divider->set_theme_type_variation(SNAME("EditorCardDivider"));
	layout->add_child(header_divider);

	body_margin = memnew(MarginContainer);
	body_margin->set_theme_type_variation(SNAME("EditorCardBody"));
	layout->add_child(body_margin);

	body = memnew(VBoxContainer);
	body->set_theme_type_variation(SNAME("EditorCardBodyContent"));
	body_margin->add_child(body);

	footer_panel = memnew(PanelContainer);
	footer_panel->set_theme_type_variation(SNAME("EditorCardFooter"));
	layout->add_child(footer_panel);

	footer = memnew(HBoxContainer);
	footer->set_theme_type_variation(SNAME("EditorCardFooterContent"));
	footer->set_alignment(BoxContainer::ALIGNMENT_END);
	footer_panel->add_child(footer);

	_update_regions();
}
