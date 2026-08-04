/**************************************************************************/
/*  shader_editor_plugin.h                                                */
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
#include "scene/resources/shader.h"

class CodeTextEditor;
class Control;
class DocumentView;
class MenuButton;
class ShaderCreateDialog;
class ShaderEditor;

class ShaderEditorPlugin : public EditorPlugin {
	GDCLASS(ShaderEditorPlugin, EditorPlugin);

	static ShaderEditorPlugin *singleton;

	struct EditedShader {
		Ref<Shader> shader;
		Ref<ShaderInclude> shader_inc;
		ShaderEditor *shader_editor = nullptr;
		String path;

		// The edited resource, whichever kind this entry holds.
		Ref<Resource> resource() const { return shader.is_valid() ? Ref<Resource>(shader) : Ref<Resource>(shader_inc); }
	};

	LocalVector<EditedShader> edited_shaders;

	enum FileMenu {
		FILE_MENU_NEW,
		FILE_MENU_NEW_INCLUDE,
		FILE_MENU_OPEN,
		FILE_MENU_OPEN_INCLUDE,
		FILE_MENU_SAVE,
		FILE_MENU_SAVE_AS,
		FILE_MENU_INSPECT,
		FILE_MENU_INSPECT_NATIVE_SHADER_CODE,
		FILE_MENU_CLOSE,
		FILE_MENU_SHOW_IN_FILE_SYSTEM,
		FILE_MENU_COPY_PATH,
	};

	// G-Shader: the File menu is chrome that travels into the focused shader tab's editor widget
	// (ShaderEditor::use_menu_bar). menu_home is where it parks when no shader tab is current;
	// current_shader_editor_id is the editor currently hosting it (the "current shader" the menu acts on).
	MenuButton *file_menu = nullptr;
	Control *menu_home = nullptr;
	ObjectID current_shader_editor_id;

	ShaderCreateDialog *shader_create_dialog = nullptr;

	float text_shader_zoom_factor = 1.0f;

	int _find_edited_shader(const ShaderEditor *p_editor) const;
	int _current_edited_shader() const; // Index of current_shader_editor_id in edited_shaders, or -1.
	bool _save_view(ShaderEditor *p_editor, bool p_save_as);
	Ref<Resource> _get_current_shader();
	void _park_file_menu(); // Return the File menu to menu_home (no shader tab focused / one is dying).
	void _setup_file_menu(PopupMenu *p_menu);
	void _menu_item_pressed(int p_index);
	void _close_current_shader();
	void _close_builtin_shaders_from_scene(const String &p_scene);
	void _file_removed(const String &p_removed_file);
	void _res_saved_callback(const Ref<Resource> &p_res);

	void _shader_created(Ref<Shader> p_shader);
	void _shader_include_created(Ref<ShaderInclude> p_shader_inc);

	void _set_text_shader_zoom_factor(float p_zoom_factor);
	void _update_shader_editor_zoom_factor(CodeTextEditor *p_shader_editor) const;

protected:
	void _notification(int p_what);

public:
	static ShaderEditorPlugin *get_singleton() { return singleton; }

	virtual String get_plugin_name() const override { return "Shader"; }
	virtual void edit(Object *p_object) override;
	virtual bool handles(Object *p_object) const override;
	virtual void make_visible(bool p_visible) override;
	virtual void set_current() override;

	// G-Shader: mint + wire the editor widget for one shader resource (text -> code editor, visual ->
	// node-graph editor, via the shader-language factory) and track it, without parenting it anywhere.
	// The workspace tab's DocumentView parents it; release_editor_view drops the tracking when it dies.
	ShaderEditor *create_editor_view(const Ref<Resource> &p_resource);
	void release_editor_view(ShaderEditor *p_editor);

	// G-Shader: focus hook (driven by TabbedDocumentHost / EditorWorkspace, like ScriptEditor). When a
	// shader tab becomes current its editor hosts the File menu; any other kind of tab parks it.
	void set_current_surface(DocumentView *p_view);
	bool save_view(ShaderEditor *p_editor) { return _save_view(p_editor, false); }
	bool save_view_as(ShaderEditor *p_editor) { return _save_view(p_editor, true); }

	ShaderEditor *get_shader_editor(const Ref<Shader> &p_for_shader);

	virtual void set_window_layout(Ref<ConfigFile> p_layout) override;
	virtual void get_window_layout(Ref<ConfigFile> p_layout) override;

	virtual String get_unsaved_status(const String &p_for_scene) const override;
	virtual void save_external_data() override;
	virtual void apply_changes() override;

	ShaderEditorPlugin();
	~ShaderEditorPlugin();
};
