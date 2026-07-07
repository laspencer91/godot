/**************************************************************************/
/*  pane_drop_overlay.h                                                   */
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

#pragma once

#include "scene/gui/control.h"

class TabbedDocumentHost;

// PaneDropOverlay is the drag-to-split "compass" (G6): a transparent Control that fills a workspace
// pane's content area. It is mouse-transparent normally and only arms while a document tab is being
// dragged, at which point it draws the 5-zone compass (center + N/S/E/W) plus a translucent preview
// of where the split will land, and accepts the drop. The actual detach/split/adopt is delegated to
// its owner TabbedDocumentHost (which resolves the source host + target pane and calls the workspace).
class PaneDropOverlay : public Control {
	GDCLASS(PaneDropOverlay, Control);

public:
	enum Zone {
		ZONE_NONE = -1,
		ZONE_CENTER,
		ZONE_LEFT,
		ZONE_RIGHT,
		ZONE_UP,
		ZONE_DOWN,
	};

private:
	TabbedDocumentHost *owner_host = nullptr;
	bool drag_active = false; // A tab drag we can accept is in progress.
	// Zone under the cursor. Updated from const can_drop_data (Godot's per-motion drop query), hence mutable.
	mutable Zone hover_zone = ZONE_NONE;

	bool _accepts(const Variant &p_data) const; // True when p_data is a workspace tab our owner can take.
	Zone _zone_at(const Point2 &p_pos) const;
	Rect2 _preview_rect(Zone p_zone) const;
	void _set_drag_active(bool p_active);
	void _draw_compass();
	void _draw_glyph(Zone p_zone, const Rect2 &p_cell, const Color &p_color);

protected:
	void _notification(int p_what);
	static void _bind_methods() {}

	virtual bool can_drop_data(const Point2 &p_point, const Variant &p_data) const override;
	virtual void drop_data(const Point2 &p_point, const Variant &p_data) override;

public:
	void set_owner_host(TabbedDocumentHost *p_host) { owner_host = p_host; }

	PaneDropOverlay();
};
