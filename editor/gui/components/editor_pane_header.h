/**************************************************************************/
/*  editor_pane_header.h                                                  */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                          */
/**************************************************************************/

#pragma once

#include "editor/gui/components/editor_action.h"
#include "scene/gui/panel_container.h"

class EditorActionButton;
class EditorToolbar;
class HBoxContainer;
class Label;
class TextureRect;

class EditorPaneHeader : public PanelContainer {
	GDCLASS(EditorPaneHeader, PanelContainer);

	TextureRect *icon_rect = nullptr;
	Label *title_label = nullptr;
	HBoxContainer *subtitle_row = nullptr;
	Control *subtitle_indent = nullptr;
	Label *subtitle_label = nullptr;
	PanelContainer *dirty_indicator = nullptr;
	EditorToolbar *toolbar = nullptr;

	String title;
	String subtitle;
	StringName icon_name;
	bool dirty = false;

	void _update_icon();

protected:
	static void _bind_methods();
	void _notification(int p_what);

public:
	void set_title(const String &p_title);
	String get_title() const;

	void set_subtitle(const String &p_subtitle);
	String get_subtitle() const;

	void set_icon_name(const StringName &p_icon_name);
	StringName get_icon_name() const;

	void set_dirty(bool p_dirty);
	bool is_dirty() const;

	EditorActionButton *add_action(const Ref<EditorAction> &p_action, const StringName &p_group = SNAME("primary"));
	void clear_actions();

	EditorPaneHeader();
};
