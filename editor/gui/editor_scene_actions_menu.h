/**************************************************************************/
/*  editor_scene_actions_menu.h                                           */
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

#include "editor/gui/editor_scene_actions.h"
#include "scene/gui/button.h"

class Label;
class PopupPanel;
class ScrollContainer;
class VBoxContainer;

// Always-present toolbar dropdown listing every action exposed by the edited
// scene. The list is a pure function of `get_edited_scene()` at open time: the
// rows are rebuilt from scratch in `about_to_popup`, so there is no bookkeeping
// to keep in sync with scene loads, node additions, renames or tab switches.
class EditorSceneActionsMenu : public Button {
	GDCLASS(EditorSceneActionsMenu, Button);

	PopupPanel *popup = nullptr;
	Label *empty_label = nullptr;
	ScrollContainer *scroll = nullptr;
	VBoxContainer *rows = nullptr;

	// Rebuilt per open; indices into it are bound into the row callbacks and are
	// only ever valid until the next rebuild (which frees those rows first).
	LocalVector<EditorSceneActionEntry> entries;
	Button *first_enabled_button = nullptr;

	// Resolves an entry's node and verifies it is still part of the edited scene.
	static Node *_resolve_node(ObjectID p_node_id);

	void _rebuild();
	void _activate(int p_index);
	void _run(int p_index);
	void _select_node(ObjectID p_node_id);

protected:
	void _notification(int p_what);
	// Overridden rather than connected to `pressed`: this class is not in ClassDB, so
	// Object::connect cannot resolve a signal inherited from BaseButton by name.
	virtual void pressed() override;

public:
	EditorSceneActionsMenu();
};
