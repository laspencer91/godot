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

#include "editor/editor_document.h"
#include "scene/gui/subviewport_container.h"
#include "scene/main/viewport.h"
#include "scene/resources/world_2d.h"

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
	// ⑤a: place the canvas origin at the center of the view (no interactive pan/zoom yet).
	// The transform is per-(viewport, canvas), so this does not affect any other view of the
	// same World2D. Interactive pan/zoom + overlay come with the ⑤b services/view split.
	Transform2D xform;
	xform.columns[2] = get_size() * 0.5;
	view_viewport->set_global_canvas_transform(xform);
}
