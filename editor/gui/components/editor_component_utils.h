/**************************************************************************/
/*  editor_component_utils.h                                              */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                          */
/**************************************************************************/

#pragma once

#include "editor/gui/components/editor_action.h"
#include "editor/gui/components/editor_action_button.h"
#include "scene/gui/control.h"
#include "scene/gui/texture_rect.h"

// Small shared building blocks for the editor component library. They exist so
// the components below stay declarative: every one of these was written out by
// hand in three to six places before.

// Deletes the caller-visible children of `p_node`, leaving internal children
// (timers, helper nodes added with INTERNAL_MODE_*) alone.
inline void editor_component_free_children(Node *p_node) {
	while (p_node->get_child_count(false) > 0) {
		memdelete(p_node->get_child(0, false));
	}
}

// Applies an optional editor icon, hiding the rect when no icon is named. The
// texture is only fetched inside the tree; outside it the theme is not resolved
// yet and NOTIFICATION_THEME_CHANGED on entry runs the lookup again anyway.
inline void editor_component_update_icon(Control *p_owner, TextureRect *p_rect, const StringName &p_icon_name) {
	p_rect->set_visible(!p_icon_name.is_empty());
	if (!p_owner->is_inside_tree()) {
		return;
	}
	p_rect->set_texture(p_icon_name.is_empty() ? Ref<Texture2D>() : p_owner->get_editor_theme_icon(p_icon_name));
}

// Creates the button that presents `p_action` and parents it to `p_parent`.
inline EditorActionButton *editor_component_add_action(const Ref<EditorAction> &p_action, Node *p_parent, bool p_emphasized = false) {
	ERR_FAIL_COND_V(p_action.is_null(), nullptr);
	EditorActionButton *button = memnew(EditorActionButton);
	button->set_emphasized(p_emphasized);
	button->set_action(p_action);
	p_parent->add_child(button);
	return button;
}
