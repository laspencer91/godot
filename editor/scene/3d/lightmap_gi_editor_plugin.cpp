/**************************************************************************/
/*  lightmap_gi_editor_plugin.cpp                                         */
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

#include "lightmap_gi_editor_plugin.h"

#include "core/object/callable_mp.h"
#include "core/os/os.h"
#include "editor/derived_data/editor_derived_data.h"
#include "editor/editor_node.h"
#include "editor/editor_undo_redo_manager.h"
#include "scene/3d/lightmap_gi.h"
#include "scene/main/scene_tree.h"
#include "servers/display/display_server.h"
#include "servers/rendering/rendering_server.h"

#include "modules/modules_enabled.gen.h" // For lightmapper_rd.

void LightmapGIEditorPlugin::bake_node(Node *p_node) {
	// The registry's invoke path does not re-check the class match, so this cast is the guard.
	LightmapGI *lightmap = Object::cast_to<LightmapGI>(p_node);
	ERR_FAIL_NULL(lightmap);

	// The editor owns the output location: it is allocated from the node's persistent identity so
	// renames and moves keep resolving to the same bundle. The atlas .exr / shadowmask .png files
	// are derived from this basename inside the core bake, so they land in the bundle too.
	EditorDerivedData *edd = EditorDerivedData::get_singleton();
	ERR_FAIL_NULL(edd);
	const String out_path = edd->file_for(lightmap, SNAME("godot.lightmap"), "lmbake");
	if (out_path.is_empty()) {
		return; // The allocator already pushed a precise error (unsaved owner, unset registry, ...).
	}

	// The core bake reuses whatever LightmapGIData is currently assigned and re-paths it. A pasted or
	// duplicated node can still reference the source node's resource, so anything that does not
	// already live at this node's allocated path is dropped and rebuilt from scratch -- that also
	// covers the foreign/imported cases the old pre-check rejected.
	const Ref<LightmapGIData> prev = lightmap->get_light_data();
	const bool dropped_prev = prev.is_valid() && prev->get_path() != out_path;
	if (dropped_prev) {
		lightmap->set_light_data(Ref<LightmapGIData>());
	}

	LightmapGI::BakeError err = LightmapGI::BAKE_ERROR_OK;
	const uint64_t time_started = OS::get_singleton()->get_ticks_msec();
	if (get_tree()->get_edited_scene_root()) {
		if (get_tree()->get_edited_scene_root() == lightmap) {
			err = lightmap->bake(lightmap, out_path, bake_func_step);
		} else {
			err = lightmap->bake(lightmap->get_parent(), out_path, bake_func_step);
		}
	} else {
		err = LightmapGI::BAKE_ERROR_NO_SCENE_ROOT;
	}

	bake_func_end(time_started);

	if (err == LightmapGI::BAKE_ERROR_OK) {
		const Ref<LightmapGIData> current = lightmap->get_light_data();
		if (current != prev) {
			// The bake assigned the property itself, so only record it -- committing without
			// executing keeps the already-applied value and makes the repoint undoable.
			EditorUndoRedoManager *undo_redo = get_undo_redo();
			undo_redo->create_action(TTR("Bake Lightmaps"));
			undo_redo->add_do_property(lightmap, "light_data", current);
			undo_redo->add_undo_property(lightmap, "light_data", prev);
			undo_redo->commit_action(false);
		}
	} else if (dropped_prev && lightmap->get_light_data().is_null()) {
		lightmap->set_light_data(prev); // Failed bake: don't silently drop the old reference.
	}

	switch (err) {
		case LightmapGI::BAKE_ERROR_NO_SAVE_PATH: {
			// Defensive: the allocator always hands out a res:// path, so the core can no longer
			// reach this through the plugin.
			EditorNode::get_singleton()->show_warning(TTR("Can't determine a save path for lightmap images.\nSave your scene and try again."));
		} break;
		case LightmapGI::BAKE_ERROR_NO_MESHES: {
			EditorNode::get_singleton()->show_warning(
					TTR("No meshes with lightmapping support to bake. Make sure they contain UV2 data and their Global Illumination property is set to Static.") +
					String::utf8("\n\n•  ") + TTR("To import a scene with lightmapping support, set Meshes > Light Baking to Static Lightmaps in the Import dock.") +
					String::utf8("\n•  ") + TTR("To enable lightmapping support on a primitive mesh, edit the PrimitiveMesh resource in the inspector and check Add UV2.") +
					String::utf8("\n•  ") + TTR("To enable lightmapping support on a CSG mesh, select the root CSG node and choose CSG > Bake Mesh Instance at the top of the 3D editor viewport.\nSelect the generated MeshInstance3D node and choose Mesh > Unwrap UV2 for Lightmap/AO at the top of the 3D editor viewport."));
		} break;
		case LightmapGI::BAKE_ERROR_CANT_CREATE_IMAGE: {
			EditorNode::get_singleton()->show_warning(TTR("Failed creating lightmap images. Make sure the lightmap destination path is writable."));
		} break;
		case LightmapGI::BAKE_ERROR_NO_SCENE_ROOT: {
			EditorNode::get_singleton()->show_warning(TTR("No editor scene root found."));
		} break;
		case LightmapGI::BAKE_ERROR_TEXTURE_SIZE_TOO_SMALL: {
			EditorNode::get_singleton()->show_warning(TTR("Maximum texture size is too small for the lightmap images.\nWhile this can be fixed by increasing the maximum texture size, it is recommended you split the scene into more objects instead."));
		} break;
		case LightmapGI::BAKE_ERROR_LIGHTMAP_TOO_SMALL: {
			EditorNode::get_singleton()->show_warning(TTR("Failed creating lightmap images. Make sure all meshes to bake have the Lightmap Size Hint property set high enough, and the LightmapGI's Texel Scale value is not too low."));
		} break;
		case LightmapGI::BAKE_ERROR_ATLAS_TOO_SMALL: {
			EditorNode::get_singleton()->show_warning(TTR("Failed fitting a lightmap image into an atlas. This should never happen and should be reported."));
		} break;
		default: {
		} break;
	}
}

