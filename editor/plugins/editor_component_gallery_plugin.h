/**************************************************************************/
/*  editor_component_gallery_plugin.h                                     */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                          */
/**************************************************************************/

#pragma once

#include "editor/plugins/editor_plugin.h"

class AcceptDialog;
class EditorAction;

class EditorComponentGalleryPlugin : public EditorPlugin {
	GDCLASS(EditorComponentGalleryPlugin, EditorPlugin);

	AcceptDialog *gallery = nullptr;
	String menu_name;

	void _build_gallery();
	void _open_gallery();
	Ref<EditorAction> _make_action(const String &p_text, const StringName &p_icon = StringName(), bool p_enabled = true);

protected:
	static void _bind_methods() {}

public:
	EditorComponentGalleryPlugin();
	~EditorComponentGalleryPlugin();
};
