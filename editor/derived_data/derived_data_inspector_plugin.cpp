/**************************************************************************/
/*  derived_data_inspector_plugin.cpp                                     */
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

#include "derived_data_inspector_plugin.h"

#include "core/config/project_settings.h"
#include "core/io/resource.h"
#include "core/io/resource_uid.h"
#include "core/object/callable_mp.h"
#include "editor/derived_data/editor_derived_data.h"
#include "editor/docks/inspector_dock.h"
#include "editor/editor_node.h"
#include "editor/editor_string_names.h"
#include "editor/editor_undo_redo_manager.h"
#include "editor/themes/editor_scale.h"
#include "scene/gui/button.h"
#include "scene/gui/label.h"
#include "scene/gui/popup_menu.h"
#include "scene/gui/texture_rect.h"
#include "scene/main/node.h"
#include "scene/scene_string_names.h"

// Shared-bake row ///////////////////////////////////////////////////////////

DerivedDataSharedBakeRow::DerivedDataSharedBakeRow(Node *p_node, const StringName &p_property, const String &p_slot, const Dictionary &p_manifest) {
	node_id = p_node->get_instance_id();
	property = p_property;

	icon = memnew(TextureRect);
	icon->set_stretch_mode(TextureRect::STRETCH_KEEP_CENTERED);
	add_child(icon);

	label = memnew(Label);
	label->set_text(TTR("Bake inherited from another node; rebake to own it."));
	label->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	label->set_h_size_flags(SIZE_EXPAND_FILL);
	add_child(label);

	rebake_button = memnew(EditorInspectorActionButton(TTR("Rebake to Own"), SNAME("Bake")));
	rebake_button->connect(SceneStringName(pressed), callable_mp(this, &DerivedDataSharedBakeRow::_rebake_pressed));
	add_child(rebake_button);

	action_menu = memnew(PopupMenu);
	action_menu->connect("index_pressed", callable_mp(this, &DerivedDataSharedBakeRow::_action_selected));
	add_child(action_menu);

	// The manifest already names the owner, and it cost nothing extra to read, so
	// the row can say *whose* bake this is without any further disk work.
	const String owner_scene = p_manifest.get("scene_path", "");
	const String owner_node = p_manifest.get("node_path", "");
	String tooltip = vformat(TTR("Slot: %s"), p_slot);
	if (!owner_scene.is_empty()) {
		tooltip += "\n" + vformat(TTR("Allocated to: %s"), owner_node.is_empty() ? owner_scene : owner_scene + "::" + owner_node);
		tooltip += "\n" + TTR("Names in the manifest are recorded at bake time and may be stale; the identity behind them is not.");
	}
	set_tooltip_text(tooltip);
}

void DerivedDataSharedBakeRow::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_THEME_CHANGED: {
			const Ref<Texture2D> warning_icon = get_editor_theme_icon(SNAME("NodeWarning"));
			icon->set_texture(warning_icon);
			if (warning_icon.is_valid()) {
				icon->set_custom_minimum_size(warning_icon->get_size());
			}
			label->add_theme_color_override(SceneStringName(font_color), get_theme_color(SNAME("warning_color"), EditorStringName(Editor)));
			add_theme_constant_override(SNAME("separation"), 4 * EDSCALE);
		} break;
	}
}

void DerivedDataSharedBakeRow::_invoke(const EditorSceneActionEntry &p_entry) {
	EditorSceneActionRegistry *registry = EditorSceneActionRegistry::get_singleton();
	ERR_FAIL_NULL(registry);
	registry->invoke(p_entry);

	// A successful bake repoints the property, which removes the reason this row
	// exists. Rebuilding the tree frees this control, so it can only happen after
	// the press signal has unwound.
	if (InspectorDock::get_inspector_singleton()) {
		callable_mp(InspectorDock::get_inspector_singleton(), &EditorInspector::update_tree).call_deferred();
	}
}

void DerivedDataSharedBakeRow::_action_selected(int p_index) {
	ERR_FAIL_INDEX(p_index, (int)actions.size());
	_invoke(actions[p_index]);
}

void DerivedDataSharedBakeRow::_clear_inherited(Node *p_node) {
	// Fallback for producers that expose no scene action: dropping the foreign
	// reference is the half of "rebake to own" this surface can honestly do. The
	// allocator mints a fresh bundle whenever the property is empty, so the node's
	// own bake — run however that producer is normally run — completes the repair.
	const Variant current = p_node->get(property);
	const Variant cleared = current.get_type() == Variant::STRING ? Variant(String()) : Variant();

	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	ERR_FAIL_NULL(undo_redo);
	undo_redo->create_action(TTR("Drop Inherited Bake"));
	undo_redo->add_do_property(p_node, property, cleared);
	undo_redo->add_undo_property(p_node, property, current);
	undo_redo->commit_action();

	EditorNode::get_singleton()->show_warning(TTR("This node exposes no bake action to the editor, so only the inherited reference could be dropped. Run this node's own bake now: with the property empty it will allocate a bundle of its own."));
}

