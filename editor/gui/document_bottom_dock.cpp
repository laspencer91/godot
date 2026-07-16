/**************************************************************************/
/*  document_bottom_dock.cpp                                              */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             Godot Engine                               */
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

#include "document_bottom_dock.h"

#include "core/object/callable_mp.h"
#include "editor/editor_string_names.h"
#include "editor/themes/editor_scale.h"
#include "scene/gui/button.h"
#include "scene/gui/label.h"
#include "scene/gui/panel_container.h"
#include "scene/gui/split_container.h"
#include "scene/resources/style_box_flat.h"

void DocumentBottomDockHost::_toggle_pressed(bool p_pressed, StringName p_id) {
	set_dock_open(p_id, p_pressed);
	emit_signal(SNAME("dock_user_toggled"), p_id, p_pressed);
}

void DocumentBottomDockHost::_update_theme() {
	const Color base = get_theme_color(SNAME("base_color"), EditorStringName(Editor));
	const Color border = get_theme_color(SNAME("contrast_color_1"), EditorStringName(Editor));

	for (KeyValue<StringName, DockEntry> &E : docks) {
		DockEntry &entry = E.value;
		entry.toggle_button->set_button_icon(get_theme_icon(entry.icon_name, EditorStringName(EditorIcons)));
		entry.close_button->set_button_icon(get_theme_icon(SNAME("Close"), EditorStringName(EditorIcons)));

		Ref<StyleBoxFlat> panel_style;
		panel_style.instantiate();
		panel_style->set_bg_color(base.darkened(0.055));
		panel_style->set_border_color(border);
		panel_style->set_border_width(SIDE_TOP, MAX(1, int(EDSCALE)));
		panel_style->set_content_margin(SIDE_LEFT, 6 * EDSCALE);
		panel_style->set_content_margin(SIDE_RIGHT, 6 * EDSCALE);
		panel_style->set_content_margin(SIDE_TOP, 4 * EDSCALE);
		panel_style->set_content_margin(SIDE_BOTTOM, 4 * EDSCALE);
		entry.panel->add_theme_style_override(SceneStringName(panel), panel_style);
	}
}

void DocumentBottomDockHost::_notification(int p_what) {
	if (p_what == NOTIFICATION_THEME_CHANGED) {
		_update_theme();
	}
}

void DocumentBottomDockHost::_bind_methods() {
	ADD_SIGNAL(MethodInfo("dock_toggled", PropertyInfo(Variant::STRING_NAME, "dock_id"), PropertyInfo(Variant::BOOL, "open")));
	// Separate explicit toolbar/header intent from state restoration and service-driven reveals.
	// Document-owned drawers use this to focus their search field only when the user opened them.
	ADD_SIGNAL(MethodInfo("dock_user_toggled", PropertyInfo(Variant::STRING_NAME, "dock_id"), PropertyInfo(Variant::BOOL, "open")));
}

void DocumentBottomDockHost::set_surface(Control *p_surface) {
	ERR_FAIL_NULL(p_surface);
	ERR_FAIL_COND(p_surface->get_parent());
	// A plain Control intentionally isolates the editor surface's natural minimum size from the
	// split. Otherwise the four-viewport 3D editor can refuse to become shorter in a compact window,
	// causing the drawer to extend below the document while bottom-anchored overlays remain still.
	// The clip follows the split allocation and the surface follows the clip, so opening a drawer
	// always moves overlays such as Floating Camera Preview with the visible viewport.
	surface_clip->add_child(p_surface);
	p_surface->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);
}

