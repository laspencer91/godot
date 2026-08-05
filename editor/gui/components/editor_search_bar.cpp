/**************************************************************************/
/*  editor_search_bar.cpp                                                 */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                          */
/**************************************************************************/

#include "editor_search_bar.h"

#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "editor/gui/components/editor_action_button.h"
#include "editor/gui/components/editor_toolbar.h"
#include "editor/gui/filter_line_edit.h"
#include "scene/gui/box_container.h"
#include "scene/gui/label.h"
#include "scene/gui/panel_container.h"
#include "scene/gui/texture_rect.h"
#include "scene/main/timer.h"

void EditorSearchBar::_query_text_changed(const String &p_query) {
	pending_query = p_query;
	debounce_timer->stop();
	if (debounce_msec == 0) {
		_emit_query_changed();
	} else {
		debounce_timer->start(double(debounce_msec) / 1000.0);
	}
}

void EditorSearchBar::_query_submitted(const String &p_query) {
	pending_query = p_query;
	debounce_timer->stop();
	_emit_query_changed();
	emit_signal(SNAME("query_submitted"), p_query);
}

void EditorSearchBar::_emit_query_changed() {
	emit_signal(SNAME("query_changed"), pending_query);
}

void EditorSearchBar::_update_result_count() {
	if (result_count < 0) {
		result_count_label->hide();
		return;
	}
	result_count_label->set_text(vformat(TTRN("%d result", "%d results", result_count), result_count));
	result_count_label->show();
}

void EditorSearchBar::_search_focus_changed(bool p_focused) {
	search_panel->set_theme_type_variation(p_focused ? SNAME("EditorSearchFieldFocused") : SNAME("EditorSearchField"));
}

void EditorSearchBar::_update_theme() {
	search_icon->set_texture(get_editor_theme_icon(SNAME("Search")));
	const int icon_size = get_theme_constant(SNAME("icon_size"), SNAME("EditorSearchBar"));
	search_icon->set_custom_minimum_size(Size2(icon_size, icon_size));
	_search_focus_changed(search_field->has_focus());
}

void EditorSearchBar::_notification(int p_what) {
	if (p_what == NOTIFICATION_THEME_CHANGED) {
		_update_theme();
	} else if (p_what == NOTIFICATION_TRANSLATION_CHANGED) {
		_update_result_count();
	}
}

void EditorSearchBar::set_query(const String &p_query) {
	pending_query = p_query;
	search_field->set_text(p_query);
}

String EditorSearchBar::get_query() const {
	return search_field->get_text();
}

void EditorSearchBar::clear_query() {
	set_query(String());
	debounce_timer->stop();
	_emit_query_changed();
}

void EditorSearchBar::set_placeholder(const String &p_placeholder) {
	search_field->set_placeholder(p_placeholder);
}

String EditorSearchBar::get_placeholder() const {
	return search_field->get_placeholder();
}

void EditorSearchBar::set_result_count(int p_count) {
	if (result_count == p_count) {
		return;
	}
	result_count = p_count;
	_update_result_count();
}

int EditorSearchBar::get_result_count() const {
	return result_count;
}

void EditorSearchBar::set_debounce_msec(int p_msec) {
	debounce_msec = MAX(0, p_msec);
}

int EditorSearchBar::get_debounce_msec() const {
	return debounce_msec;
}

void EditorSearchBar::set_forward_control(Control *p_control) {
	search_field->set_forward_control(p_control);
}

LineEdit *EditorSearchBar::get_line_edit() const {
	return search_field;
}

EditorActionButton *EditorSearchBar::add_filter_action(const Ref<EditorAction> &p_action) {
	filter_toolbar->show();
	return filter_toolbar->add_action(p_action);
}

