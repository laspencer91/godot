/**************************************************************************/
/*  ao_baker_3d_editor_plugin.cpp                                        */
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

#include "ao_baker_3d_editor_plugin.h"

#include "core/io/file_access.h"
#include "core/io/resource_loader.h"
#include "core/object/callable_mp.h"
#include "core/os/os.h"
#include "editor/derived_data/editor_derived_data.h"
#include "editor/editor_interface.h"
#include "editor/editor_node.h"
#include "editor/editor_undo_redo_manager.h"
#include "scene/3d/ao_baker_3d.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/gui/button.h"
#include "scene/gui/dialogs.h"
#include "scene/resources/3d/primitive_meshes.h"
#include "scene/resources/mesh.h"
#include "scene/resources/texture.h"
#include "servers/display/display_server.h"
#include "servers/rendering/rendering_server.h"

#include "modules/modules_enabled.gen.h" // For lightmapper_rd.

bool AOBaker3DEditorPlugin::_can_unwrap_in_place(const Ref<Mesh> &p_mesh, String &r_reason) const {
	if (p_mesh.is_null()) {
		r_reason = TTR("no mesh");
		return false;
	}

	// Primitive meshes gain UV2 simply by flipping their add_uv2 flag.
	Ref<PrimitiveMesh> primitive_mesh = p_mesh;
	if (primitive_mesh.is_valid()) {
		return true;
	}

	Ref<ArrayMesh> array_mesh = p_mesh;
	if (array_mesh.is_null()) {
		r_reason = TTR("not an ArrayMesh");
		return false;
	}

	// Imported / shared meshes must be made local before we can rewrite their UVs (same rule the
	// Mesh menu's "Unwrap UV2" enforces).
	const String path = array_mesh->get_path();
	const int srpos = path.find("::");
	if (srpos != -1) {
		const String base = path.substr(0, srpos);
		if (ResourceLoader::get_resource_type(base) == "PackedScene") {
			Node *edited_scene = EditorNode::get_singleton()->get_edited_scene();
			if (!edited_scene || edited_scene->get_scene_file_path() != base) {
				r_reason = TTR("make unique -- belongs to another scene");
				return false;
			}
		} else if (FileAccess::exists(path + ".import")) {
			r_reason = TTR("make unique -- imported");
			return false;
		}
	} else if (FileAccess::exists(path + ".import")) {
		r_reason = TTR("make unique -- imported");
		return false;
	}

	if (array_mesh->get_blend_shape_count() > 0) {
		r_reason = TTR("has blend shapes");
		return false;
	}
	for (int i = 0; i < array_mesh->get_surface_count(); i++) {
		if (array_mesh->surface_get_primitive_type(i) != Mesh::PRIMITIVE_TRIANGLES) {
			r_reason = TTR("has non-triangle surfaces");
			return false;
		}
		if (!(array_mesh->surface_get_format(i) & Mesh::ARRAY_FORMAT_NORMAL)) {
			r_reason = TTR("missing normals");
			return false;
		}
	}
	return true;
}

Error AOBaker3DEditorPlugin::_unwrap_mesh_instance(MeshInstance3D *p_mi, EditorUndoRedoManager *p_ur, String &r_error) {
	Ref<Mesh> mesh = p_mi->get_mesh();

	Ref<PrimitiveMesh> primitive_mesh = mesh;
	if (primitive_mesh.is_valid()) {
		p_ur->add_do_method(*primitive_mesh, "set_add_uv2", true);
		p_ur->add_undo_method(*primitive_mesh, "set_add_uv2", primitive_mesh->get_add_uv2());
		return OK;
	}

	Ref<ArrayMesh> array_mesh = mesh;
	if (array_mesh.is_null()) {
		r_error = TTR("not an ArrayMesh");
		return ERR_INVALID_DATA;
	}

	Ref<ArrayMesh> unwrapped_mesh = array_mesh->duplicate(false);
	const Error err = unwrapped_mesh->lightmap_unwrap(p_mi->get_global_transform());
	if (err != OK) {
		r_error = TTR("unwrap failed (mesh may not be manifold)");
		return err;
	}

	p_ur->add_do_method(p_mi, "set_mesh", unwrapped_mesh);
	p_ur->add_do_reference(unwrapped_mesh.ptr());
	p_ur->add_undo_method(p_mi, "set_mesh", array_mesh);
	return OK;
}

