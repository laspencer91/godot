/**************************************************************************/
/*  editor_pane_header.cpp                                                */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                          */
/**************************************************************************/

#include "editor_pane_header.h"

#include "core/object/class_db.h"
#include "editor/gui/components/editor_action_button.h"
#include "editor/gui/components/editor_component_utils.h"
#include "editor/gui/components/editor_toolbar.h"
#include "scene/gui/box_container.h"
#include "scene/gui/label.h"
#include "scene/gui/texture_rect.h"

void EditorPaneHeader::_update_icon() {
	editor_component_update_icon(this, icon_rect, icon_name);
	// The subtitle sits on its own line, indented to stay under the title.
	subtitle_indent->set_visible(icon_rect->is_visible());
	subtitle_indent->set_custom_minimum_size(Size2(icon_rect->get_custom_minimum_size().x, 0));
}

void EditorPaneHeader::_notification(int p_what) {
	if (p_what == NOTIFICATION_THEME_CHANGED) {
		const int icon_size = get_theme_constant(SNAME("icon_size"), SNAME("EditorPaneHeader"));
		icon_rect->set_custom_minimum_size(Size2(icon_size, icon_size));
		_update_icon();
	}
}

void EditorPaneHeader::set_title(const String &p_title) {
	if (title == p_title) {
		return;
	}
	title = p_title;
	title_label->set_text(title);
}

String EditorPaneHeader::get_title() const {
	return title;
}

void EditorPaneHeader::set_subtitle(const String &p_subtitle) {
	if (subtitle == p_subtitle) {
		return;
	}
	subtitle = p_subtitle;
	subtitle_label->set_text(subtitle);
	subtitle_row->set_visible(!subtitle.is_empty());
}

String EditorPaneHeader::get_subtitle() const {
	return subtitle;
}

void EditorPaneHeader::set_icon_name(const StringName &p_icon_name) {
	if (icon_name == p_icon_name) {
		return;
	}
	icon_name = p_icon_name;
	_update_icon();
}

StringName EditorPaneHeader::get_icon_name() const {
	return icon_name;
}

void EditorPaneHeader::set_dirty(bool p_dirty) {
	if (dirty == p_dirty) {
		return;
	}
	dirty = p_dirty;
	dirty_indicator->set_visible(dirty);
}

bool EditorPaneHeader::is_dirty() const {
	return dirty;
}

EditorActionButton *EditorPaneHeader::add_action(const Ref<EditorAction> &p_action, const StringName &p_group) {
	return toolbar->add_action(p_action, p_group);
}

void EditorPaneHeader::clear_actions() {
	toolbar->clear();
}

void EditorPaneHeader::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_title", "title"), &EditorPaneHeader::set_title);
	ClassDB::bind_method(D_METHOD("get_title"), &EditorPaneHeader::get_title);
	ClassDB::bind_method(D_METHOD("set_subtitle", "subtitle"), &EditorPaneHeader::set_subtitle);
	ClassDB::bind_method(D_METHOD("get_subtitle"), &EditorPaneHeader::get_subtitle);
	ClassDB::bind_method(D_METHOD("set_icon_name", "icon_name"), &EditorPaneHeader::set_icon_name);
	ClassDB::bind_method(D_METHOD("get_icon_name"), &EditorPaneHeader::get_icon_name);
	ClassDB::bind_method(D_METHOD("set_dirty", "dirty"), &EditorPaneHeader::set_dirty);
	ClassDB::bind_method(D_METHOD("is_dirty"), &EditorPaneHeader::is_dirty);
	ClassDB::bind_method(D_METHOD("add_action", "action", "group"), &EditorPaneHeader::add_action, DEFVAL(SNAME("primary")));
	ClassDB::bind_method(D_METHOD("clear_actions"), &EditorPaneHeader::clear_actions);

	ADD_PROPERTY(PropertyInfo(Variant::STRING, "title"), "set_title", "get_title");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "subtitle"), "set_subtitle", "get_subtitle");
	ADD_PROPERTY(PropertyInfo(Variant::STRING_NAME, "icon_name"), "set_icon_name", "get_icon_name");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "dirty"), "set_dirty", "is_dirty");
}

EditorPaneHeader::EditorPaneHeader() {
	VBoxContainer *column = memnew(VBoxContainer);
	add_child(column);

	// Icon, title, dirty chip and actions all live on the title row and center
	// on it, so they stay mutually aligned whether or not a subtitle is shown.
	HBoxContainer *title_row = memnew(HBoxContainer);
	column->add_child(title_row);

	icon_rect = memnew(TextureRect);
	icon_rect->set_stretch_mode(TextureRect::STRETCH_KEEP_ASPECT_CENTERED);
	icon_rect->set_v_size_flags(SIZE_SHRINK_CENTER);
	title_row->add_child(icon_rect);

	title_label = memnew(Label);
	title_label->set_theme_type_variation(SNAME("EditorPaneTitle"));
	title_label->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
	title_label->set_vertical_alignment(VERTICAL_ALIGNMENT_CENTER);
	title_label->set_h_size_flags(SIZE_EXPAND_FILL);
	title_label->set_v_size_flags(SIZE_SHRINK_CENTER);
	title_row->add_child(title_label);

	dirty_indicator = memnew(PanelContainer);
	dirty_indicator->set_theme_type_variation(SNAME("EditorPaneDirtyChip"));
	dirty_indicator->set_v_size_flags(SIZE_SHRINK_CENTER);
	dirty_indicator->set_tooltip_text(TTRC("This item has unsaved changes."));
	dirty_indicator->hide();
	title_row->add_child(dirty_indicator);

	HBoxContainer *dirty_row = memnew(HBoxContainer);
	dirty_indicator->add_child(dirty_row);

	// The bullet is a separate label: concatenating it onto the translated
	// string would make the runtime lookup miss the extracted message.
	Label *dirty_bullet = memnew(Label);
	dirty_bullet->set_theme_type_variation(SNAME("EditorPaneDirtyIndicator"));
	dirty_bullet->set_text(String(U"●"));
	dirty_row->add_child(dirty_bullet);

	Label *dirty_label = memnew(Label);
	dirty_label->set_theme_type_variation(SNAME("EditorPaneDirtyIndicator"));
	dirty_label->set_text(TTRC("Unsaved"));
	dirty_row->add_child(dirty_label);

	toolbar = memnew(EditorToolbar);
	toolbar->set_v_size_flags(SIZE_SHRINK_CENTER);
	title_row->add_child(toolbar);

	subtitle_row = memnew(HBoxContainer);
	subtitle_row->hide();
	column->add_child(subtitle_row);

	subtitle_indent = memnew(Control);
	subtitle_row->add_child(subtitle_indent);

	subtitle_label = memnew(Label);
	subtitle_label->set_theme_type_variation(SNAME("EditorPaneSubtitle"));
	subtitle_label->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
	subtitle_label->set_h_size_flags(SIZE_EXPAND_FILL);
	subtitle_row->add_child(subtitle_label);

	_update_icon();
}
