/**************************************************************************/
/*  editor_form.h                                                         */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                          */
/**************************************************************************/

#pragma once

#include "scene/gui/box_container.h"

class EditorFormSection;

class EditorForm : public VBoxContainer {
	GDCLASS(EditorForm, VBoxContainer);

protected:
	static void _bind_methods();

public:
	EditorFormSection *add_section(const String &p_title = String());
	void clear();

	EditorForm();
};