void AOBaker3DEditorPlugin::bake_node(Node *p_node) {
	// The registry's invoke path does not re-check the class match, so this cast is the guard.
	AOBaker3D *b = Object::cast_to<AOBaker3D>(p_node);
	ERR_FAIL_NULL(b);
	// The uv2_prompt flow reads `baker`, so the target is stored rather than kept local.
	baker = b;

	// The editor owns the output location: it is allocated from the node's persistent identity, so
	// renames and moves keep resolving to the same bundle and a rebake overwrites in place.
	EditorDerivedData *edd = EditorDerivedData::get_singleton();
	ERR_FAIL_NULL(edd);
	const String out_path = edd->file_for(baker, SNAME("godot.ao_bake"), "png");
	if (out_path.is_empty()) {
		return; // The allocator already pushed a precise error (unsaved owner, unset registry, ...).
	}

	pending_atlas_path = out_path;
	_prepare_bake();
}

void AOBaker3DEditorPlugin::_prepare_bake() {
	if (!baker || pending_atlas_path.is_empty()) {
		return;
	}

	Vector<MeshInstance3D *> ready;
	Vector<MeshInstance3D *> missing_uv2;
	baker->get_bake_candidates(ready, missing_uv2);

	// The common path: everything is already unwrapped, so bake straight away.
	if (missing_uv2.is_empty()) {
		_do_bake();
		return;
	}

	// Split the missing meshes into those we can auto-unwrap and those the user must fix by hand.
	int fixable = 0;
	Vector<String> blocked;
	for (int i = 0; i < missing_uv2.size(); i++) {
		String reason;
		if (_can_unwrap_in_place(missing_uv2[i]->get_mesh(), reason)) {
			fixable++;
		} else {
			blocked.push_back(vformat("    - %s (%s)", missing_uv2[i]->get_name(), reason));
		}
	}

	String msg = vformat(TTR("%d mesh(es) already have UV2 and are ready to bake."), ready.size());
	if (fixable > 0) {
		msg += "\n" + vformat(TTR("%d mesh(es) are missing UV2 and will be unwrapped first (this can be undone)."), fixable);
	}
	if (!blocked.is_empty()) {
		msg += "\n\n" + TTR("These meshes can't be unwrapped automatically -- make them local (Make Unique) first:") + "\n" + String("\n").join(blocked);
	}

	// Nothing bakeable and nothing we can fix: just report and stop.
	if (fixable == 0 && ready.is_empty()) {
		EditorNode::get_singleton()->show_warning(msg);
		return;
	}

	uv2_prompt->set_text(msg);
	uv2_prompt->get_ok_button()->set_disabled(fixable == 0);
	bake_ready_button->set_disabled(ready.is_empty());
	uv2_prompt->reset_size();
	uv2_prompt->popup_centered();
}

void AOBaker3DEditorPlugin::_prompt_custom_action(const String &p_action) {
	if (p_action == "bake_ready") {
		uv2_prompt->hide();
		_do_bake();
	}
}

void AOBaker3DEditorPlugin::_unwrap_and_bake() {
	if (!baker) {
		return;
	}

	Vector<MeshInstance3D *> ready;
	Vector<MeshInstance3D *> missing_uv2;
	baker->get_bake_candidates(ready, missing_uv2);

	EditorUndoRedoManager *ur = EditorUndoRedoManager::get_singleton();
	ur->create_action(TTR("Unwrap UV2 for AO"));
	Vector<String> failures;
	for (int i = 0; i < missing_uv2.size(); i++) {
		String reason;
		if (!_can_unwrap_in_place(missing_uv2[i]->get_mesh(), reason)) {
			continue; // Blocked mesh -- already reported in the prompt; leave it out of the bake.
		}
		String err;
		if (_unwrap_mesh_instance(missing_uv2[i], ur, err) != OK) {
			failures.push_back(vformat("    - %s (%s)", missing_uv2[i]->get_name(), err));
		}
	}
	// Applies the queued set_mesh / set_add_uv2 calls synchronously, so the bake below sees the UV2.
	ur->commit_action();

	if (!failures.is_empty()) {
		EditorNode::get_singleton()->show_warning(TTR("Some meshes could not be unwrapped and were skipped:") + "\n" + String("\n").join(failures));
	}

	_do_bake();
}

