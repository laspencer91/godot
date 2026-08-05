/**************************************************************************/
/*  editor_action_button.h                                                */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                          */
/**************************************************************************/

#pragma once

#include "editor/gui/components/editor_action.h"
#include "scene/gui/button.h"

class EditorActionButton : public Button {
	GDCLASS(EditorActionButton, Button);

	Ref<EditorAction> action;
	bool emphasized = false;
	bool updating = false;

	void _pressed();
	void _update_action();

protected:
	static void _bind_methods();
	void _notification(int p_what);

public:
	void set_action(const Ref<EditorAction> &p_action);
	Ref<EditorAction> get_action() const;

	void set_emphasized(bool p_emphasized);
	bool is_emphasized() const;

	EditorActionButton();
	~EditorActionButton();
};
