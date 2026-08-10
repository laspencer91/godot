/**************************************************************************/
/*  editor_section_header.h                                               */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                          */
/**************************************************************************/

#pragma once

#include "editor/gui/components/editor_action.h"
#include "scene/gui/box_container.h"

class Button;
class EditorActionButton;
class EditorToolbar;
class HBoxContainer;
class InputEvent;
class Label;
class Texture2D;
class TextureRect;

class EditorSectionHeader : public VBoxContainer {
	GDCLASS(EditorSectionHeader, VBoxContainer);

public:
	enum VisualRole {
		VISUAL_ROLE_DEFAULT,
		VISUAL_ROLE_CATEGORY,
		VISUAL_ROLE_DIVIDER,
		VISUAL_ROLE_SECTION,
		VISUAL_ROLE_SUBSECTION,
	};

private:

	Button *collapse_button = nullptr;
	HBoxContainer *title_row = nullptr;
	Control *indent_spacer = nullptr;
	TextureRect *leading_icon = nullptr;
	Label *title_label = nullptr;
	Label *badge_label = nullptr;
	Label *status_label = nullptr;
	EditorToolbar *action_toolbar = nullptr;
	Label *description_label = nullptr;

	String title;
	String description;
	String badge;
	String status;
	VisualRole visual_role = VISUAL_ROLE_DEFAULT;
	int hierarchy_depth = 0;
	bool compact = false;
	bool collapsible = false;
	bool collapsed = false;

	void _collapse_pressed();
	void _update_icon();
	void _update_interaction();
	void _update_presentation();

protected:
	static void _bind_methods();
	void _notification(int p_what);
	virtual void gui_input(const Ref<InputEvent> &p_event) override;

public:
	void set_visual_role(VisualRole p_role);
	VisualRole get_visual_role() const;

	void set_hierarchy_depth(int p_depth);
	int get_hierarchy_depth() const;

	void set_compact(bool p_compact);
	bool is_compact() const;

	void set_leading_icon(const Ref<Texture2D> &p_icon);
	Ref<Texture2D> get_leading_icon() const;

	void set_title(const String &p_title);
	String get_title() const;

	void set_description(const String &p_description);
	String get_description() const;

	void set_badge(const String &p_badge);
	String get_badge() const;

	void set_status(const String &p_status);
	String get_status() const;

	void set_collapsible(bool p_collapsible);
	bool is_collapsible() const;

	void set_collapsed(bool p_collapsed);
	bool is_collapsed() const;

	EditorActionButton *add_action(const Ref<EditorAction> &p_action);
	void clear_actions();

	EditorSectionHeader();
};

VARIANT_ENUM_CAST(EditorSectionHeader::VisualRole)
