/**************************************************************************/
/*  editor_search_bar.h                                                   */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                          */
/**************************************************************************/

#pragma once

#include "editor/gui/components/editor_action.h"
#include "scene/gui/box_container.h"

class EditorToolbar;
class EditorActionButton;
class FilterLineEdit;
class Label;
class LineEdit;
class PanelContainer;
class Timer;
class TextureRect;

class EditorSearchBar : public HBoxContainer {
	GDCLASS(EditorSearchBar, HBoxContainer);

	FilterLineEdit *search_field = nullptr;
	PanelContainer *search_panel = nullptr;
	TextureRect *search_icon = nullptr;
	Label *result_count_label = nullptr;
	EditorToolbar *filter_toolbar = nullptr;
	Timer *debounce_timer = nullptr;

	int result_count = -1;
	int debounce_msec = 150;
	String pending_query;

	void _query_text_changed(const String &p_query);
	void _query_submitted(const String &p_query);
	void _emit_query_changed();
	void _update_result_count();
	void _search_focus_changed(bool p_focused);
	void _update_theme();

protected:
	static void _bind_methods();
	void _notification(int p_what);

public:
	void set_query(const String &p_query);
	String get_query() const;
	void clear_query();

	void set_placeholder(const String &p_placeholder);
	String get_placeholder() const;

	void set_result_count(int p_count);
	int get_result_count() const;

	void set_debounce_msec(int p_msec);
	int get_debounce_msec() const;

	void set_forward_control(Control *p_control);
	LineEdit *get_line_edit() const;

	EditorActionButton *add_filter_action(const Ref<EditorAction> &p_action);

	EditorSearchBar();
};
