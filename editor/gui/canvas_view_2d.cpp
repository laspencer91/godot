/**************************************************************************/
/*  canvas_view_2d.cpp                                                    */
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

#include "canvas_view_2d.h"

#include "core/input/input_event.h"
#include "core/object/callable_mp.h"
#include "editor/editor_data.h"
#include "editor/editor_document.h"
#include "editor/editor_node.h"
#include "editor/editor_string_names.h"
#include "editor/scene/canvas_item_editor_plugin.h"
#include "scene/gui/subviewport_container.h"
#include "scene/main/canvas_item.h"
#include "scene/main/viewport.h"
#include "scene/resources/world_2d.h"

namespace {
constexpr real_t MIN_ZOOM = 0.02;
constexpr real_t MAX_ZOOM = 100.0;
constexpr real_t ZOOM_STEP = 1.1;
} // namespace

CanvasView2D::CanvasView2D(EditorDocument *p_document) {
	document = p_document;
	set_h_size_flags(SIZE_EXPAND_FILL);
	set_v_size_flags(SIZE_EXPAND_FILL);
	set_clip_contents(true);

	// A SubViewportContainer draws its child SubViewport's texture, stretched to fill.
	container = memnew(SubViewportContainer);
	container->set_stretch(true);
	container->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);
	add_child(container);

	// Our own viewport, pointed at the document's World2D so it renders that scene's canvas.
	// disable_3d: this is a 2D surface; disable_input: ⑤a is render-only (editing input is ⑤b).
	view_viewport = memnew(SubViewport);
	view_viewport->set_disable_3d(true);
	view_viewport->set_disable_input(true);
	container->add_child(view_viewport);

	if (document) {
		Ref<World2D> world = document->get_world_2d();
		if (world.is_valid()) {
			view_viewport->set_world_2d(world);
		}
	}

	// Transparent top layer to capture pan/zoom input (the viewport itself has input disabled).
	input_overlay = memnew(Control);
	input_overlay->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);
	input_overlay->set_mouse_filter(Control::MOUSE_FILTER_STOP);
	input_overlay->connect(SceneStringName(gui_input), callable_mp(this, &CanvasView2D::_gui_input_overlay));
	input_overlay->connect(SceneStringName(draw), callable_mp(this, &CanvasView2D::_draw_overlay));
	add_child(input_overlay);

	// Redraw the overlay when the (global) selection changes, so selection boxes stay current
	// even when selection is driven from elsewhere (e.g. the scene-tree dock). The connection is
	// auto-removed when input_overlay is freed.
	if (EditorNode *en = EditorNode::get_singleton()) {
		if (EditorSelection *sel = en->get_editor_selection()) {
			sel->connect("selection_changed", callable_mp((CanvasItem *)input_overlay, &CanvasItem::queue_redraw));
		}
	}
}

void CanvasView2D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE:
		case NOTIFICATION_RESIZED: {
			_update_view_transform();
		} break;
	}
}

void CanvasView2D::_update_view_transform() {
	if (!view_viewport) {
		return;
	}
	// Map canvas -> view pixels: scale by zoom, and place view_offset's canvas point at the view
	// center. Per-(viewport, canvas), so it never affects another view of the same World2D.
	Transform2D xform;
	xform.scale_basis(Size2(zoom, zoom));
	xform.columns[2] = get_size() * 0.5 - view_offset * zoom;
	view_viewport->set_global_canvas_transform(xform);

	if (input_overlay) {
		input_overlay->queue_redraw(); // Keep the grid/axis overlay aligned with the new transform.
	}
}

Point2 CanvasView2D::_screen_to_canvas(const Point2 &p_screen) const {
	return (p_screen - get_size() * 0.5) / zoom + view_offset;
}

Point2 CanvasView2D::_canvas_to_screen(const Point2 &p_canvas) const {
	return (p_canvas - view_offset) * zoom + get_size() * 0.5;
}