EditorProgress *LightmapGIEditorPlugin::tmp_progress = nullptr;

bool LightmapGIEditorPlugin::bake_func_step(float p_progress, const String &p_description, void *, bool p_refresh) {
	if (!tmp_progress) {
		tmp_progress = memnew(EditorProgress("bake_lightmaps", TTR("Bake Lightmaps"), 1000, true));
		ERR_FAIL_NULL_V(tmp_progress, false);
	}
	return tmp_progress->step(p_description, p_progress * 1000, p_refresh);
}

void LightmapGIEditorPlugin::bake_func_end(uint64_t p_time_started) {
	if (tmp_progress != nullptr) {
		memdelete(tmp_progress);
		tmp_progress = nullptr;
	}

	const int time_taken = OS::get_singleton()->get_ticks_msec() - p_time_started;
	print_line(vformat("Done baking lightmaps in %02d:%02d:%02d.%02d.", time_taken / 3'600'000, (time_taken % 3'600'000) / 60'000, (time_taken % 60'000) / 1000, (time_taken % 1000) / 10));
	// Request attention in case the user was doing something else.
	// Baking lightmaps is likely the editor task that can take the most time,
	// so only request the attention for baking lightmaps.
	DisplayServer::get_singleton()->window_request_attention();
}

LightmapGIEditorPlugin::LightmapGIEditorPlugin() {
	EditorDerivedData *edd = EditorDerivedData::get_singleton();
	ERR_FAIL_NULL(edd);
	PackedStringArray extensions;
	extensions.push_back("lmbake");
	extensions.push_back("exr");
	extensions.push_back("png");
	ERR_FAIL_COND(edd->register_slot(SNAME("godot.lightmap"), SNAME("lightmap_gi_editor_plugin"), extensions) != OK);

	bake_action = EditorSceneActionRegistry::get_singleton()->register_class_action(
			SNAME("lightmap_gi"), SNAME("bake"), SNAME("LightmapGI"),
			TTR("Bake Lightmaps"), SNAME("Bake"),
			callable_mp(this, &LightmapGIEditorPlugin::bake_node));

#ifdef MODULE_LIGHTMAPPER_RD_ENABLED
	// Disable lightmap baking if not supported on the current GPU.
	if (!DisplayServer::get_singleton()->can_create_rendering_device()) {
		bake_action->set_disabled(true, vformat(TTR("Lightmap baking is not supported on this GPU (%s)."), RenderingServer::get_singleton()->get_video_adapter_name()));
	}
#else
	// Disable lightmap baking if the module is disabled at compile-time.
#if defined(ANDROID_ENABLED) || defined(APPLE_EMBEDDED_ENABLED)
	bake_action->set_disabled(true, vformat(TTR("Lightmaps cannot be baked on %s."), OS::get_singleton()->get_name()));
#else
	bake_action->set_disabled(true, TTR("Lightmaps cannot be baked, as the `lightmapper_rd` module was disabled at compile-time."));
#endif
#endif // MODULE_LIGHTMAPPER_RD_ENABLED
}
