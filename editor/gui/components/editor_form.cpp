/**************************************************************************/
/*  editor_form.cpp                                                       */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                          */
/**************************************************************************/

#include "editor_form.h"

#include "core/object/class_db.h"
#include "editor/gui/components/editor_form_section.h"

EditorFormSection *EditorForm::add_section(const String &p_title) {
	EditorFormSection *section = memnew(EditorFormSection);
	section->set_title(p_title);
	add_child(section);
	return section;
}

void EditorForm::clear() {
	while (get_child_count() > 0) {
		memdelete(get_child(0));
	}
}

void EditorForm::_bind_methods() {
	ClassDB::bind_method(D_METHOD("add_section", "title"), &EditorForm::add_section, DEFVAL(String()));
	ClassDB::bind_method(D_METHOD("clear"), &EditorForm::clear);
}

EditorForm::EditorForm() {}
