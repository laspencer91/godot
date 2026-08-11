/**************************************************************************/
/*  derived_data_inspector_plugin.h                                       */
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

#include "core/templates/local_vector.h"
#include "editor/gui/editor_scene_actions.h"
#include "editor/inspector/editor_inspector.h"
#include "scene/gui/box_container.h"

class Button;
class Label;
class Node;
class PopupMenu;
class TextureRect;

// The inline row injected under a derived property whose bundle the node does not
// own. It carries only an address (node ObjectID + property name): the node is
// re-resolved at press time, exactly like a scene action entry, so a row that
// outlives its node can never call into freed memory.
class DerivedDataSharedBakeRow : public HBoxContainer {
	GDCLASS(DerivedDataSharedBakeRow, HBoxContainer);

	ObjectID node_id;
	StringName property;

	TextureRect *icon = nullptr;
	Label *label = nullptr;
	Button *rebake_button = nullptr;
	PopupMenu *action_menu = nullptr;

	// Refilled on every press; the menu indices index into this.
	LocalVector<EditorSceneActionEntry> actions;

	void _rebake_pressed();
	void _action_selected(int p_index);
	void _invoke(const EditorSceneActionEntry &p_entry);
	void _clear_inherited(Node *p_node);

protected:
	void _notification(int p_what);

public:
	DerivedDataSharedBakeRow(Node *p_node, const StringName &p_property, const String &p_slot, const Dictionary &p_manifest);
};

// A8's inspector surface for the derived-data store: a property pointing at a
// bundle allocated to *another* node is the one failure this system cannot fix
// silently, because both nodes look correct in isolation. Detection is O(1) —
// one manifest read via describe(), then owns() — so it can run for every
// property of every inspected node without a directory walk. Answering "who
// else shares this bundle?" stays in the Derived Data dialog: only a full disk
// scan can know, and a live inspector row would be answering from stale state.
class DerivedDataInspectorPlugin : public EditorInspectorPlugin {
	GDCLASS(DerivedDataInspectorPlugin, EditorInspectorPlugin);

	static String _artifact_path(const Variant &p_value);
	static bool _identity_settled(Node *p_node);

public:
	virtual bool can_handle(Object *p_object) override;
	virtual bool parse_property(Object *p_object, const Variant::Type p_type, const String &p_path, const PropertyHint p_hint, const String &p_hint_text, const BitField<PropertyUsageFlags> p_usage, const bool p_wide) override;
};