void AOBaker3DEditorPlugin::_do_bake() {
	if (!baker) {
		return;
	}
	const Ref<TextureLayered> prev_atlas = baker->get_ao_atlas();
	const Dictionary prev_transforms = baker->get_ao_transforms();

	const uint64_t time_started = OS::get_singleton()->get_ticks_msec();
	AOBaker3D::BakeError err = baker->bake(pending_atlas_path);
	const int time_taken = OS::get_singleton()->get_ticks_msec() - time_started;

	switch (err) {
		case AOBaker3D::BAKE_ERROR_OK: {
			// The atlas is an external resource; only its reference and the compact per-mesh transforms
			// persist in the scene. The bake assigned both properties itself, so only record them --
			// committing without executing keeps the already-applied values and makes the bake undoable.
			EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
			undo_redo->create_action(TTR("Bake AO"));
			undo_redo->add_do_property(baker, "ao_atlas", baker->get_ao_atlas());
			undo_redo->add_do_property(baker, "ao_transforms", baker->get_ao_transforms());
			undo_redo->add_undo_property(baker, "ao_atlas", prev_atlas);
			undo_redo->add_undo_property(baker, "ao_transforms", prev_transforms);
			undo_redo->commit_action(false);

			// Mark the scene dirty so Ctrl+S records both.
			const int wired = baker->get_ao_transforms().size();
			EditorInterface::get_singleton()->mark_scene_as_unsaved();
			print_line(vformat("Done baking AO in %d ms (%d meshes into %s; AO_MAP wired via the per-instance channel).", time_taken, wired, pending_atlas_path));
		} break;
		case AOBaker3D::BAKE_ERROR_NO_MESHES: {
			EditorNode::get_singleton()->show_warning(TTR("No meshes to bake AO for. Meshes need UV2 data and their Global Illumination property set to Static."));
		} break;
		case AOBaker3D::BAKE_ERROR_NO_LIGHTMAPPER: {
			EditorNode::get_singleton()->show_warning(TTR("AO baking is not supported on this GPU or build."));
		} break;
		case AOBaker3D::BAKE_ERROR_BAKE_FAILED: {
			const String reason = baker->get_last_bake_error_message();
			EditorNode::get_singleton()->show_warning(reason.is_empty() ? TTR("AO bake failed. See the output log for details.") : vformat(TTR("AO bake failed:\n\n%s"), reason));
		} break;
		case AOBaker3D::BAKE_ERROR_CANT_CREATE_DATA: {
			EditorNode::get_singleton()->show_warning(TTR("The AO bake completed, but its external Texture2DArray source could not be saved or imported. Check that the destination is a writable res:// path ending in .png."));
		} break;
	}
}

AOBaker3DEditorPlugin::AOBaker3DEditorPlugin() {
	uv2_prompt = memnew(ConfirmationDialog);
	uv2_prompt->set_title(TTR("Bake AO"));
	uv2_prompt->get_ok_button()->set_text(TTR("Unwrap & Bake"));
	bake_ready_button = uv2_prompt->add_button(TTR("Bake Ready Only"), true, "bake_ready");
	uv2_prompt->connect(SceneStringName(confirmed), callable_mp(this, &AOBaker3DEditorPlugin::_unwrap_and_bake));
	uv2_prompt->connect("custom_action", callable_mp(this, &AOBaker3DEditorPlugin::_prompt_custom_action));
	EditorNode::get_singleton()->get_gui_base()->add_child(uv2_prompt);

	bake_action = EditorSceneActionRegistry::get_singleton()->register_class_action(
			SNAME("ao_baker_3d"), SNAME("bake"), SNAME("AOBaker3D"),
			TTR("Bake AO"), SNAME("Bake"),
			callable_mp(this, &AOBaker3DEditorPlugin::bake_node));

#ifdef MODULE_LIGHTMAPPER_RD_ENABLED
	if (!DisplayServer::get_singleton()->can_create_rendering_device()) {
		bake_action->set_disabled(true, vformat(TTR("AO baking is not supported on this GPU (%s)."), RenderingServer::get_singleton()->get_video_adapter_name()));
	}
#else
	bake_action->set_disabled(true, TTR("AO cannot be baked, as the `lightmapper_rd` module was disabled at compile-time."));
#endif
}
