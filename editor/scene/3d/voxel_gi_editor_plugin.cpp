/**************************************************************************/
/*  voxel_gi_editor_plugin.cpp                                            */
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

#include "voxel_gi_editor_plugin.h"

#include "core/io/resource_saver.h"
#include "core/object/callable_mp.h"
#include "editor/derived_data/editor_derived_data.h"
#include "editor/editor_node.h"
#include "editor/editor_string_names.h"
#include "editor/editor_undo_redo_manager.h"
#include "scene/gui/button.h"

void VoxelGIEditorPlugin::_bake() {
	if (!voxel_gi) {
		return;
	}

	// The editor owns the output location: it is allocated from the node's persistent identity, so
	// renames and moves keep resolving to the same bundle.
	EditorDerivedData *edd = EditorDerivedData::get_singleton();
	ERR_FAIL_NULL(edd);
	const String out_path = edd->file_for(voxel_gi, SNAME("godot.voxel_gi"), "res");
	if (out_path.is_empty()) {
		return; // The allocator already pushed a precise error (unsaved owner, unset registry, ...).
	}

	// The core bake reuses whatever VoxelGIData is currently assigned. A pasted or duplicated node
	// can still reference the source node's resource, so anything that does not already live at this
	// node's allocated path is dropped and rebuilt -- that also covers the foreign/imported cases the
	// old guards rejected.
	const Ref<VoxelGIData> prev = voxel_gi->get_probe_data();
	const bool dropped_prev = prev.is_valid() && prev->get_path() != out_path;
	if (dropped_prev) {
		voxel_gi->set_probe_data(Ref<VoxelGIData>());
	}

	voxel_gi->bake();

	Ref<VoxelGIData> data = voxel_gi->get_probe_data();
	if (data.is_null()) {
		if (dropped_prev) {
			voxel_gi->set_probe_data(prev); // Bake bailed out: don't silently drop the old reference.
		}
		ERR_FAIL_MSG("VoxelGI bake produced no data.");
	}

	// Always write the data out. Keeping it external avoids bloating the scene file with large binary
	// data, which a `.tscn` would serialize as Base64. Take-over form of set_path: on a rebake in
	// place the resource cache already holds this path.
	if (data->get_path() != out_path) {
		data->set_path(out_path, true);
	}
	ResourceSaver::save(data, out_path, ResourceSaver::FLAG_CHANGE_PATH);

	if (data != prev) {
		// The bake assigned the property itself, so only record it -- committing without executing
		// keeps the already-applied value and makes the repoint undoable.
		EditorUndoRedoManager *undo_redo = get_undo_redo();
		undo_redo->create_action(TTR("Bake VoxelGI"));
		undo_redo->add_do_property(voxel_gi, "data", data);
		undo_redo->add_undo_property(voxel_gi, "data", prev);
		undo_redo->commit_action(false);
	}
}

void VoxelGIEditorPlugin::edit(Object *p_object) {
	VoxelGI *s = Object::cast_to<VoxelGI>(p_object);
	if (!s) {
		return;
	}

	voxel_gi = s;
}

bool VoxelGIEditorPlugin::handles(Object *p_object) const {
	return p_object->is_class("VoxelGI");
}

void VoxelGIEditorPlugin::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_PROCESS: {
			if (!voxel_gi) {
				return;
			}

			// Set information tooltip on the Bake button. This information is useful
			// to optimize performance (video RAM size) and reduce light leaking (individual cell size).

			const Vector3i cell_size = voxel_gi->get_estimated_cell_size();

			const Vector3 half_size = voxel_gi->get_size() / 2;

			const int data_size = 4;
			const double size_mb = cell_size.x * cell_size.y * cell_size.z * data_size / (1024.0 * 1024.0);
			// Add a qualitative measurement to help the user assess whether a VoxelGI node is using a lot of VRAM.
			String size_quality;
			if (size_mb < 16.0) {
				size_quality = TTR("Low");
			} else if (size_mb < 64.0) {
				size_quality = TTR("Moderate");
			} else {
				size_quality = TTR("High");
			}

			String text;
			text += vformat(TTR("Subdivisions: %s"), vformat(U"%d × %d × %d", cell_size.x, cell_size.y, cell_size.z)) + "\n";
			text += vformat(TTR("Cell size: %s"), vformat(U"%.3f × %.3f × %.3f", half_size.x / cell_size.x, half_size.y / cell_size.y, half_size.z / cell_size.z)) + "\n";
			text += vformat(TTR("Video RAM size: %s MB (%s)"), String::num(size_mb, 2), size_quality);

			// Only update the tooltip when needed to avoid constant redrawing.
			if (bake->get_tooltip(Point2()) == text) {
				return;
			}

			bake->set_tooltip_text(text);
		} break;
	}
}

void VoxelGIEditorPlugin::make_visible(bool p_visible) {
	if (p_visible) {
		bake_hb->show();
		set_process(true);
	} else {
		bake_hb->hide();
		set_process(false);
	}
}

EditorProgress *VoxelGIEditorPlugin::tmp_progress = nullptr;

void VoxelGIEditorPlugin::bake_func_begin() {
	ERR_FAIL_COND(tmp_progress != nullptr);

	tmp_progress = memnew(EditorProgress("bake_gi", TTR("Bake VoxelGI"), 1000, true));
}

bool VoxelGIEditorPlugin::bake_func_step(int p_progress, const String &p_description) {
	ERR_FAIL_NULL_V(tmp_progress, false);
	return tmp_progress->step(p_description, p_progress, false);
}

void VoxelGIEditorPlugin::bake_func_end() {
	ERR_FAIL_NULL(tmp_progress);
	memdelete(tmp_progress);
	tmp_progress = nullptr;
}

VoxelGIEditorPlugin::VoxelGIEditorPlugin() {
	bake_hb = memnew(HBoxContainer);
	bake_hb->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	bake_hb->hide();
	bake = memnew(Button);
	bake->set_theme_type_variation(SceneStringName(FlatButton));
	// TODO: Rework this as a dedicated toolbar control so we can hook into theme changes and update it
	// when the editor theme updates.
	bake->set_button_icon(EditorNode::get_singleton()->get_editor_theme()->get_icon(SNAME("Bake"), EditorStringName(EditorIcons)));
	bake->set_text(TTR("Bake VoxelGI"));
	bake->connect(SceneStringName(pressed), callable_mp(this, &VoxelGIEditorPlugin::_bake));
	bake_hb->add_child(bake);

	add_control_to_container(CONTAINER_SPATIAL_EDITOR_MENU, bake_hb);
	voxel_gi = nullptr;

	VoxelGI::bake_begin_function = bake_func_begin;
	VoxelGI::bake_step_function = bake_func_step;
	VoxelGI::bake_end_function = bake_func_end;
}
