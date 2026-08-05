/**************************************************************************/
/*  editor_card.h                                                        */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                          */
/**************************************************************************/

#pragma once

#include "editor/gui/components/editor_action.h"
#include "scene/gui/panel_container.h"

class Control;
class EditorActionButton;
class EditorSectionHeader;
class HBoxContainer;
class HSeparator;
class MarginContainer;
class VBoxContainer;

class EditorCard : public PanelContainer {
	GDCLASS(EditorCard, PanelContainer);

	EditorSectionHeader *header = nullptr;
	HSeparator *header_divider = nullptr;
	MarginContainer *body_margin = nullptr;
	VBoxContainer *body = nullptr;
	PanelContainer *footer_panel = nullptr;
	HBoxContainer *footer = nullptr;

	void _header_toggled(bool p_collapsed);
	void _update_regions();

protected:
	static void _bind_methods();

public:
	void set_title(const String &p_title);
	String get_title() const;

	void set_description(const String &p_description);
	String get_description() const;

	void set_badge(const String &p_badge);
	String get_badge() const;

	void set_collapsible(bool p_collapsible);
	bool is_collapsible() const;

	void set_collapsed(bool p_collapsed);
	bool is_collapsed() const;

	EditorSectionHeader *get_header() const;
	VBoxContainer *get_body() const;
	HBoxContainer *get_footer() const;

	void add_body_control(Control *p_control);
	void add_footer_control(Control *p_control);
	EditorActionButton *add_header_action(const Ref<EditorAction> &p_action);
	EditorActionButton *add_footer_action(const Ref<EditorAction> &p_action, bool p_emphasized = false);
	void clear_body();
	void clear_footer();

	EditorCard();
};
