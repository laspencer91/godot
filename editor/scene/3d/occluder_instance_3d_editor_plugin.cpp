/**************************************************************************/
/*  occluder_instance_3d_editor_plugin.cpp                                */
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

#include "occluder_instance_3d_editor_plugin.h"

#include "core/object/callable_mp.h"
#include "editor/derived_data/editor_derived_data.h"
#include "editor/editor_node.h"
#include "editor/editor_undo_redo_manager.h"
#include "scene/3d/occluder_instance_3d.h"
#include "scene/main/scene_tree.h"

void OccluderInstance3DEditorPlugin::bake_node(Node *p_node) {
	// The registry's invoke path does not re-check the class match, so this cast is the guard.
	OccluderInstance3D *occluder_instance = Object::cast_to<OccluderInstance3D>(p_node);
	ERR_FAIL_NULL(occluder_instance);

	// The editor owns the output location: it is allocated from the node's persistent identity, so
	// renames and moves keep resolving to the same bundle.
	EditorDerivedData *edd = EditorDerivedData::get_singleton();
	ERR_FAIL_NULL(edd);
	const String out_path = edd->file_for(occluder_instance, SNAME("godot.occluder"), "occ");
	if (out_path.is_empty()) {
		return; // The allocator already pushed a precise error (unsaved owner, unset registry, ...).
	}

	// The core bake reuses whatever Occluder3D is currently assigned and re-paths it. A pasted or
	// duplicated node can still reference the source node's resource, so anything that does not
	// already live at this node's allocated path is dropped and rebuilt from scratch.
	const Ref<Occluder3D> prev = occluder_instance->get_occluder();
	const bool dropped_prev = prev.is_valid() && prev->get_path() != out_path;
	if (dropped_prev) {
		occluder_instance->set_occluder(Ref<Occluder3D>());
	}

	OccluderInstance3D::BakeError err;
	if (get_tree()->get_edited_scene_root() && get_tree()->get_edited_scene_root() == occluder_instance) {
		err = occluder_instance->bake_scene(occluder_instance, out_path);
	} else {
		err = occluder_instance->bake_scene(occluder_instance->get_parent(), out_path);
	}

	if (err == OccluderInstance3D::BAKE_ERROR_OK) {
		const Ref<Occluder3D> current = occluder_instance->get_occluder();
		if (current != prev) {
			// The bake assigned the property itself, so only record it -- committing without
			// executing keeps the already-applied value and makes the repoint undoable.
			EditorUndoRedoManager *undo_redo = get_undo_redo();
			undo_redo->create_action(TTR("Bake Occluders"));
			undo_redo->add_do_property(occluder_instance, "occluder", current);
			undo_redo->add_undo_property(occluder_instance, "occluder", prev);
			undo_redo->commit_action(false);
		}
	} else if (dropped_prev && occluder_instance->get_occluder().is_null()) {
		occluder_instance->set_occluder(prev); // Failed bake: don't silently drop the old reference.
	}

	switch (err) {
		case OccluderInstance3D::BAKE_ERROR_NO_SAVE_PATH: {
			// Defensive: the allocator always hands out a res:// path, so the core can no longer
			// reach this through the plugin.
			EditorNode::get_singleton()->show_warning(TTR("Can't determine a save path for the occluder.\nSave your scene and try again."));
		} break;
		case OccluderInstance3D::BAKE_ERROR_NO_MESHES: {
			EditorNode::get_singleton()->show_warning(TTR("No meshes to bake.\nMake sure there is at least one MeshInstance3D node in the scene whose visual layers are part of the OccluderInstance3D's Bake Mask property."));
			break;
		}
		case OccluderInstance3D::BAKE_ERROR_CANT_SAVE: {
			EditorNode::get_singleton()->show_warning(TTR("Could not save the new occluder at the specified path:") + " " + out_path);
			break;
		}
		default: {
		}
	}
}

OccluderInstance3DEditorPlugin::OccluderInstance3DEditorPlugin() {
	bake_action = EditorSceneActionRegistry::get_singleton()->register_class_action(
			SNAME("occluder_instance_3d"), SNAME("bake"), SNAME("OccluderInstance3D"),
			TTR("Bake Occluders"), SNAME("Bake"),
			callable_mp(this, &OccluderInstance3DEditorPlugin::bake_node));
}
