/**************************************************************************/
/*  pane_drop_overlay.cpp                                                 */
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

#include "pane_drop_overlay.h"

#include "editor/editor_string_names.h"
#include "editor/gui/tabbed_document_host.h"
#include "editor/themes/editor_scale.h"
#include "scene/main/viewport.h"

bool PaneDropOverlay::_accepts(const Variant &p_data) const {
	return owner_host && owner_host->can_accept_tab_drop(p_data);
}

PaneDropOverlay::Zone PaneDropOverlay::_zone_at(const Point2 &p_pos) const {
	const Size2 s = get_size();
	if (s.x <= 0 || s.y <= 0) {
		return ZONE_CENTER;
	}
	const float nx = (p_pos.x / s.x - 0.5f) * 2.0f; // -1 (left) .. 1 (right)
	const float ny = (p_pos.y / s.y - 0.5f) * 2.0f; // -1 (top) .. 1 (bottom)
	if (Math::abs(nx) < 0.34f && Math::abs(ny) < 0.34f) {
		return ZONE_CENTER;
	}
	if (Math::abs(nx) >= Math::abs(ny)) {
		return nx < 0 ? ZONE_LEFT : ZONE_RIGHT;
	}
	return ny < 0 ? ZONE_UP : ZONE_DOWN;
}

Rect2 PaneDropOverlay::_preview_rect(Zone p_zone) const {
	const Size2 s = get_size();
	switch (p_zone) {
		case ZONE_LEFT:
			return Rect2(0, 0, s.x * 0.5f, s.y);
		case ZONE_RIGHT:
			return Rect2(s.x * 0.5f, 0, s.x * 0.5f, s.y);
		case ZONE_UP:
			return Rect2(0, 0, s.x, s.y * 0.5f);
		case ZONE_DOWN:
			return Rect2(0, s.y * 0.5f, s.x, s.y * 0.5f);
		default:
			return Rect2(Point2(), s); // Center adds a tab -> whole pane.
	}
}

void PaneDropOverlay::_set_drag_active(bool p_active) {
	if (drag_active == p_active) {
		return;
	}
	drag_active = p_active;
	hover_zone = ZONE_NONE;
	// Only intercept the mouse while armed; otherwise clicks must reach the editor surface below.
	set_mouse_filter(p_active ? MOUSE_FILTER_STOP : MOUSE_FILTER_IGNORE);
	queue_redraw();
}

bool PaneDropOverlay::can_drop_data(const Point2 &p_point, const Variant &p_data) const {
	if (!_accepts(p_data)) {
		return false;
	}
	const Zone z = _zone_at(p_point);
	if (z != hover_zone) {
		hover_zone = z;
		const_cast<PaneDropOverlay *>(this)->queue_redraw();
	}
	return true;
}

void PaneDropOverlay::drop_data(const Point2 &p_point, const Variant &p_data) {
	if (!owner_host || !_accepts(p_data)) {
		return;
	}
	// The overlay owns zone semantics: translate the hovered zone into the split intent the workspace
	// primitive expects, so the host stays a pure pass-through.
	const Zone z = _zone_at(p_point);
	const bool center = (z == ZONE_CENTER);
	const bool vertical = (z == ZONE_UP || z == ZONE_DOWN);
	const bool new_on_second = (z == ZONE_RIGHT || z == ZONE_DOWN);
	owner_host->accept_tab_drop(p_data, center, vertical, new_on_second);
	_set_drag_active(false);
}

void PaneDropOverlay::_draw_glyph(Zone p_zone, const Rect2 &p_cell, const Color &p_color) {
	const Vector2 c = p_cell.get_center();
	const float a = p_cell.size.x * 0.22f;
	if (p_zone == ZONE_CENTER) {
		draw_rect(Rect2(c - Vector2(a, a), Vector2(a, a) * 2.0f), p_color, false, MAX(1, int(EDSCALE)));
		return;
	}
	Vector<Vector2> tri;
	switch (p_zone) {
		case ZONE_LEFT:
			tri.push_back(c + Vector2(a, -a));
			tri.push_back(c + Vector2(a, a));
			tri.push_back(c + Vector2(-a, 0));
			break;
		case ZONE_RIGHT:
			tri.push_back(c + Vector2(-a, -a));
			tri.push_back(c + Vector2(-a, a));
			tri.push_back(c + Vector2(a, 0));
			break;
		case ZONE_UP:
			tri.push_back(c + Vector2(-a, a));
			tri.push_back(c + Vector2(a, a));
			tri.push_back(c + Vector2(0, -a));
			break;
		case ZONE_DOWN:
			tri.push_back(c + Vector2(-a, -a));
			tri.push_back(c + Vector2(a, -a));
			tri.push_back(c + Vector2(0, a));
			break;
		default:
			return;
	}
	draw_colored_polygon(tri, p_color);
}

void PaneDropOverlay::_draw_compass() {
	const Color accent = get_theme_color(SNAME("accent_color"), EditorStringName(Editor));

	// The half (or whole, for center) the dropped tab will occupy.
	Color fill = accent;
	fill.a = 0.16f;
	Color border = accent;
	border.a = 0.85f;
	const Rect2 pr = _preview_rect(hover_zone);
	draw_rect(pr, fill, true);
	draw_rect(pr, border, false, MAX(1, int(2 * EDSCALE)));

	// The 5 compass cells at the pane center; the hovered one is highlighted.
	const float cs = 40 * EDSCALE;
	const float gap = 8 * EDSCALE;
	const Vector2 center = get_size() * 0.5f;
	const Color cell_bg = Color(0, 0, 0, 0.5f);
	const Zone zones[5] = { ZONE_CENTER, ZONE_LEFT, ZONE_RIGHT, ZONE_UP, ZONE_DOWN };
	const Vector2 offsets[5] = {
		Vector2(0, 0),
		Vector2(-(cs + gap), 0),
		Vector2(cs + gap, 0),
		Vector2(0, -(cs + gap)),
		Vector2(0, cs + gap),
	};
	for (int i = 0; i < 5; i++) {
		const Rect2 cell(center + offsets[i] - Vector2(cs, cs) * 0.5f, Vector2(cs, cs));
		const bool hot = (zones[i] == hover_zone);
		draw_rect(cell, hot ? accent : cell_bg, true);
		draw_rect(cell, border, false, MAX(1, int(EDSCALE)));
		_draw_glyph(zones[i], cell, hot ? cell_bg : accent);
	}
}

void PaneDropOverlay::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_DRAG_BEGIN: {
			const Variant drag_data = get_viewport() ? get_viewport()->gui_get_drag_data() : Variant();
			_set_drag_active(_accepts(drag_data));
		} break;

		case NOTIFICATION_DRAG_END: {
			_set_drag_active(false);
		} break;

		case NOTIFICATION_DRAW: {
			if (drag_active && hover_zone != ZONE_NONE) {
				_draw_compass();
			}
		} break;
	}
}

PaneDropOverlay::PaneDropOverlay() {
	set_mouse_filter(MOUSE_FILTER_IGNORE); // Armed only during a tab drag (see _set_drag_active).
}
