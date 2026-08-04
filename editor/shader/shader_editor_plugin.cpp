/**************************************************************************/
/*  shader_editor_plugin.cpp                                              */
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

#include "shader_editor_plugin.h"

#include "core/io/resource_loader.h"
#include "core/object/callable_mp.h"
#include "editor/docks/filesystem_dock.h"
#include "editor/docks/inspector_dock.h"
#include "editor/editor_data.h"
#include "editor/editor_document.h"
#include "editor/editor_main_screen.h"
#include "editor/editor_node.h"
#include "editor/editor_undo_redo_manager.h"
#include "editor/gui/document_view.h"
#include "editor/settings/editor_settings.h"
#include "editor/shader/shader_create_dialog.h"
#include "editor/shader/text_shader_editor.h"
#include "editor/shader/text_shader_language_plugin.h"
#include "scene/gui/menu_button.h"
#include "servers/display/display_server.h"

ShaderEditorPlugin *ShaderEditorPlugin::singleton = nullptr;

int ShaderEditorPlugin::_find_edited_shader(const ShaderEditor *p_editor) const {
	for (uint32_t i = 0; i < edited_shaders.size(); i++) {
		if (edited_shaders[i].shader_editor == p_editor) {
			return i;
		}
	}
	return -1;
}

int ShaderEditorPlugin::_current_edited_shader() const {
	// G-Shader: the "current shader" is the focused workspace shader tab (the editor hosting the File
	// menu), tracked by current_shader_editor_id — not a bottom-dock tab index anymore.
	if (current_shader_editor_id.is_valid()) {
		if (ShaderEditor *se = Object::cast_to<ShaderEditor>(ObjectDB::get_instance(current_shader_editor_id))) {
			return _find_edited_shader(se);
		}
	}
	return -1;
}

Ref<Resource> ShaderEditorPlugin::_get_current_shader() {
	int index = _current_edited_shader();
	return index < 0 ? Ref<Resource>() : edited_shaders[index].resource();
}

ShaderEditor *ShaderEditorPlugin::create_editor_view(const Ref<Resource> &p_resource) {
	// G-Shader: mint + wire the widget for one shader resource and track it in `edited_shaders`,
	// WITHOUT parenting it. The workspace tab's DocumentView parents it; the text-vs-visual choice
	// is the shader-language factory's job.
	ERR_FAIL_COND_V(p_resource.is_null(), nullptr);
	EditedShader es;
	ShaderInclude *shader_include = Object::cast_to<ShaderInclude>(p_resource.ptr());
	if (shader_include != nullptr) {
		es.shader_inc = Ref<ShaderInclude>(shader_include);
		for (Ref<EditorShaderLanguagePlugin> shader_lang : EditorShaderLanguagePlugin::get_shader_languages_read_only()) {
			if (shader_lang->handles_shader_include(es.shader_inc)) {
				es.shader_editor = shader_lang->edit_shader_include(es.shader_inc);
				break;
			}
		}
	} else {
		Shader *shader = Object::cast_to<Shader>(p_resource.ptr());
		ERR_FAIL_NULL_V_MSG(shader, nullptr, "ShaderEditorPlugin: Unable to edit resource because it is not a Shader or ShaderInclude.");
		es.shader = Ref<Shader>(shader);
		for (Ref<EditorShaderLanguagePlugin> shader_lang : EditorShaderLanguagePlugin::get_shader_languages_read_only()) {
			if (shader_lang->handles_shader(es.shader)) {
				es.shader_editor = shader_lang->edit_shader(es.shader);
				break;
			}
		}
	}

	ERR_FAIL_NULL_V_MSG(es.shader_editor, nullptr, "ShaderEditorPlugin: Unable to edit shader because no suitable editor was found.");

	// Cache the path (used by _file_removed to match a deleted file, and by get_unsaved_status for
	// the display name, now that the bottom list — which used to fill these — is gone).
	es.path = p_resource->get_path();

	// No bottom list to toggle in the workspace; the pane tab bar is the shader list.
	es.shader_editor->set_toggle_list_control(nullptr);

	// TextShaderEditor-specific setup code.
	TextShaderEditor *text_shader_editor = Object::cast_to<TextShaderEditor>(es.shader_editor);
	if (text_shader_editor) {
		CodeTextEditor *cte = text_shader_editor->get_code_editor();
		if (cte) {
			cte->set_zoom_factor(text_shader_zoom_factor);
			cte->connect("zoomed", callable_mp(this, &ShaderEditorPlugin::_set_text_shader_zoom_factor));
			cte->connect(SceneStringName(visibility_changed), callable_mp(this, &ShaderEditorPlugin::_update_shader_editor_zoom_factor).bind(cte));
		}
	}

	edited_shaders.push_back(es);
	return es.shader_editor;
}

