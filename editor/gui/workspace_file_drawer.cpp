/**************************************************************************/
/*  workspace_file_drawer.cpp                                             */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "workspace_file_drawer.h"

#include "core/io/config_file.h"
#include "core/object/callable_mp.h"
#include "editor/editor_string_names.h"
#include "editor/themes/editor_scale.h"
#include "scene/animation/tween.h"
#include "scene/gui/box_container.h"
#include "scene/gui/button.h"
#include "scene/gui/label.h"
#include "scene/gui/tab_container.h"
#include "scene/resources/style_box_flat.h"
#include "scene/scene_string_names.h"

void WorkspaceFileDrawer::set_host(Control *p_host) {
	if (host == p_host) {
		return;
	}
	const Callable cb = callable_mp(this, &WorkspaceFileDrawer::_relayout);
	if (host && host->is_connected(SceneStringName(item_rect_changed), cb)) {
		host->disconnect(SceneStringName(item_rect_changed), cb);
	}
	host = p_host;
	if (host) {
		host->connect(SceneStringName(item_rect_changed), cb);
	}
	_relayout();
}

void WorkspaceFileDrawer::add_panel(Control *p_panel, const String &p_title) {
	ERR_FAIL_NULL(p_panel);
	if (p_panel->get_parent()) {
		p_panel->get_parent()->remove_child(p_panel);
	}
	tab_host->add_child(p_panel);
	tab_host->set_tab_title(tab_host->get_tab_idx_from_control(p_panel), p_title);
	// A single panel needs no tab strip -- the wrapping tab reads as a strange nested tab (the panel
	// has its own toolbar). Only show tabs once the drawer actually hosts more than one.
	tab_host->set_tabs_visible(tab_host->get_tab_count() > 1);
}

void WorkspaceFileDrawer::_relayout() {
	if (!host || !is_inside_tree()) {
		return;
	}
	if (!is_visible()) {
		// Closed drawers are hidden and never mid-animation (open shows before tweening, close hides
		// after), so skip host-rect churn here; geometry is recomputed the moment we reopen.
		return;
	}
	const Rect2 hr = host->get_global_rect();
	if (hr.size.x <= 0 || hr.size.y <= 0) {
		return;
	}
	const float margin = 10 * EDSCALE;
	const float max_w = 1700 * EDSCALE; // Owner-tuned width for the centered overlay.
	float w = MIN(hr.size.x - 2 * margin, max_w);
	w = MAX(w, 240 * EDSCALE);
	const float h = CLAMP(hr.size.y * height_fraction, 180 * EDSCALE, MAX(180 * EDSCALE, hr.size.y - margin));
	const float x = hr.position.x + (hr.size.x - w) * 0.5f;
	// Open: bottom-aligned inside the host with a small gap. Closed: fully below the host's bottom edge.
	const float open_y = hr.position.y + hr.size.y - h - margin;
	const float closed_y = hr.position.y + hr.size.y;
	const float y = Math::lerp(closed_y, open_y, shown_amount);
	set_size(Size2(w, h));
	set_position(Point2(x, y));
}

void WorkspaceFileDrawer::_set_shown_amount(float p_amount) {
	shown_amount = p_amount;
	_relayout();
}

void WorkspaceFileDrawer::set_open(bool p_open, bool p_animate) {
	if (p_open && !enabled) {
		return; // A profile-disabled drawer never opens (guards the filter-files shortcut etc.).
	}
	const bool was_open = drawer_open;
	drawer_open = p_open;

	if (tween.is_valid()) {
		tween->kill();
		tween = Ref<Tween>();
	}

	if (p_open) {
		set_visible(true);
		_relayout();
	}

	const float target = p_open ? 1.0f : 0.0f;
	if (!p_animate || !is_inside_tree()) {
		_set_shown_amount(target);
		if (!p_open) {
			set_visible(false);
		}
	} else {
		tween = create_tween();
		tween->tween_method(callable_mp(this, &WorkspaceFileDrawer::_set_shown_amount), shown_amount, target, 0.18)
				->set_trans(Tween::TRANS_CUBIC)
				->set_ease(Tween::EASE_OUT);
		if (!p_open) {
			tween->tween_callback(callable_mp(static_cast<CanvasItem *>(this), &CanvasItem::hide));
		}
	}

	if (was_open != drawer_open) {
		emit_signal(SNAME("open_toggled"), drawer_open);
	}
}

