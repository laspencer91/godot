/**************************************************************************/
/*  editor_action.h                                                       */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                          */
/**************************************************************************/

#pragma once

#include "core/input/shortcut.h"
#include "core/object/ref_counted.h"
#include "core/variant/callable.h"

class EditorAction : public RefCounted {
	GDCLASS(EditorAction, RefCounted);

	StringName action_id;
	String text;
	String tooltip;
	StringName icon_name;
	Ref<Shortcut> shortcut;
	Callable callback;

	bool enabled = true;
	bool visible = true;
	bool checkable = false;
	bool checked = false;

	void _changed();

protected:
	static void _bind_methods();

public:
	void set_action_id(const StringName &p_action_id);
	StringName get_action_id() const;

	void set_text(const String &p_text);
	String get_text() const;

	void set_tooltip(const String &p_tooltip);
	String get_tooltip() const;

	void set_icon_name(const StringName &p_icon_name);
	StringName get_icon_name() const;

	void set_shortcut(const Ref<Shortcut> &p_shortcut);
	Ref<Shortcut> get_shortcut() const;

	void set_callback(const Callable &p_callback);
	Callable get_callback() const;

	void set_enabled(bool p_enabled);
	bool is_enabled() const;

	void set_visible(bool p_visible);
	bool is_visible() const;

	void set_checkable(bool p_checkable);
	bool is_checkable() const;

	void set_checked(bool p_checked);
	bool is_checked() const;

	void trigger();
};