void ShaderEditorPlugin::release_editor_view(ShaderEditor *p_editor) {
	// G-Shader: the DocumentView is about to free this shader surface (PREDELETE). Park the traveling
	// File menu if it is hosted here (so it survives the view's teardown) and drop our tracking entry.
	if (!p_editor) {
		return;
	}
	if (file_menu && p_editor->is_ancestor_of(file_menu)) {
		_park_file_menu();
	}
	if (current_shader_editor_id == p_editor->get_instance_id()) {
		current_shader_editor_id = ObjectID();
	}
	int idx = _find_edited_shader(p_editor);
	if (idx >= 0) {
		edited_shaders.remove_at(idx);
	}
}

void ShaderEditorPlugin::_park_file_menu() {
	if (!file_menu || !menu_home) {
		return;
	}
	if (file_menu->get_parent() == menu_home) {
		return;
	}
	if (file_menu->get_parent()) {
		file_menu->reparent(menu_home);
	} else {
		menu_home->add_child(file_menu);
	}
}

void ShaderEditorPlugin::set_current_surface(DocumentView *p_view) {
	// G-Shader (focus hook): a shader tab becoming current hosts the shared File menu in its own
	// editor toolbar (ShaderEditor::use_menu_bar); any other kind of tab parks it. Driven by
	// TabbedDocumentHost / EditorWorkspace, mirroring ScriptEditor::set_current_surface (which also
	// takes the view and extracts the surface here).
	ShaderEditor *se = p_view ? Object::cast_to<ShaderEditor>(p_view->get_editor_surface()) : nullptr;
	const ObjectID new_id = se ? se->get_instance_id() : ObjectID();
	if (new_id == current_shader_editor_id) {
		return;
	}
	current_shader_editor_id = new_id;
	if (se) {
		if (file_menu->get_parent()) {
			file_menu->get_parent()->remove_child(file_menu);
		}
		se->use_menu_bar(file_menu);
	} else {
		_park_file_menu();
	}
}

void ShaderEditorPlugin::edit(Object *p_object) {
	if (!p_object) {
		return;
	}
	Resource *res = Object::cast_to<Resource>(p_object);
	ERR_FAIL_NULL_MSG(res, "ShaderEditorPlugin: Unable to edit object " + p_object->to_string() + " because it is not a Shader or ShaderInclude.");
	ERR_FAIL_COND_MSG(!Object::cast_to<Shader>(res) && !Object::cast_to<ShaderInclude>(res),
			"ShaderEditorPlugin: Unable to edit object " + p_object->to_string() + " because it is not a Shader or ShaderInclude.");

	// G-Shader: shaders open as workspace tabs. The document dedups; reveal() focuses an existing tab
	// or opens a new one (whose DocumentView mints the editor via create_editor_view).
	ShaderDocument *doc = EditorNode::get_editor_data().get_or_create_shader_document(Ref<Resource>(res));
	EditorNode::get_singleton()->get_editor_main_screen()->reveal(doc);
}

bool ShaderEditorPlugin::handles(Object *p_object) const {
	return Object::cast_to<Shader>(p_object) != nullptr || Object::cast_to<ShaderInclude>(p_object) != nullptr;
}

void ShaderEditorPlugin::make_visible(bool p_visible) {
	// G-Shader: not a main-screen plugin — shaders are workspace tabs, visibility is the tab's job.
}

void ShaderEditorPlugin::set_current() {
	// Only focus the shader text editor when the shader was directly opened (upstream behavior),
	// adapted to the workspace model: revealing the tab is edit()'s job; focus the current shader
	// tab's text editor if one is hosting the File menu.
	if (!current_shader_editor_id.is_valid()) {
		return;
	}
	TextShaderEditor *text_shader_editor = Object::cast_to<TextShaderEditor>(ObjectDB::get_instance(current_shader_editor_id));
	if (text_shader_editor) {
		text_shader_editor->get_code_editor()->get_text_editor()->grab_focus();
	}
}

