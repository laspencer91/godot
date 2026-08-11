/**************************************************************************/
/*  gpu_particles_collision_sdf_editor_plugin.cpp                         */
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

#include "gpu_particles_collision_sdf_editor_plugin.h"

#include "core/io/resource_loader.h"
#include "core/object/callable_mp.h"
#include "editor/editor_interface.h"
#include "editor/editor_node.h"
#include "editor/gui/editor_file_dialog.h"
#include "scene/3d/gpu_particles_collision_3d.h"
#include "scene/main/scene_tree.h"

void GPUParticlesCollisionSDF3DEditorPlugin::bake_node(Node *p_node) {
	// The registry's invoke path does not re-check the class match, so this cast is the guard.
	GPUParticlesCollisionSDF3D *s = Object::cast_to<GPUParticlesCollisionSDF3D>(p_node);
	ERR_FAIL_NULL(s);
	// The save-path dialog flow reads `col_sdf`, so the target is stored rather than kept local.
	col_sdf = s;

	if (col_sdf->get_texture().is_null() || !col_sdf->get_texture()->get_path().is_resource_file()) {
		String path = get_tree()->get_edited_scene_root()->get_scene_file_path();
		if (path.is_empty()) {
			path = "res://" + col_sdf->get_name() + "_data.exr";
		} else {
			path = path.get_basename() + "." + col_sdf->get_name() + "_data.exr";
		}
		probe_file->set_current_path(path);
		probe_file->popup_file_dialog();
		return;
	}

	_sdf_save_path_and_bake(col_sdf->get_texture()->get_path());
}

String GPUParticlesCollisionSDF3DEditorPlugin::get_bake_tooltip(Node *p_node) const {
	GPUParticlesCollisionSDF3D *sdf = Object::cast_to<GPUParticlesCollisionSDF3D>(p_node);
	if (!sdf) {
		return String();
	}

	// Information tooltip for the bake action. This information is useful to optimize performance
	// (video RAM size) and reduce collision tunneling (individual cell size).

	const Vector3i size = sdf->get_estimated_cell_size();

	const Vector3 extents = sdf->get_size() / 2;

	int data_size = 2;
	const double size_mb = size.x * size.y * size.z * data_size / (1024.0 * 1024.0);
	// Add a qualitative measurement to help the user assess whether a GPUParticlesCollisionSDF3D node is using a lot of VRAM.
	String size_quality;
	if (size_mb < 8.0) {
		size_quality = TTR("Low");
	} else if (size_mb < 32.0) {
		size_quality = TTR("Moderate");
	} else {
		size_quality = TTR("High");
	}

	String text;
	text += vformat(TTR("Subdivisions: %s"), vformat(U"%d × %d × %d", size.x, size.y, size.z)) + "\n";
	text += vformat(TTR("Cell size: %s"), vformat(U"%.3f × %.3f × %.3f", extents.x / size.x, extents.y / size.y, extents.z / size.z)) + "\n";
	text += vformat(TTR("Video RAM size: %s MB (%s)"), String::num(size_mb, 2), size_quality);
	return text;
}

EditorProgress *GPUParticlesCollisionSDF3DEditorPlugin::tmp_progress = nullptr;

void GPUParticlesCollisionSDF3DEditorPlugin::bake_func_begin(int p_steps) {
	ERR_FAIL_COND(tmp_progress != nullptr);

	tmp_progress = memnew(EditorProgress("bake_sdf", TTR("Bake SDF"), p_steps));
}

void GPUParticlesCollisionSDF3DEditorPlugin::bake_func_step(int p_step, const String &p_description) {
	ERR_FAIL_NULL(tmp_progress);
	tmp_progress->step(p_description, p_step, false);
}

void GPUParticlesCollisionSDF3DEditorPlugin::bake_func_end() {
	ERR_FAIL_NULL(tmp_progress);
	memdelete(tmp_progress);
	tmp_progress = nullptr;
}

void GPUParticlesCollisionSDF3DEditorPlugin::_sdf_save_path_and_bake(const String &p_path) {
	probe_file->hide();
	if (col_sdf) {
		Ref<Image> bake_img = col_sdf->bake();
		if (bake_img.is_null()) {
			EditorNode::get_singleton()->show_warning(TTR("No faces detected during GPUParticlesCollisionSDF3D bake.\nCheck whether there are visible meshes matching the bake mask within its extents."));
			return;
		}

		Ref<ConfigFile> config;

		config.instantiate();
		if (FileAccess::exists(p_path + ".import")) {
			config->load(p_path + ".import");
		}

		config->set_value("remap", "importer", "3d_texture");
		config->set_value("remap", "type", "CompressedTexture3D");
		if (!config->has_section_key("params", "compress/mode")) {
			config->set_value("params", "compress/mode", 3); //user may want another compression, so leave it be
		}
		config->set_value("params", "compress/channel_pack", 1);
		config->set_value("params", "mipmaps/generate", false);
		config->set_value("params", "slices/horizontal", 1);
		config->set_value("params", "slices/vertical", bake_img->get_meta("depth"));

		config->save(p_path + ".import");

		Error err = bake_img->save_exr(p_path, false);
		ERR_FAIL_COND(err);
		ResourceLoader::import(p_path);
		Ref<Texture> t = ResourceLoader::load(p_path); //if already loaded, it will be updated on refocus?
		ERR_FAIL_COND(t.is_null());

		col_sdf->set_texture(t);
	}
}

GPUParticlesCollisionSDF3DEditorPlugin::GPUParticlesCollisionSDF3DEditorPlugin() {
	bake_action = EditorSceneActionRegistry::get_singleton()->register_class_action(
			SNAME("gpu_particles_collision_sdf_3d"), SNAME("bake"), SNAME("GPUParticlesCollisionSDF3D"),
			TTR("Bake SDF"), SNAME("Bake"),
			callable_mp(this, &GPUParticlesCollisionSDF3DEditorPlugin::bake_node));
	bake_action->set_tooltip_provider(callable_mp(this, &GPUParticlesCollisionSDF3DEditorPlugin::get_bake_tooltip));

	probe_file = memnew(EditorFileDialog);
	probe_file->set_file_mode(EditorFileDialog::FILE_MODE_SAVE_FILE);
	probe_file->add_filter("*.exr");
	probe_file->connect("file_selected", callable_mp(this, &GPUParticlesCollisionSDF3DEditorPlugin::_sdf_save_path_and_bake));
	EditorInterface::get_singleton()->get_base_control()->add_child(probe_file);
	probe_file->set_title(TTR("Select path for SDF Texture"));

	GPUParticlesCollisionSDF3D::bake_begin_function = bake_func_begin;
	GPUParticlesCollisionSDF3D::bake_step_function = bake_func_step;
	GPUParticlesCollisionSDF3D::bake_end_function = bake_func_end;
}