void WorkspaceFileDrawer::set_enabled(bool p_enabled) {
	if (enabled == p_enabled) {
		return;
	}
	enabled = p_enabled;
	if (!enabled) {
		set_open(false, false);
	}
}

void WorkspaceFileDrawer::_on_close_pressed() {
	set_open(false);
}

void WorkspaceFileDrawer::_update_theme() {
	if (applying_theme) {
		return; // Guard against the THEME_CHANGED re-entry that add_theme_style_override triggers.
	}
	applying_theme = true;

	Ref<StyleBoxFlat> sb;
	sb.instantiate();
	const Color base = get_theme_color(SNAME("base_color"), EditorStringName(Editor));
	sb->set_bg_color(base.lightened(0.03));
	// Round only the top corners (the bottom sits off-screen). Content is inset well past the radius
	// below so no opaque inner panel reaches into the rounded corner (clip is rectangular, not rounded).
	const int radius = 5 * EDSCALE;
	sb->set_corner_radius(CORNER_TOP_LEFT, radius);
	sb->set_corner_radius(CORNER_TOP_RIGHT, radius);
	sb->set_corner_radius(CORNER_BOTTOM_LEFT, 0);
	sb->set_corner_radius(CORNER_BOTTOM_RIGHT, 0);
	sb->set_border_width_all(Math::round(EDSCALE));
	sb->set_border_color(base.lightened(0.28));
	sb->set_content_margin_all(10 * EDSCALE);
	sb->set_shadow_color(Color(0, 0, 0, 0.38));
	sb->set_shadow_size(6 * EDSCALE);
	add_theme_style_override(SceneStringName(panel), sb);

	if (close_button) {
		close_button->set_button_icon(get_editor_theme_icon(SNAME("Close")));
	}

	applying_theme = false;
}

void WorkspaceFileDrawer::save_state_to_config(Ref<ConfigFile> p_config, const String &p_section) const {
	p_config->set_value(p_section, "file_drawer_open", drawer_open);
	p_config->set_value(p_section, "file_drawer_height", height_fraction);
}

void WorkspaceFileDrawer::load_state_from_config(const Ref<ConfigFile> &p_config, const String &p_section) {
	height_fraction = CLAMP((float)p_config->get_value(p_section, "file_drawer_height", 0.4f), 0.2f, 0.8f);
	const bool want_open = p_config->get_value(p_section, "file_drawer_open", false);
	set_open(want_open, false);
}

void WorkspaceFileDrawer::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE:
		case NOTIFICATION_THEME_CHANGED: {
			_update_theme();
			if (!applying_theme) { // Skip the relayout on the re-entrant THEME_CHANGED from add_theme_style_override.
				_relayout();
			}
		} break;
	}
}

void WorkspaceFileDrawer::_bind_methods() {
	ADD_SIGNAL(MethodInfo("open_toggled", PropertyInfo(Variant::BOOL, "open")));
}

WorkspaceFileDrawer::WorkspaceFileDrawer() {
	set_as_top_level(true);
	set_clip_contents(true);
	set_visible(false);
	set_mouse_filter(MOUSE_FILTER_STOP);

	VBoxContainer *body = memnew(VBoxContainer);
	add_child(body);

	HBoxContainer *header = memnew(HBoxContainer);
	body->add_child(header);

	Label *title_label = memnew(Label);
	title_label->set_text(TTR("FileSystem"));
	title_label->set_h_size_flags(SIZE_EXPAND_FILL);
	header->add_child(title_label);

	close_button = memnew(Button);
	close_button->set_flat(true);
	close_button->set_tooltip_text(TTR("Close"));
	close_button->connect(SceneStringName(pressed), callable_mp(this, &WorkspaceFileDrawer::_on_close_pressed));
	header->add_child(close_button);

	tab_host = memnew(TabContainer);
	tab_host->set_v_size_flags(SIZE_EXPAND_FILL);
	// The tab host's own square content/tab-bar backgrounds would poke through the drawer's rounded
	// corners (clip is rectangular, not rounded). Blank them so only the drawer panel paints there.
	Ref<StyleBoxEmpty> empty_sb;
	empty_sb.instantiate();
	tab_host->add_theme_style_override(SNAME("panel"), empty_sb);
	tab_host->add_theme_style_override(SNAME("tabbar_background"), empty_sb);
	body->add_child(tab_host);
}