ShaderEditor *ShaderEditorPlugin::get_shader_editor(const Ref<Shader> &p_for_shader) {
	for (EditedShader &edited_shader : edited_shaders) {
		if (edited_shader.shader == p_for_shader) {
			return edited_shader.shader_editor;
		}
	}
	return nullptr;
}

void ShaderEditorPlugin::set_window_layout(Ref<ConfigFile> p_layout) {
	// G-Shader: which shaders are open (and in which pane) is restored by the workspace session
	// (EditorMainScreen tabs -> get_or_create_document_for_path -> a ShaderDocument), so we no longer
	// reopen shaders here. Only the shared text-zoom preference rides in this legacy blob.
	_set_text_shader_zoom_factor(p_layout->get_value("ShaderEditor", "text_shader_zoom_factor", 1.0f));
}

void ShaderEditorPlugin::get_window_layout(Ref<ConfigFile> p_layout) {
#ifndef DISABLE_DEPRECATED
	if (p_layout->has_section_key("ShaderEditor", "window_rect")) {
		p_layout->erase_section_key("ShaderEditor", "window_rect");
	}
	if (p_layout->has_section_key("ShaderEditor", "window_screen")) {
		p_layout->erase_section_key("ShaderEditor", "window_screen");
	}
	if (p_layout->has_section_key("ShaderEditor", "window_screen_rect")) {
		p_layout->erase_section_key("ShaderEditor", "window_screen_rect");
	}
	// The open-shader list moved to the workspace session; drop the stale keys if a pre-migration
	// layout still carries them.
	if (p_layout->has_section_key("ShaderEditor", "open_shaders")) {
		p_layout->erase_section_key("ShaderEditor", "open_shaders");
	}
	if (p_layout->has_section_key("ShaderEditor", "selected_shader")) {
		p_layout->erase_section_key("ShaderEditor", "selected_shader");
	}
	if (p_layout->has_section_key("ShaderEditor", "split_offset")) {
		p_layout->erase_section_key("ShaderEditor", "split_offset");
	}
#endif
	p_layout->set_value("ShaderEditor", "text_shader_zoom_factor", text_shader_zoom_factor);
}

String ShaderEditorPlugin::get_unsaved_status(const String &p_for_scene) const {
	// TODO: This should also include visual shaders and shader includes, but save_external_data() doesn't seem to save them...
	PackedStringArray unsaved_shaders;
	for (uint32_t i = 0; i < edited_shaders.size(); i++) {
		if (edited_shaders[i].shader_editor) {
			if (edited_shaders[i].shader_editor->is_unsaved()) {
				if (unsaved_shaders.is_empty()) {
					unsaved_shaders.append(TTR("Save changes to the following shaders(s) before quitting?"));
				}
				String display = edited_shaders[i].path.get_file();
				unsaved_shaders.append(display.is_empty() ? TTR("[unsaved]") : display);
			}
		}
	}

	if (!p_for_scene.is_empty()) {
		PackedStringArray unsaved_built_in_shaders;

		const String scene_file = p_for_scene.get_file();
		for (const String &E : unsaved_shaders) {
			if (!E.is_resource_file() && E.contains(scene_file)) {
				if (unsaved_built_in_shaders.is_empty()) {
					unsaved_built_in_shaders.append(TTR("There are unsaved changes in the following built-in shaders(s):"));
				}
				unsaved_built_in_shaders.append(E);
			}
		}

		if (!unsaved_built_in_shaders.is_empty()) {
			return String("\n").join(unsaved_built_in_shaders);
		}
		return String();
	}

	return String("\n").join(unsaved_shaders);
}

void ShaderEditorPlugin::save_external_data() {
	for (EditedShader &edited_shader : edited_shaders) {
		if (edited_shader.shader_editor && edited_shader.shader_editor->is_unsaved()) {
			edited_shader.shader_editor->save_external_data();
		}
	}
}

void ShaderEditorPlugin::apply_changes() {
	for (EditedShader &edited_shader : edited_shaders) {
		if (edited_shader.shader_editor) {
			edited_shader.shader_editor->apply_shaders();
		}
	}
}