void CanvasView2D::_draw_overlay() {
	if (!input_overlay) {
		return;
	}
	const Size2 size = get_size();
	if (size.x <= 0 || size.y <= 0) {
		return;
	}

	// Grid spacing comes from the CanvasItemEditor SERVICE (shared 2D editing policy); adapt it
	// upward so lines never pack tighter than a few pixels on screen at the current zoom.
	Vector2 step(8, 8);
	if (CanvasItemEditor *cie = CanvasItemEditor::get_singleton()) {
		step = cie->get_grid_step();
	}
	if (step.x <= 0) {
		step.x = 8;
	}
	if (step.y <= 0) {
		step.y = 8;
	}
	const real_t min_px = 8.0;
	while (step.x * zoom < min_px) {
		step.x *= 2.0;
	}
	while (step.y * zoom < min_px) {
		step.y *= 2.0;
	}

	const Color grid_color(1, 1, 1, 0.06);
	const Point2 tl = _screen_to_canvas(Point2());
	const Point2 br = _screen_to_canvas(size);
	for (real_t cx = Math::floor(tl.x / step.x) * step.x; cx <= br.x; cx += step.x) {
		const real_t sx = _canvas_to_screen(Point2(cx, 0)).x;
		input_overlay->draw_line(Point2(sx, 0), Point2(sx, size.y), grid_color);
	}
	for (real_t cy = Math::floor(tl.y / step.y) * step.y; cy <= br.y; cy += step.y) {
		const real_t sy = _canvas_to_screen(Point2(0, cy)).y;
		input_overlay->draw_line(Point2(0, sy), Point2(size.x, sy), grid_color);
	}

	// Origin axes (x = horizontal line at canvas y=0, y = vertical line at canvas x=0), matching
	// the 2D editor's axis colors.
	const Point2 o = _canvas_to_screen(Point2());
	const Color axis_x = get_theme_color(SNAME("axis_x_color"), EditorStringName(Editor)) * Color(1, 1, 1, 0.75);
	const Color axis_y = get_theme_color(SNAME("axis_y_color"), EditorStringName(Editor)) * Color(1, 1, 1, 0.75);
	if (o.y >= 0 && o.y <= size.y) {
		input_overlay->draw_line(Point2(0, o.y), Point2(size.x, o.y), axis_x);
	}
	if (o.x >= 0 && o.x <= size.x) {
		input_overlay->draw_line(Point2(o.x, 0), Point2(o.x, size.y), axis_y);
	}

	// Selection boxes -- only when THIS view's document is the active one, so an inactive split
	// pane never shows another document's (stale) selection. The global selection belongs to the
	// active document (per-document selection Model A); per-pane independent selection is later.
	EditorNode *en = EditorNode::get_singleton();
	if (!en || !_is_active_document()) {
		return;
	}
	EditorSelection *selection = en->get_editor_selection();
	if (!selection) {
		return;
	}
	const Color sel_color = get_theme_color(SNAME("accent_color"), EditorStringName(Editor));
	for (const Node *n : selection->get_top_selected_node_list()) {
		const CanvasItem *ci = Object::cast_to<CanvasItem>(n);
		if (!ci || !ci->is_inside_tree() || !ci->is_visible_in_tree()) {
			continue;
		}
		const Rect2 rect = ci->_edit_get_rect();
		const Transform2D gt = ci->get_global_transform(); // canvas space
		const Point2 c[4] = {
			_canvas_to_screen(gt.xform(rect.position)),
			_canvas_to_screen(gt.xform(rect.position + Vector2(rect.size.x, 0))),
			_canvas_to_screen(gt.xform(rect.position + rect.size)),
			_canvas_to_screen(gt.xform(rect.position + Vector2(0, rect.size.y))),
		};
		for (int i = 0; i < 4; i++) {
			input_overlay->draw_line(c[i], c[(i + 1) % 4], sel_color, 2.0);
		}
	}
}

