/**************************************************************************/
/*  editor_status_panel.h                                                 */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                          */
/**************************************************************************/

#pragma once

#include "editor/gui/components/editor_action.h"
#include "scene/gui/panel_container.h"

class Button;
class EditorActionButton;
class Label;
class TextureRect;

class EditorStatusPanel : public PanelContainer {
	GDCLASS(EditorStatusPanel, PanelContainer);

public:
	enum Severity {
		SEVERITY_INFO,
		SEVERITY_SUCCESS,
		SEVERITY_WARNING,
		SEVERITY_ERROR,
	};

private:
	TextureRect *icon_rect = nullptr;
	Label *title_label = nullptr;
	Control *message_indent = nullptr;
	Label *message_label = nullptr;
	EditorActionButton *action_button = nullptr;
	Button *dismiss_button = nullptr;

	String title;
	String message;
	Severity severity = SEVERITY_INFO;
	bool dismissible = false;

	void _dismiss_pressed();
	void _update_content();
	void _update_theme();

protected:
	static void _bind_methods();
	void _notification(int p_what);

public:
	void set_title(const String &p_title);
	String get_title() const;

	void set_message(const String &p_message);
	String get_message() const;

	void set_severity(Severity p_severity);
	Severity get_severity() const;

	void set_dismissible(bool p_dismissible);
	bool is_dismissible() const;

	void set_action(const Ref<EditorAction> &p_action);

	EditorStatusPanel();
};

VARIANT_ENUM_CAST(EditorStatusPanel::Severity)