void ShaderEditorPlugin::_close_current_shader() {
	int index = _current_edited_shader();
	if (index < 0) {
		return;
	}
	Ref<Resource> res = edited_shaders[index].resource();
	if (res.is_null()) {
		return;
	}
	// Close the workspace tab; its DocumentView PREDELETE calls release_editor_view (drops tracking).
	ShaderDocument *doc = EditorNode::get_editor_data().get_or_create_shader_document(res);
	EditorNode::get_singleton()->get_editor_main_screen()->close_document(doc);
	EditorUndoRedoManager::get_singleton()->clear_history(); // To prevent undo on deleted graphs.
}

void ShaderEditorPlugin::_close_builtin_shaders_from_scene(const String &p_scene) {
	// Collect first — closing mutates edited_shaders (via release_editor_view).
	Vector<Ref<Resource>> to_close;
	for (const EditedShader &edited_shader : edited_shaders) {
		const Ref<Shader> &shader = edited_shader.shader;
		if (shader.is_valid() && shader->is_built_in() && shader->get_path().begins_with(p_scene)) {
			to_close.push_back(shader);
			continue;
		}
		const Ref<ShaderInclude> &include = edited_shader.shader_inc;
		if (include.is_valid() && include->is_built_in() && include->get_path().begins_with(p_scene)) {
			to_close.push_back(include);
		}
	}
	EditorMainScreen *main_screen = EditorNode::get_singleton()->get_editor_main_screen();
	for (const Ref<Resource> &res : to_close) {
		main_screen->close_document(EditorNode::get_editor_data().get_or_create_shader_document(res));
	}
}

void ShaderEditorPlugin::_menu_item_pressed(int p_index) {
	switch (p_index) {
		case FILE_MENU_NEW: {
			String base_path = FileSystemDock::get_singleton()->get_current_path().get_base_dir();
			shader_create_dialog->config(base_path.path_join("new_shader"), false, false, "Shader");
			shader_create_dialog->popup_centered();
		} break;
		case FILE_MENU_NEW_INCLUDE: {
			String base_path = FileSystemDock::get_singleton()->get_current_path().get_base_dir();
			shader_create_dialog->config(base_path.path_join("new_shader"), false, false, "ShaderInclude");
			shader_create_dialog->popup_centered();
		} break;
		case FILE_MENU_OPEN: {
			InspectorDock::get_singleton()->open_resource("Shader");
		} break;
		case FILE_MENU_OPEN_INCLUDE: {
			InspectorDock::get_singleton()->open_resource("ShaderInclude");
		} break;
		case FILE_MENU_SAVE: {
			int index = _current_edited_shader();
			_save_view(index >= 0 ? edited_shaders[index].shader_editor : nullptr, false);
		} break;
		case FILE_MENU_SAVE_AS: {
			int index = _current_edited_shader();
			_save_view(index >= 0 ? edited_shaders[index].shader_editor : nullptr, true);
		} break;
		case FILE_MENU_INSPECT: {
			int index = _current_edited_shader();
			if (index < 0) {
				return;
			}
			if (edited_shaders[index].shader.is_valid()) {
				EditorNode::get_singleton()->push_item(edited_shaders[index].shader.ptr());
			} else {
				EditorNode::get_singleton()->push_item(edited_shaders[index].shader_inc.ptr());
			}
		} break;
		case FILE_MENU_INSPECT_NATIVE_SHADER_CODE: {
			int index = _current_edited_shader();
			if (index >= 0 && edited_shaders[index].shader.is_valid()) {
				edited_shaders[index].shader->inspect_native_shader_code();
			}
		} break;
		case FILE_MENU_CLOSE: {
			_close_current_shader();
		} break;
		case FILE_MENU_SHOW_IN_FILE_SYSTEM: {
			Ref<Resource> shader = _get_current_shader();
			if (shader.is_null()) {
				return;
			}
			String path = shader->get_path();
			if (!path.is_empty()) {
				FileSystemDock::get_singleton()->navigate_to_path(path);
			}
		} break;
		case FILE_MENU_COPY_PATH: {
			Ref<Resource> shader = _get_current_shader();
			if (shader.is_valid()) {
				DisplayServer::get_singleton()->clipboard_set(shader->get_path());
			}
		} break;
	}
}

