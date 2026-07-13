/**************************************************************************/
/*  resource_inspector_dock.cpp                                           */
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
/* "Software"), to deal in the Software without restriction, including  */
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

#include "resource_inspector_dock.h"

#include "core/io/resource_loader.h"
#include "core/object/callable_mp.h"
#include "editor/editor_node.h"
#include "editor/editor_string_names.h"
#include "editor/inspector/editor_inspector.h"
#include "editor/themes/editor_scale.h"
#include "scene/gui/box_container.h"
#include "scene/gui/button.h"
#include "scene/gui/label.h"
#include "scene/gui/line_edit.h"
#include "scene/gui/margin_container.h"
#include "scene/gui/scroll_container.h"

ResourceInspectorDock *ResourceInspectorDock::singleton = nullptr;

void ResourceInspectorDock::_set_dirty(bool p_dirty) {
	dirty = p_dirty && edited_resource.is_valid();
	resource_name->set_text(edited_path.get_file() + (dirty ? " (*)" : ""));
	resource_name->set_tooltip_text(edited_path);
	const bool read_only = edited_resource.is_valid() && EditorNode::get_singleton()->is_resource_read_only(edited_resource);
	save_button->set_disabled(edited_resource.is_null() || read_only);
}

void ResourceInspectorDock::_save_pressed() {
	ERR_FAIL_COND(edited_resource.is_null());
	EditorNode::get_singleton()->save_resource(edited_resource);
	_set_dirty(false);
}

void ResourceInspectorDock::_property_edited(const StringName &p_property) {
	_set_dirty(true);
}

void ResourceInspectorDock::_property_deleted(const StringName &p_property) {
	_set_dirty(true);
}

void ResourceInspectorDock::_update_theme() {
	if (save_button) {
		save_button->set_button_icon(get_editor_theme_icon(SNAME("Save")));
	}
	if (filter) {
		filter->set_right_icon(get_editor_theme_icon(SNAME("Search")));
	}
}

void ResourceInspectorDock::set_edit_path(const String &p_path) {
	const String extension = p_path.get_extension().to_lower();
	if ((extension != "tres" && extension != "res") || !ResourceLoader::exists(p_path)) {
		clear();
		return;
	}

	if (edited_resource.is_valid() && edited_path == p_path) {
		emit_signal(SNAME("edit_target_changed"), true);
		return;
	}

	Ref<Resource> resource = ResourceLoader::load(p_path, "", ResourceFormatLoader::CACHE_MODE_REUSE);
	if (resource.is_null()) {
		clear();
		return;
	}

	edited_resource = resource;
	edited_path = p_path;
	filter->clear();
	inspector->edit(edited_resource.ptr());
	content->show();
	select_a_resource->hide();
	_set_dirty(false);
	emit_signal(SNAME("edit_target_changed"), true);
}

void ResourceInspectorDock::clear() {
	const bool had_resource = edited_resource.is_valid();
	edited_resource.unref();
	edited_path.clear();
	dirty = false;
	filter->clear();
	inspector->edit(nullptr);
	content->hide();
	select_a_resource->show();
	resource_name->set_text(String());
	resource_name->set_tooltip_text(String());
	save_button->set_disabled(true);
	if (had_resource) {
		emit_signal(SNAME("edit_target_changed"), false);
	}
}

void ResourceInspectorDock::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE:
		case NOTIFICATION_THEME_CHANGED: {
			_update_theme();
		} break;
	}
}

void ResourceInspectorDock::_bind_methods() {
	ADD_SIGNAL(MethodInfo("edit_target_changed", PropertyInfo(Variant::BOOL, "has_content")));
}

ResourceInspectorDock::ResourceInspectorDock() {
	ERR_FAIL_COND(singleton != nullptr);
	singleton = this;

	set_name(TTRC("Inspector"));
	set_icon_name("Object");

	VBoxContainer *main_vb = memnew(VBoxContainer);
	add_child(main_vb);

	content = memnew(VBoxContainer);
	content->set_v_size_flags(SIZE_EXPAND_FILL);
	main_vb->add_child(content);

	HBoxContainer *header = memnew(HBoxContainer);
	content->add_child(header);

	resource_name = memnew(Label);
	resource_name->set_h_size_flags(SIZE_EXPAND_FILL);
	resource_name->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
	header->add_child(resource_name);

	save_button = memnew(Button);
	save_button->set_text(TTRC("Save"));
	save_button->set_disabled(true);
	save_button->set_tooltip_text(TTRC("Save the selected resource."));
	save_button->connect(SceneStringName(pressed), callable_mp(this, &ResourceInspectorDock::_save_pressed));
	header->add_child(save_button);

	filter = memnew(LineEdit);
	filter->set_placeholder(TTRC("Filter Properties"));
	filter->set_clear_button_enabled(true);
	content->add_child(filter);

	MarginContainer *inspector_margin = memnew(MarginContainer);
	inspector_margin->set_theme_type_variation("NoBorderHorizontalBottom");
	inspector_margin->set_v_size_flags(SIZE_EXPAND_FILL);
	content->add_child(inspector_margin);

	inspector = EditorInspector::create_default_inspector(filter);
	inspector->set_scroll_hint_mode(ScrollContainer::SCROLL_HINT_MODE_ALL);
	inspector->set_use_doc_hints(true);
	inspector->connect("property_edited", callable_mp(this, &ResourceInspectorDock::_property_edited));
	inspector->connect("property_deleted", callable_mp(this, &ResourceInspectorDock::_property_deleted));
	inspector_margin->add_child(inspector);

	select_a_resource = memnew(Label);
	select_a_resource->set_text(TTRC("Select one .tres or .res file to inspect and edit its properties."));
	select_a_resource->set_autowrap_mode(TextServer::AUTOWRAP_WORD);
	select_a_resource->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);
	select_a_resource->set_vertical_alignment(VERTICAL_ALIGNMENT_CENTER);
	select_a_resource->set_custom_minimum_size(Size2(180, 80) * EDSCALE);
	select_a_resource->set_v_size_flags(SIZE_EXPAND_FILL);
	main_vb->add_child(select_a_resource);

	content->hide();
}

ResourceInspectorDock::~ResourceInspectorDock() {
	if (singleton == this) {
		singleton = nullptr;
	}
}
