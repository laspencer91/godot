/**************************************************************************/
/*  document_bottom_dock.h                                                */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
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

#pragma once

#include "core/templates/hash_map.h"
#include "scene/gui/box_container.h"

class Button;
class Control;
class HBoxContainer;
class PanelContainer;
class VSplitContainer;

// Hosts the viewport-side contextual drawers owned by one DocumentView. Toggle
// buttons live in that document's toolbar; drawer bodies occupy only the surface
// column, leaving the scene-tree/Inspector column at full height.
class DocumentBottomDockHost : public VBoxContainer {
	GDCLASS(DocumentBottomDockHost, VBoxContainer);

	struct DockEntry {
		StringName icon_name;
		PanelContainer *panel = nullptr;
		Button *toggle_button = nullptr;
		Button *close_button = nullptr;
	};

	HashMap<StringName, DockEntry> docks;
	VSplitContainer *surface_split = nullptr;
	Control *surface_clip = nullptr;
	VBoxContainer *drawer_stack = nullptr;
	HBoxContainer *toggle_host = nullptr;
	StringName open_dock;

	void _toggle_pressed(bool p_pressed, StringName p_id);
	void _update_theme();

protected:
	static void _bind_methods();
	void _notification(int p_what);

public:
	// The drawer must never change the minimum or desired height of the surrounding document
	// composite: doing so would resize the sibling Scene Tree/Inspector column in a short or split
	// pane. Its contents are clipped to the viewport-side allocation instead.
	virtual Size2 get_minimum_size() const override { return Size2(); }
	virtual Size2 get_desired_size() const override { return Size2(); }

	void set_surface(Control *p_surface);
	void add_dock(const StringName &p_id, const String &p_title, const StringName &p_icon_name, Control *p_content);
	void set_dock_open(const StringName &p_id, bool p_open);
	bool is_dock_open(const StringName &p_id) const;
	StringName get_open_dock() const { return open_dock; }

	DocumentBottomDockHost(HBoxContainer *p_toggle_host);
};