bool ShaderEditorPlugin::_save_view(ShaderEditor *p_editor, bool p_save_as) {
	const int index = _find_edited_shader(p_editor);
	if (index < 0) {
		return false;
	}

	TextShaderEditor *text_editor = Object::cast_to<TextShaderEditor>(edited_shaders[index].shader_editor);
	if (text_editor) {
		if (text_editor->get_trim_trailing_whitespace_on_save()) {
			text_editor->trim_trailing_whitespace();
		}
		if (text_editor->get_trim_final_newlines_on_save()) {
			text_editor->trim_final_newlines();
		}
	}

	Ref<Resource> resource = edited_shaders[index].resource();
	if (resource.is_null()) {
		return false;
	}
	if (p_save_as) {
		String path = resource->get_path();
		if (!path.is_resource_file()) {
			path = "";
		}
		EditorNode::get_singleton()->save_resource_as(resource, path);
	} else {
		EditorNode::get_singleton()->save_resource(resource);
		if (text_editor) {
			text_editor->tag_saved_version();
		}
	}
	return true;
}

void ShaderEditorPlugin::_shader_created(Ref<Shader> p_shader) {
	EditorNode::get_singleton()->push_item(p_shader.ptr());
}

void ShaderEditorPlugin::_shader_include_created(Ref<ShaderInclude> p_shader_inc) {
	EditorNode::get_singleton()->push_item(p_shader_inc.ptr());
}

void ShaderEditorPlugin::_set_text_shader_zoom_factor(float p_zoom_factor) {
	if (text_shader_zoom_factor == p_zoom_factor) {
		return;
	}

	text_shader_zoom_factor = p_zoom_factor;
}

void ShaderEditorPlugin::_update_shader_editor_zoom_factor(CodeTextEditor *p_shader_editor) const {
	if (p_shader_editor && p_shader_editor->is_visible_in_tree() && text_shader_zoom_factor != p_shader_editor->get_zoom_factor()) {
		p_shader_editor->set_zoom_factor(text_shader_zoom_factor);
	}
}

void ShaderEditorPlugin::_file_removed(const String &p_removed_file) {
	for (uint32_t i = 0; i < edited_shaders.size(); i++) {
		if (edited_shaders[i].path == p_removed_file) {
			Ref<Resource> res = edited_shaders[i].resource();
			if (res.is_valid()) {
				EditorNode::get_singleton()->get_editor_main_screen()->close_document(EditorNode::get_editor_data().get_or_create_shader_document(res));
			}
			break;
		}
	}
}

void ShaderEditorPlugin::_res_saved_callback(const Ref<Resource> &p_res) {
	if (p_res.is_null()) {
		return;
	}
	const String &path = p_res->get_path();

	for (EditedShader &edited : edited_shaders) {
		Ref<Resource> shader_res = edited.resource();
		ERR_FAIL_COND(shader_res.is_null());

		TextShaderEditor *text_shader_editor = Object::cast_to<TextShaderEditor>(edited.shader_editor);
		if (!text_shader_editor) {
			continue;
		}

		if (shader_res == p_res || (shader_res->is_built_in() && shader_res->get_path().get_slice("::", 0) == path)) {
			text_shader_editor->tag_saved_version();
		}
	}
}

void ShaderEditorPlugin::_setup_file_menu(PopupMenu *p_menu) {
	p_menu->add_shortcut(ED_GET_SHORTCUT("shader_editor/new"), FILE_MENU_NEW);
	p_menu->add_shortcut(ED_GET_SHORTCUT("shader_editor/new_include"), FILE_MENU_NEW_INCLUDE);
	p_menu->add_separator();
	p_menu->add_shortcut(ED_GET_SHORTCUT("shader_editor/open"), FILE_MENU_OPEN);
	p_menu->add_shortcut(ED_GET_SHORTCUT("shader_editor/open_include"), FILE_MENU_OPEN_INCLUDE);
	p_menu->add_separator();
	p_menu->add_shortcut(ED_GET_SHORTCUT("script_editor/save"), FILE_MENU_SAVE);
	p_menu->add_shortcut(ED_GET_SHORTCUT("script_editor/save_as"), FILE_MENU_SAVE_AS);
	p_menu->add_separator();
	p_menu->add_shortcut(ED_GET_SHORTCUT("shader_editor/open_in_inspector"), FILE_MENU_INSPECT);
	p_menu->add_shortcut(ED_GET_SHORTCUT("shader_editor/inspect_native_code"), FILE_MENU_INSPECT_NATIVE_SHADER_CODE);
	p_menu->add_shortcut(ED_GET_SHORTCUT("shader_editor/copy_path"), FILE_MENU_COPY_PATH);
	p_menu->add_shortcut(ED_GET_SHORTCUT("script_editor/show_in_file_system"), FILE_MENU_SHOW_IN_FILE_SYSTEM);
	p_menu->add_separator();
	p_menu->add_shortcut(ED_GET_SHORTCUT("script_editor/close_file"), FILE_MENU_CLOSE);
}

