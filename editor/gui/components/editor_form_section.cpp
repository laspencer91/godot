/**************************************************************************/
/*  editor_form_section.cpp                                               */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                          */
/**************************************************************************/

#include "editor_form_section.h"

#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "editor/gui/components/editor_card.h"
#include "editor/gui/components/editor_component_utils.h"
#include "editor/gui/components/editor_form_row.h"
#include "editor/gui/components/editor_section_header.h"

void EditorFormSection::_card_toggled(bool p_collapsed) {
	emit_signal(SNAME("toggled"), p_collapsed);
}

void EditorFormSection::set_title(const String &p_title) {
	card->set_title(p_title);
}

String EditorFormSection::get_title() const {
	return card->get_title();
}

void EditorFormSection::set_description(const String &p_description) {
	card->set_description(p_description);
}

String EditorFormSection::get_description() const {
	return card->get_description();
}

void EditorFormSection::set_collapsible(bool p_collapsible) {
	card->set_collapsible(p_collapsible);
}

bool EditorFormSection::is_collapsible() const {
	return card->is_collapsible();
}

void EditorFormSection::set_collapsed(bool p_collapsed) {
	card->set_collapsed(p_collapsed);
}

bool EditorFormSection::is_collapsed() const {
	return card->is_collapsed();
}

EditorFormRow *EditorFormSection::add_row(const String &p_label, Control *p_editor) {
	EditorFormRow *row = memnew(EditorFormRow);
	row->set_label(p_label);
	body->add_child(row);
	if (p_editor) {
		row->set_editor(p_editor);
	}
	return row;
}

void EditorFormSection::add_custom_control(Control *p_control) {
	ERR_FAIL_NULL(p_control);
	ERR_FAIL_COND(p_control->get_parent() != nullptr);
	body->add_child(p_control);
}

void EditorFormSection::clear() {
	editor_component_free_children(body);
}

EditorSectionHeader *EditorFormSection::get_header() const {
	return card->get_header();
}

void EditorFormSection::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_title", "title"), &EditorFormSection::set_title);
	ClassDB::bind_method(D_METHOD("get_title"), &EditorFormSection::get_title);
	ClassDB::bind_method(D_METHOD("set_description", "description"), &EditorFormSection::set_description);
	ClassDB::bind_method(D_METHOD("get_description"), &EditorFormSection::get_description);
	ClassDB::bind_method(D_METHOD("set_collapsible", "collapsible"), &EditorFormSection::set_collapsible);
	ClassDB::bind_method(D_METHOD("is_collapsible"), &EditorFormSection::is_collapsible);
	ClassDB::bind_method(D_METHOD("set_collapsed", "collapsed"), &EditorFormSection::set_collapsed);
	ClassDB::bind_method(D_METHOD("is_collapsed"), &EditorFormSection::is_collapsed);
	ClassDB::bind_method(D_METHOD("add_row", "label", "editor"), &EditorFormSection::add_row, DEFVAL(Variant()));
	ClassDB::bind_method(D_METHOD("add_custom_control", "control"), &EditorFormSection::add_custom_control);
	ClassDB::bind_method(D_METHOD("clear"), &EditorFormSection::clear);
	ClassDB::bind_method(D_METHOD("get_header"), &EditorFormSection::get_header);

	ADD_PROPERTY(PropertyInfo(Variant::STRING, "title"), "set_title", "get_title");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "description", PROPERTY_HINT_MULTILINE_TEXT), "set_description", "get_description");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "collapsible"), "set_collapsible", "is_collapsible");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "collapsed"), "set_collapsed", "is_collapsed");

	ADD_SIGNAL(MethodInfo("toggled", PropertyInfo(Variant::BOOL, "collapsed")));
}

EditorFormSection::EditorFormSection() {
	card = memnew(EditorCard);
	card->connect(SNAME("toggled"), callable_mp(this, &EditorFormSection::_card_toggled));
	add_child(card);
	body = card->get_body();
}
