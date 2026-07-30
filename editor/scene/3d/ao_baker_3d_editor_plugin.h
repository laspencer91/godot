/**************************************************************************/
/*  ao_baker_3d_editor_plugin.h                                          */
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

#include "editor/plugins/editor_plugin.h"

class AOBaker3D;
class Button;
class ConfirmationDialog;
class EditorFileDialog;
class EditorUndoRedoManager;
class Mesh;
class MeshInstance3D;

class AOBaker3DEditorPlugin : public EditorPlugin {
	GDCLASS(AOBaker3DEditorPlugin, EditorPlugin);

	AOBaker3D *baker = nullptr;
	Button *bake = nullptr;

	// Pre-bake prompt shown when some static meshes are missing UV2: offers to auto-unwrap the
	// fixable ones before baking (OK button) or to bake only the ready meshes (custom button).
	ConfirmationDialog *uv2_prompt = nullptr;
	Button *bake_ready_button = nullptr;
	EditorFileDialog *atlas_file = nullptr;
	String pending_atlas_path;

	void _bake(); // Entry point: resolve an external atlas path, then prepare the bake.
	void _atlas_path_selected(const String &p_path); // First bake: remember the selected .png path.
	void _prepare_bake(); // Classify meshes, then prompt or bake.
	void _do_bake(); // Run the actual bake and report the result.
	void _unwrap_and_bake(); // Prompt confirmed: batch-unwrap the fixable meshes (one undo step) then bake.
	void _prompt_custom_action(const String &p_action); // "bake_ready" -> bake ready meshes only.

	// True if p_mesh can be UV2-unwrapped in place; otherwise false with a short reason in r_reason.
	// Mirrors the guards in the Mesh menu's "Unwrap UV2" (imported meshes need Make Unique first, etc.).
	bool _can_unwrap_in_place(const Ref<Mesh> &p_mesh, String &r_reason) const;
	// Queue the do/undo for unwrapping one mesh onto p_ur (does not commit). Returns OK or fills r_error.
	Error _unwrap_mesh_instance(MeshInstance3D *p_mi, EditorUndoRedoManager *p_ur, String &r_error);

protected:
	static void _bind_methods();

public:
	virtual String get_plugin_name() const override { return "AOBaker3D"; }
	virtual void edit(Object *p_object) override;
	virtual bool handles(Object *p_object) const override;
	virtual void make_visible(bool p_visible) override;

	AOBaker3DEditorPlugin();
};
