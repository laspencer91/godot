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

#include "core/object/class_db.h"
#include "core/os/os.h"
#include "editor/editor_node.h"
#include "editor/editor_string_names.h"
#include "scene/3d/ao_baker_3d.h"
#include "scene/gui/button.h"
#include "servers/display/display_server.h"
#include "servers/rendering/rendering_server.h"

#include "modules/modules_enabled.gen.h" // For lightmapper_rd.

void AOBaker3DEditorPlugin::_bake() {
	if (!baker) {
		return;
	}
	const uint64_t time_started = OS::get_singleton()->get_ticks_msec();
	AOBaker3D::BakeError err = baker->bake();
	const int time_taken = OS::get_singleton()->get_ticks_msec() - time_started;

	switch (err) {
		case AOBaker3D::BAKE_ERROR_OK: {
			// ao_masks has STORAGE usage, so it persists when the scene is saved (Ctrl+S).
			print_line(vformat("Done baking AO in %d ms (%d masks).", time_taken, baker->get_ao_masks().size()));
		} break;
		case AOBaker3D::BAKE_ERROR_NO_MESHES: {
			EditorNode::get_singleton()->show_warning(TTR("No meshes to bake AO for. Meshes need UV2 data and their Global Illumination property set to Static."));
		} break;
		case AOBaker3D::BAKE_ERROR_NO_LIGHTMAPPER: {
			EditorNode::get_singleton()->show_warning(TTR("AO baking is not supported on this GPU or build."));
		} break;
		case AOBaker3D::BAKE_ERROR_BAKE_FAILED: {
			EditorNode::get_singleton()->show_warning(TTR("AO bake failed. See the output log for details."));
		} break;
	}
}

void AOBaker3DEditorPlugin::edit(Object *p_object) {
	AOBaker3D *b = Object::cast_to<AOBaker3D>(p_object);
	if (b) {
		baker = b;
	}
}

bool AOBaker3DEditorPlugin::handles(Object *p_object) const {
	return p_object->is_class("AOBaker3D");
}

void AOBaker3DEditorPlugin::make_visible(bool p_visible) {
	if (p_visible) {
		bake->show();
	} else {
		bake->hide();
	}
}

void AOBaker3DEditorPlugin::_bind_methods() {
	ClassDB::bind_method("_bake", &AOBaker3DEditorPlugin::_bake);
}

AOBaker3DEditorPlugin::AOBaker3DEditorPlugin() {
	bake = memnew(Button);
	bake->set_theme_type_variation(SceneStringName(FlatButton));
	bake->set_button_icon(EditorNode::get_singleton()->get_editor_theme()->get_icon(SNAME("Bake"), EditorStringName(EditorIcons)));
	bake->set_text(TTR("Bake AO"));

#ifdef MODULE_LIGHTMAPPER_RD_ENABLED
	if (!DisplayServer::get_singleton()->can_create_rendering_device()) {
		bake->set_disabled(true);
		bake->set_tooltip_text(vformat(TTR("AO baking is not supported on this GPU (%s)."), RenderingServer::get_singleton()->get_video_adapter_name()));
	}
#else
	bake->set_disabled(true);
	bake->set_tooltip_text(TTR("AO cannot be baked, as the `lightmapper_rd` module was disabled at compile-time."));
#endif

	bake->hide();
	bake->connect(SceneStringName(pressed), Callable(this, "_bake"));
	add_control_to_container(CONTAINER_SPATIAL_EDITOR_MENU, bake);
}