void DerivedDataSharedBakeRow::_rebake_pressed() {
	Node *node = Object::cast_to<Node>(ObjectDB::get_instance(node_id));
	ERR_FAIL_NULL_MSG(node, "The node this bake warning belongs to no longer exists.");

	// The Scene Actions registry is the only mechanism that reaches both producer
	// families: the four C++ baker plugins register a class action, and GDScript
	// producers opt their bake tool button in. Collect for the whole scene and
	// filter, because per-node collection is not part of the registry's API.
	actions.clear();
	EditorSceneActionRegistry *registry = EditorSceneActionRegistry::get_singleton();
	Node *scene_root = EditorNode::get_singleton()->get_edited_scene();
	if (registry && scene_root && (node == scene_root || scene_root->is_ancestor_of(node))) {
		LocalVector<EditorSceneActionEntry> collected;
		registry->collect(scene_root, collected);
		for (const EditorSceneActionEntry &entry : collected) {
			if (entry.node_id == node_id && entry.enabled) {
				actions.push_back(entry);
			}
		}
	}

	if (actions.is_empty()) {
		_clear_inherited(node);
		return;
	}
	if (actions.size() == 1) {
		_invoke(actions[0]);
		return;
	}

	// A node can own several slots (the smoke volume bakes a field and an irradiance
	// grid), and nothing in the manifest says which action feeds which slot. Guessing
	// would rebake the wrong one silently, so ask.
	action_menu->clear();
	for (uint32_t i = 0; i < actions.size(); i++) {
		const EditorSceneActionEntry &entry = actions[i];
		if (!entry.icon_name.is_empty() && has_theme_icon(entry.icon_name, EditorStringName(EditorIcons))) {
			action_menu->add_icon_item(get_editor_theme_icon(entry.icon_name), entry.label, (int)i);
		} else {
			action_menu->add_item(entry.label, (int)i);
		}
	}
	action_menu->reset_size();
	action_menu->set_position(rebake_button->get_screen_position() + Vector2(0, rebake_button->get_size().y));
	action_menu->popup();
}

// Inspector plugin //////////////////////////////////////////////////////////

String DerivedDataInspectorPlugin::_artifact_path(const Variant &p_value) {
	String path;
	switch (p_value.get_type()) {
		case Variant::OBJECT: {
			const Ref<Resource> resource = p_value;
			if (resource.is_null()) {
				return String();
			}
			path = resource->get_path();
		} break;
		case Variant::STRING:
		case Variant::STRING_NAME: {
			// Some producers store their artifact as a plain path string rather than a
			// resource reference (the pipe runs do), so strings are candidates too.
			path = p_value;
		} break;
		default: {
			return String();
		}
	}

	if (path.begins_with("uid://")) {
		// Not ResourceUID::ensure_path(): an unresolvable UID is a normal answer here
		// ("this property does not name a bundle"), not something to ERR_PRINT about.
		ResourceUID *uid = ResourceUID::get_singleton();
		const ResourceUID::ID id = uid->text_to_id(path);
		if (id == ResourceUID::INVALID_ID || !uid->has_id(id)) {
			return String();
		}
		path = uid->get_id_path(id);
	}
	// Built-in sub-resources ("res://scene.tscn::1") can never live in a bundle.
	if (!path.begins_with("res://") || path.contains("::")) {
		return String();
	}
	return path;
}

bool DerivedDataInspectorPlugin::_identity_settled(Node *p_node) {
	// owns() resolves the node's persistent identity, and the allocator reports a
	// missing one as a loud error. That is right for an explicit bake and wrong for
	// an inspector refresh, which would spam it on every redraw of a freshly
	// duplicated node — so check the same preconditions quietly and stay silent.
	// The row reappears by itself once the scene is saved and the IDs settle.
	if (p_node->get_unique_scene_id_path().is_empty()) {
		return false;
	}
	Node *root = p_node;
	while (root->get_owner() != nullptr) {
		root = root->get_owner();
	}
	return !root->get_scene_file_path().is_empty();
}

bool DerivedDataInspectorPlugin::can_handle(Object *p_object) {
	Node *node = Object::cast_to<Node>(p_object);
	if (!EditorDerivedData::get_singleton() || !node) {
		return false;
	}
	// A project with no slot registry has no bundles, and asking the allocator
	// anyway would make it complain about the unset setting once per property.
	if (String(GLOBAL_GET("editor/derived_data/slot_registry")).is_empty()) {
		return false;
	}
	// Per-node, so it belongs on the once-per-object gate rather than in parse_property,
	// where every candidate property would pay a manifest read only to discard it.
	return _identity_settled(node);
}

bool DerivedDataInspectorPlugin::parse_property(Object *p_object, const Variant::Type p_type, const String &p_path, const PropertyHint p_hint, const String &p_hint_text, const BitField<PropertyUsageFlags> p_usage, const bool p_wide) {
	// Cheapest discriminators first: only object and string properties can name an
	// artifact at all, so nothing else pays for a Variant fetch.
	if (p_type != Variant::OBJECT && p_type != Variant::STRING && p_type != Variant::STRING_NAME) {
		return false;
	}
	Node *node = Object::cast_to<Node>(p_object);
	EditorDerivedData *edd = EditorDerivedData::get_singleton();
	if (!node || !edd) {
		return false;
	}

	const String artifact_path = _artifact_path(node->get(p_path));
	if (artifact_path.is_empty()) {
		return false;
	}

	// One manifest read decides everything: outside a bundle there is nothing to
	// warn about, and inside one the slot is what ownership is asked against.
	const Dictionary manifest = edd->describe(artifact_path);
	if (manifest.is_empty()) {
		return false;
	}
	const String slot = manifest.get("slot", "");
	if (slot.is_empty()) {
		return false;
	}
	if (edd->owns(node, StringName(slot), artifact_path)) {
		return false;
	}

	// add_to_end places the row after the property's own editor; a warning that
	// precedes the value it is about reads as a warning about the section.
	add_property_editor(p_path, memnew(DerivedDataSharedBakeRow(node, p_path, slot, manifest)), true);
	return false; // Not exclusive: the property keeps its normal editor.
}