void EditorSearchBar::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_query", "query"), &EditorSearchBar::set_query);
	ClassDB::bind_method(D_METHOD("get_query"), &EditorSearchBar::get_query);
	ClassDB::bind_method(D_METHOD("clear_query"), &EditorSearchBar::clear_query);
	ClassDB::bind_method(D_METHOD("set_placeholder", "placeholder"), &EditorSearchBar::set_placeholder);
	ClassDB::bind_method(D_METHOD("get_placeholder"), &EditorSearchBar::get_placeholder);
	ClassDB::bind_method(D_METHOD("set_result_count", "count"), &EditorSearchBar::set_result_count);
	ClassDB::bind_method(D_METHOD("get_result_count"), &EditorSearchBar::get_result_count);
	ClassDB::bind_method(D_METHOD("set_debounce_msec", "milliseconds"), &EditorSearchBar::set_debounce_msec);
	ClassDB::bind_method(D_METHOD("get_debounce_msec"), &EditorSearchBar::get_debounce_msec);
	ClassDB::bind_method(D_METHOD("set_forward_control", "control"), &EditorSearchBar::set_forward_control);
	ClassDB::bind_method(D_METHOD("get_line_edit"), &EditorSearchBar::get_line_edit);
	ClassDB::bind_method(D_METHOD("add_filter_action", "action"), &EditorSearchBar::add_filter_action);

	ADD_PROPERTY(PropertyInfo(Variant::STRING, "query"), "set_query", "get_query");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "placeholder"), "set_placeholder", "get_placeholder");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "result_count"), "set_result_count", "get_result_count");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "debounce_msec", PROPERTY_HINT_RANGE, "0,2000,1,suffix:ms"), "set_debounce_msec", "get_debounce_msec");

	ADD_SIGNAL(MethodInfo("query_changed", PropertyInfo(Variant::STRING, "query")));
	ADD_SIGNAL(MethodInfo("query_submitted", PropertyInfo(Variant::STRING, "query")));
}

EditorSearchBar::EditorSearchBar() {
	set_h_size_flags(SIZE_EXPAND_FILL);

	search_panel = memnew(PanelContainer);
	search_panel->set_theme_type_variation(SNAME("EditorSearchField"));
	search_panel->set_h_size_flags(SIZE_EXPAND_FILL);
	search_panel->set_stretch_ratio(1.0);
	add_child(search_panel);

	HBoxContainer *search_row = memnew(HBoxContainer);
	search_row->set_theme_type_variation(SNAME("EditorSearchFieldContent"));
	search_panel->add_child(search_row);

	search_icon = memnew(TextureRect);
	search_icon->set_stretch_mode(TextureRect::STRETCH_KEEP_ASPECT_CENTERED);
	search_row->add_child(search_icon);

	search_field = memnew(FilterLineEdit);
	search_field->set_search_icon_enabled(false);
	search_field->set_theme_type_variation(SNAME("EditorSearchLineEdit"));
	search_field->set_h_size_flags(SIZE_EXPAND_FILL);
	search_field->connect(SceneStringName(text_changed), callable_mp(this, &EditorSearchBar::_query_text_changed));
	search_field->connect(SceneStringName(text_submitted), callable_mp(this, &EditorSearchBar::_query_submitted));
	search_field->connect(SceneStringName(focus_entered), callable_mp(this, &EditorSearchBar::_search_focus_changed).bind(true));
	search_field->connect(SceneStringName(focus_exited), callable_mp(this, &EditorSearchBar::_search_focus_changed).bind(false));
	search_row->add_child(search_field);

	result_count_label = memnew(Label);
	result_count_label->set_theme_type_variation(SNAME("EditorSearchCount"));
	result_count_label->set_vertical_alignment(VERTICAL_ALIGNMENT_CENTER);
	result_count_label->hide();
	search_row->add_child(result_count_label);

	filter_toolbar = memnew(EditorToolbar);
	filter_toolbar->set_h_size_flags(SIZE_SHRINK_END);
	filter_toolbar->hide();
	add_child(filter_toolbar);

	debounce_timer = memnew(Timer);
	debounce_timer->set_one_shot(true);
	debounce_timer->connect(SNAME("timeout"), callable_mp(this, &EditorSearchBar::_emit_query_changed));
	add_child(debounce_timer, false, INTERNAL_MODE_BACK);
}
