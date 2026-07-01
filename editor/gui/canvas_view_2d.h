/**************************************************************************/
/*  canvas_view_2d.h                                                      */
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

class EditorDocument;
class SubViewport;
class SubViewportContainer;

// CanvasView2D is the per-pane 2D editor surface (Step ⑤a), the 2D analog of
// Node3DEditorView. It owns its OWN SubViewport bound to the document's World2D
// (via set_world_2d), so it renders that document's 2D scene independently of any
// other view -- multiple panes can show the same or different documents' 2D
// content at once, WITHOUT reparenting the document's scene_root (the v1 shim's
// limitation). CanvasItems render into the World2D's shared canvas RID, so simply
// pointing a viewport at that world displays them.
//
// ⑤a: renders the scene (origin centered). ⑤b.1: the view now OWNS its pan/zoom
// (view-state, per the taxonomy) -- mouse-wheel zoom-at-cursor and middle-drag
// pan, each view independent. The editing overlay (selection/handles/rulers/grid)
// and scene manipulation are the remaining CanvasItemEditor services/view split.
class CanvasView2D : public Control {
	GDCLASS(CanvasView2D, Control);

	EditorDocument *document = nullptr; // The document this view renders (not owned).
	SubViewportContainer *container = nullptr;
	SubViewport *view_viewport = nullptr; // Own viewport, bound to the document's World2D.
	Control *input_overlay = nullptr; // Transparent top layer that captures pan/zoom input.

	// Per-view pan/zoom (view-state). view_offset is the canvas point shown at the view center.
	real_t zoom = 1.0;
	Point2 view_offset;
	bool panning = false;

	void _update_view_transform(); // Push zoom + view_offset onto the viewport's canvas transform.
	Point2 _screen_to_canvas(const Point2 &p_screen) const;
	void _zoom_at(const Point2 &p_screen, real_t p_factor); // Zoom keeping p_screen's canvas point fixed.
	void _gui_input_overlay(const Ref<InputEvent> &p_event);

protected:
	void _notification(int p_what);
	static void _bind_methods() {}

public:
	SubViewport *get_view_viewport() const { return view_viewport; }
	real_t get_zoom() const { return zoom; }

	CanvasView2D(EditorDocument *p_document);
};