void ShaderEditorPlugin::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_READY: {
			EditorNode::get_singleton()->connect("scene_closed", callable_mp(this, &ShaderEditorPlugin::_close_builtin_shaders_from_scene));
			FileSystemDock::get_singleton()->connect("file_removed", callable_mp(this, &ShaderEditorPlugin::_file_removed));
			EditorNode::get_singleton()->connect("resource_saved", callable_mp(this, &ShaderEditorPlugin::_res_saved_callback));
		} break;
	}
}

ShaderEditorPlugin::ShaderEditorPlugin() {
	singleton = this;
	ED_SHORTCUT("shader_editor/new", TTRC("New Shader..."), KeyModifierMask::CMD_OR_CTRL | Key::N);
	ED_SHORTCUT("shader_editor/new_include", TTRC("New Shader Include..."), KeyModifierMask::CMD_OR_CTRL | KeyModifierMask::SHIFT | Key::N);
	ED_SHORTCUT("shader_editor/open", TTRC("Load Shader File..."), KeyModifierMask::CMD_OR_CTRL | Key::O);
	ED_SHORTCUT("shader_editor/open_include", TTRC("Load Shader Include File..."), KeyModifierMask::CMD_OR_CTRL | KeyModifierMask::SHIFT | Key::O);
	ED_SHORTCUT("shader_editor/open_in_inspector", TTRC("Open File in Inspector"));
	ED_SHORTCUT("shader_editor/inspect_native_code", TTRC("Inspect Native Shader Code..."));
	ED_SHORTCUT("shader_editor/copy_path", TTRC("Copy Shader Path"));

	// G-Shader: hidden home the File menu parks under when no shader tab is focused. It travels into
	// the current shader editor's own toolbar (ShaderEditor::use_menu_bar) via set_current_surface.
	menu_home = memnew(Control);
	menu_home->set_name("ShaderMenuHome");
	menu_home->hide();
	add_child(menu_home);

	file_menu = memnew(MenuButton);
	file_menu->set_flat(false);
	file_menu->set_theme_type_variation("FlatMenuButton");
	file_menu->set_text(TTRC("File"));
	file_menu->set_h_size_flags(Control::SIZE_SHRINK_BEGIN);
	file_menu->set_switch_on_hover(true);
	file_menu->get_popup()->connect(SceneStringName(id_pressed), callable_mp(this, &ShaderEditorPlugin::_menu_item_pressed));
	_setup_file_menu(file_menu->get_popup());
	menu_home->add_child(file_menu);

	shader_create_dialog = memnew(ShaderCreateDialog);
	shader_create_dialog->connect("shader_created", callable_mp(this, &ShaderEditorPlugin::_shader_created));
	shader_create_dialog->connect("shader_include_created", callable_mp(this, &ShaderEditorPlugin::_shader_include_created));
	add_child(shader_create_dialog);

	Ref<TextShaderLanguagePlugin> text_shader_lang;
	text_shader_lang.instantiate();
	EditorShaderLanguagePlugin::register_shader_language(text_shader_lang);
}

ShaderEditorPlugin::~ShaderEditorPlugin() {
	EditorShaderLanguagePlugin::clear_registered_shader_languages();
	// file_menu lives under menu_home (a child of this plugin) unless it is currently mounted in a
	// shader editor; release_editor_view parks it home before any such editor dies, so the plugin's
	// node teardown frees it normally — no explicit memdelete here.
	singleton = nullptr;
}
