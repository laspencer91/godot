/**************************************************************************/
/*  resource_inspector_dock.h                                             */
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

#pragma once

#include "core/io/resource.h"
#include "editor/docks/editor_dock.h"

class Button;
class EditorInspector;
class Label;
class LineEdit;
class VBoxContainer;

// File-backed resource detail view for the workspace FileSystem drawer. This is
// deliberately separate from InspectorDock: scene inspectors are pane/document
// bound, while a selected .tres/.res is a project asset with global resource undo.
class ResourceInspectorDock : public EditorDock {
	GDCLASS(ResourceInspectorDock, EditorDock);

	static ResourceInspectorDock *singleton;

	Ref<Resource> edited_resource;
	String edited_path;
	bool dirty = false;

	VBoxContainer *content = nullptr;
	Label *resource_name = nullptr;
	Button *save_button = nullptr;
	LineEdit *filter = nullptr;
	EditorInspector *inspector = nullptr;
	Label *select_a_resource = nullptr;

	void _set_dirty(bool p_dirty);
	void _save_pressed();
	void _property_edited(const StringName &p_property);
	void _property_deleted(const StringName &p_property);
	void _update_theme();

protected:
	static void _bind_methods();
	void _notification(int p_what);

public:
	static ResourceInspectorDock *get_singleton() { return singleton; }

	void set_edit_path(const String &p_path);
	void clear();

	ResourceInspectorDock();
	~ResourceInspectorDock();
};