void CanvasView2D::_zoom_at(const Point2 &p_screen, real_t p_factor) {
	const real_t new_zoom = CLAMP(zoom * p_factor, MIN_ZOOM, MAX_ZOOM);
	if (new_zoom == zoom) {
		return;
	}
	const Point2 canvas_point = _screen_to_canvas(p_screen); // Under the cursor, before zooming.
	zoom = new_zoom;
	// Re-solve view_offset so canvas_point still lands under p_screen.
	view_offset = canvas_point - (p_screen - get_size() * 0.5) / zoom;
	_update_view_transform();
}

void CanvasView2D::_gui_input_overlay(const Ref<InputEvent> &p_event) {
	const Ref<InputEventMouseButton> mb = p_event;
	if (mb.is_valid()) {
		if (mb->is_pressed() && mb->get_button_index() == MouseButton::WHEEL_UP) {
			_zoom_at(mb->get_position(), ZOOM_STEP);
			input_overlay->accept_event();
		} else if (mb->is_pressed() && mb->get_button_index() == MouseButton::WHEEL_DOWN) {
			_zoom_at(mb->get_position(), 1.0 / ZOOM_STEP);
			input_overlay->accept_event();
		} else if (mb->get_button_index() == MouseButton::MIDDLE) {
			panning = mb->is_pressed();
			input_overlay->accept_event();
		} else if (mb->is_pressed() && mb->get_button_index() == MouseButton::LEFT) {
			// Click-select: make this pane's document active, then pick the item under the cursor.
			_ensure_active();
			_select_at(_screen_to_canvas(mb->get_position()), mb->is_shift_pressed() || mb->is_command_or_control_pressed());
			input_overlay->accept_event();
		}
		return;
	}
	const Ref<InputEventMouseMotion> mm = p_event;
	if (mm.is_valid() && panning) {
		// Drag the view: a screen delta moves the shown canvas point by delta/zoom (opposite sign).
		view_offset -= mm->get_relative() / zoom;
		_update_view_transform();
		input_overlay->accept_event();
	}
}

bool CanvasView2D::_is_active_document() const {
	EditorNode *en = EditorNode::get_singleton();
	return en && document && en->get_editor_data().get_active_document() == document;
}

void CanvasView2D::_ensure_active() {
	EditorNode *en = EditorNode::get_singleton();
	if (!en || !document || _is_active_document()) {
		return;
	}
	// Resolve this document's current edited-scene index and make it active (same path as a tab
	// selection), so the global selection this view edits belongs to THIS document.
	EditorData &ed = en->get_editor_data();
	const int count = ed.get_edited_scene_count();
	for (int i = 0; i < count; i++) {
		if (ed.get_document(i) == document) {
			en->set_edited_scene_index(i);
			return;
		}
	}
}

void CanvasView2D::_select_at(const Point2 &p_canvas_pos, bool p_additive) {
	CanvasItemEditor *cie = CanvasItemEditor::get_singleton();
	EditorNode *en = EditorNode::get_singleton();
	if (!cie || !en) {
		return;
	}
	Node *scene = en->get_edited_scene();
	EditorSelection *selection = en->get_editor_selection();
	if (!scene || !selection) {
		return;
	}

	// Hit-test in canvas space (the accumulated node transforms only), reusing the editor's
	// picker; then take the top-most (highest z-index) result.
	Vector<CanvasItemEditor::SelectResult> results;
	cie->find_canvas_items_at_pos(p_canvas_pos, scene, results);

	CanvasItem *hit = nullptr;
	int best_z = 0;
	for (int i = 0; i < results.size(); i++) {
		if (!hit || results[i].z_index >= best_z) {
			hit = results[i].item;
			best_z = results[i].z_index;
		}
	}

	if (!hit) {
		if (!p_additive) {
			selection->clear();
		}
	} else if (p_additive) {
		if (selection->is_selected(hit)) {
			selection->remove_node(hit);
		} else {
			selection->add_node(hit);
		}
	} else {
		selection->clear();
		selection->add_node(hit);
	}
	if (input_overlay) {
		input_overlay->queue_redraw();
	}
}