void DocumentBottomDockHost::add_dock(const StringName &p_id, const String &p_title, const StringName &p_icon_name, Control *p_content) {
	ERR_FAIL_COND(p_id.is_empty());
	ERR_FAIL_COND(docks.has(p_id));
	ERR_FAIL_NULL(p_content);
	ERR_FAIL_NULL(toggle_host);
	ERR_FAIL_COND(p_content->get_parent());

	DockEntry entry;
	entry.icon_name = p_icon_name;

	entry.toggle_button = memnew(Button);
	entry.toggle_button->set_name(String(p_id) + "BottomDockToggle");
	entry.toggle_button->set_toggle_mode(true);
	entry.toggle_button->set_flat(true);
	entry.toggle_button->set_focus_mode(FOCUS_NONE);
	entry.toggle_button->set_tooltip_text(vformat(TTR("Toggle %s drawer"), p_title));
	entry.toggle_button->set_accessibility_name(vformat(TTR("Toggle %s drawer"), p_title));
	entry.toggle_button->connect(SceneStringName(toggled), callable_mp(this, &DocumentBottomDockHost::_toggle_pressed).bind(p_id));
	toggle_host->add_child(entry.toggle_button);

	entry.panel = memnew(PanelContainer);
	entry.panel->set_name(String(p_id) + "BottomDockPanel");
	entry.panel->set_custom_minimum_size(Size2(0, 280 * EDSCALE));
	entry.panel->set_h_size_flags(SIZE_EXPAND_FILL);
	// EXPAND, not just FILL: the panel is the lone child of the drawer_stack VBox, so with FILL it would
	// sit at its 280px minimum and leave the rest of the drawer region (everything the user drags open
	// past 280) empty. EXPAND lets it grow to fill whatever height the split hands the drawer, so the
	// track editor inside grows with it. The custom minimum still floors the drawer at 280.
	entry.panel->set_v_size_flags(SIZE_EXPAND_FILL);
	entry.panel->hide();

	VBoxContainer *drawer_vbox = memnew(VBoxContainer);
	drawer_vbox->add_theme_constant_override("separation", 4 * EDSCALE);
	entry.panel->add_child(drawer_vbox);

	HBoxContainer *header = memnew(HBoxContainer);
	drawer_vbox->add_child(header);

	Label *title = memnew(Label);
	title->set_text(p_title);
	title->set_h_size_flags(SIZE_EXPAND_FILL);
	title->set_theme_type_variation(SNAME("HeaderSmall"));
	header->add_child(title);

	entry.close_button = memnew(Button);
	entry.close_button->set_flat(true);
	entry.close_button->set_focus_mode(FOCUS_NONE);
	entry.close_button->set_tooltip_text(vformat(TTR("Close %s drawer"), p_title));
	entry.close_button->connect(SceneStringName(pressed), callable_mp(this, &DocumentBottomDockHost::_toggle_pressed).bind(false, p_id));
	header->add_child(entry.close_button);

	p_content->set_h_size_flags(SIZE_EXPAND_FILL);
	p_content->set_v_size_flags(SIZE_EXPAND_FILL);
	drawer_vbox->add_child(p_content);

	drawer_stack->add_child(entry.panel);
	docks.insert(p_id, entry);
	_update_theme();
}

void DocumentBottomDockHost::set_dock_open(const StringName &p_id, bool p_open) {
	DockEntry *entry = docks.getptr(p_id);
	ERR_FAIL_NULL(entry);

	if (p_open && open_dock == p_id) {
		entry->toggle_button->set_pressed_no_signal(true);
		return;
	}
	if (!p_open && open_dock != p_id) {
		entry->toggle_button->set_pressed_no_signal(false);
		return;
	}

	if (p_open && !open_dock.is_empty()) {
		DockEntry *previous = docks.getptr(open_dock);
		if (previous) {
			const StringName previous_id = open_dock;
			previous->panel->hide();
			previous->toggle_button->set_pressed_no_signal(false);
			open_dock = StringName();
			emit_signal(SNAME("dock_toggled"), previous_id, false);
		}
	}

	if (p_open) {
		open_dock = p_id;
		drawer_stack->show();
		entry->panel->show();
		entry->toggle_button->set_pressed_no_signal(true);
	} else {
		entry->panel->hide();
		drawer_stack->hide();
		entry->toggle_button->set_pressed_no_signal(false);
		open_dock = StringName();
	}

	emit_signal(SNAME("dock_toggled"), p_id, p_open);
}

bool DocumentBottomDockHost::is_dock_open(const StringName &p_id) const {
	return open_dock == p_id;
}

DocumentBottomDockHost::DocumentBottomDockHost(HBoxContainer *p_toggle_host) {
	ERR_FAIL_NULL(p_toggle_host);
	Control *toggle_spacer = memnew(Control);
	toggle_spacer->set_h_size_flags(SIZE_EXPAND_FILL);
	toggle_spacer->set_mouse_filter(MOUSE_FILTER_IGNORE);
	p_toggle_host->add_child(toggle_spacer);

	// Keep all contextual buttons as a compact right-aligned cluster. The focused pane's shared
	// 2D/3D toolbar is later inserted at child index zero, before this spacer and cluster.
	toggle_host = memnew(HBoxContainer);
	toggle_host->add_theme_constant_override("separation", 0);
	p_toggle_host->add_child(toggle_host);
	set_h_size_flags(SIZE_EXPAND_FILL);
	set_v_size_flags(SIZE_EXPAND_FILL);
	set_clip_contents(true);
	add_theme_constant_override("separation", 0);

	surface_split = memnew(VSplitContainer);
	surface_split->set_h_size_flags(SIZE_EXPAND_FILL);
	surface_split->set_v_size_flags(SIZE_EXPAND_FILL);
	add_child(surface_split);

	surface_clip = memnew(Control);
	surface_clip->set_h_size_flags(SIZE_EXPAND_FILL);
	surface_clip->set_v_size_flags(SIZE_EXPAND_FILL);
	surface_clip->set_clip_contents(true);
	surface_split->add_child(surface_clip);

	drawer_stack = memnew(VBoxContainer);
	drawer_stack->set_h_size_flags(SIZE_EXPAND_FILL);
	drawer_stack->set_v_size_flags(SIZE_FILL);
	drawer_stack->add_theme_constant_override("separation", 0);
	drawer_stack->hide();
	surface_split->add_child(drawer_stack);
}
