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
#include "editor/editor_undo_redo_manager.h"
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
		} else if (mb->get_button_index() == MouseButton::LEFT) {
			if (mb->is_pressed()) {
				_ensure_active();
				const Point2 canvas = _screen_to_canvas(mb->get_position());
				// Ctrl (no shift) drags to rotate the current selection; plain/shift click selects
				// and (on a hit) moves. Shift is additive-select (Ctrl is reserved for rotate).
				const bool rotate = mb->is_command_or_control_pressed() && !mb->is_shift_pressed();
				const bool additive = mb->is_shift_pressed();
				if (rotate) {
					_begin_drag(DRAG_ROTATE, canvas);
				} else {
					CanvasItem *hit = _pick_at(canvas);
					EditorNode *en = EditorNode::get_singleton();
					EditorSelection *selection = en ? en->get_editor_selection() : nullptr;
					if (selection) {
						if (additive) {
							if (hit) {
								if (selection->is_selected(hit)) {
									selection->remove_node(hit);
								} else {
									selection->add_node(hit);
								}
							}
						} else if (hit) {
							if (!selection->is_selected(hit)) {
								// Clicking an unselected item replaces the selection; clicking an already-
								// selected one keeps it (so a multi-selection drags as a group).
								selection->clear();
								selection->add_node(hit);
							}
						} else {
							selection->clear();
						}
					}
					if (hit && !additive) {
						_begin_drag(DRAG_MOVE, canvas);
					}
				}
				if (input_overlay) {
					input_overlay->queue_redraw();
				}
			} else if (drag_type != DRAG_NONE) {
				_commit_drag();
			}
			input_overlay->accept_event();
		}
		return;
	}
	const Ref<InputEventMouseMotion> mm = p_event;
	if (mm.is_valid()) {
		if (panning) {
			// Drag the view: a screen delta moves the shown canvas point by delta/zoom (opposite sign).
			view_offset -= mm->get_relative() / zoom;
			_update_view_transform();
			input_overlay->accept_event();
		} else if (drag_type == DRAG_MOVE) {
			_update_move(_screen_to_canvas(mm->get_position()));
			input_overlay->accept_event();
		} else if (drag_type == DRAG_ROTATE) {
			_update_rotate(_screen_to_canvas(mm->get_position()));
			input_overlay->accept_event();
		}
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

CanvasItem *CanvasView2D::_pick_at(const Point2 &p_canvas_pos) {
	CanvasItemEditor *cie = CanvasItemEditor::get_singleton();
	EditorNode *en = EditorNode::get_singleton();
	if (!cie || !en) {
		return nullptr;
	}
	Node *scene = en->get_edited_scene();
	if (!scene) {
		return nullptr;
	}
	// Hit-test in canvas space (accumulates only node transforms, so it works with our own view
	// transform), reusing the editor's picker; take the top-most (highest z-index) result.
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
	return hit;
}

void CanvasView2D::_begin_drag(DragType p_type, const Point2 &p_canvas_pos) {
	drag_type = DRAG_NONE;
	drag_selection.clear();
	drag_pre_state.clear();
	EditorNode *en = EditorNode::get_singleton();
	EditorSelection *selection = en ? en->get_editor_selection() : nullptr;
	if (!selection) {
		return;
	}
	for (Node *n : selection->get_top_selected_node_list()) {
		CanvasItem *ci = Object::cast_to<CanvasItem>(n);
		if (!ci || !ci->is_inside_tree()) {
			continue;
		}
		drag_selection.push_back(ci);
		drag_pre_state[ci->get_instance_id()] = ci->_edit_get_state(); // For live restore + undo.
	}
	if (drag_selection.is_empty()) {
		return;
	}
	drag_type = p_type;
	drag_from_canvas = p_canvas_pos;

	if (p_type == DRAG_ROTATE) {
		// Rotate about the first item's pivot (or origin), in canvas space.
		CanvasItem *ci = drag_selection.front()->get();
		if (ci->_edit_use_pivot()) {
			rotate_center = ci->get_screen_transform().xform(ci->_edit_get_pivot());
		} else {
			rotate_center = ci->get_screen_transform().get_origin();
		}
	}
}

void CanvasView2D::_update_move(const Point2 &p_canvas_pos) {
	const Point2 delta = p_canvas_pos - drag_from_canvas; // Total move in canvas space, from start.
	for (CanvasItem *ci : drag_selection) {
		HashMap<ObjectID, Dictionary>::Iterator it = drag_pre_state.find(ci->get_instance_id());
		if (!it) {
			continue;
		}
		ci->_edit_set_state(it->value); // Restore to pre-drag pose so the delta never accumulates.
		// Convert the canvas-space delta into the item's parent space, then offset its position.
		const Transform2D parent_xform_inv = ci->get_transform() * ci->get_screen_transform().affine_inverse();
		ci->_edit_set_position(ci->_edit_get_position() + parent_xform_inv.basis_xform(delta));
	}
	if (input_overlay) {
		input_overlay->queue_redraw();
	}
}

void CanvasView2D::_update_rotate(const Point2 &p_canvas_pos) {
	// Angle swept from the grab point to the cursor about the shared pivot; applied on top of each
	// item's pre-drag rotation. Sign flips for mirrored items (matching CanvasItemEditor).
	const real_t swept = (drag_from_canvas - rotate_center).angle_to(p_canvas_pos - rotate_center);
	for (CanvasItem *ci : drag_selection) {
		HashMap<ObjectID, Dictionary>::Iterator it = drag_pre_state.find(ci->get_instance_id());
		if (!it) {
			continue;
		}
		ci->_edit_set_state(it->value); // Restore, so the pre-drag rotation is the base.
		const bool opposite = ci->get_global_transform().get_scale().sign().dot(ci->get_transform().get_scale().sign()) == 0;
		ci->_edit_set_rotation(ci->_edit_get_rotation() + (opposite ? -1 : 1) * swept);
	}
	if (input_overlay) {
		input_overlay->queue_redraw();
	}
}

void CanvasView2D::_commit_drag() {
	const DragType type = drag_type;
	drag_type = DRAG_NONE;
	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	if (undo_redo) {
		bool changed = false;
		for (CanvasItem *ci : drag_selection) {
			HashMap<ObjectID, Dictionary>::Iterator it = drag_pre_state.find(ci->get_instance_id());
			if (it && ci->_edit_get_state().hash() != it->value.hash()) {
				changed = true;
				break;
			}
		}
		if (changed) {
			undo_redo->create_action(type == DRAG_ROTATE ? TTR("Rotate CanvasItem") : TTR("Move CanvasItem"));
			for (CanvasItem *ci : drag_selection) {
				HashMap<ObjectID, Dictionary>::Iterator it = drag_pre_state.find(ci->get_instance_id());
				if (!it) {
					continue;
				}
				undo_redo->add_do_method(ci, "_edit_set_state", ci->_edit_get_state());
				undo_redo->add_undo_method(ci, "_edit_set_state", it->value);
			}
			undo_redo->add_do_method(input_overlay, "queue_redraw");
			undo_redo->add_undo_method(input_overlay, "queue_redraw");
			undo_redo->commit_action();
		}
	}
	drag_selection.clear();
	drag_pre_state.clear();
}
