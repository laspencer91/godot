/**************************************************************************/
/*  box3d_physics_settings_tab.cpp                                        */
/**************************************************************************/

#include "box3d_physics_settings_tab.h"

#include "core/io/resource_saver.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "editor/editor_node.h"
#include "editor/editor_string_names.h"
#include "editor/editor_undo_redo_manager.h"
#include "editor/inspector/editor_inspector.h"
#include "editor/themes/editor_scale.h"
#include "scene/gui/button.h"
#include "scene/gui/dialogs.h"
#include "scene/gui/label.h"
#include "scene/gui/margin_container.h"
#include "scene/gui/separator.h"
#include "scene/gui/split_container.h"
#include "scene/gui/tree.h"
#include "scene/main/timer.h"
#include "scene/scene_string_names.h"

// The inspector edits a material through this proxy rather than the resource
// directly: the name is renamed in the material tree so collisions can be rejected
// before they happen, and the id is an engine-assigned handle baked into shapes and
// recordings, so neither belongs in an inspector field. Both are shown by the tree.
class Box3DSurfaceMaterialProxy : public Object {
	GDCLASS(Box3DSurfaceMaterialProxy, Object);

	Ref<Box3DSurfaceMaterial> material;

protected:
	bool _set(const StringName &p_name, const Variant &p_value) {
		if (material.is_null() || p_name == SNAME("material_name") || p_name == SNAME("material_id")) {
			return false;
		}
		bool valid = false;
		material->set(p_name, p_value, &valid);
		return valid;
	}

	bool _get(const StringName &p_name, Variant &r_ret) const {
		if (material.is_null()) {
			return false;
		}
		bool valid = false;
		r_ret = material->get(p_name, &valid);
		return valid;
	}

	void _get_property_list(List<PropertyInfo> *p_list) const {
		if (material.is_null()) {
			return;
		}
		List<PropertyInfo> class_properties;
		ClassDB::get_property_list(Box3DSurfaceMaterial::get_class_static(), &class_properties, true);
		for (const PropertyInfo &property : class_properties) {
			if (property.name == SNAME("material_name") || property.name == SNAME("material_id")) {
				continue;
			}
			p_list->push_back(property);
		}
	}

public:
	void set_material(const Ref<Box3DSurfaceMaterial> &p_material) { material = p_material; }
	Ref<Box3DSurfaceMaterial> get_material() const { return material; }
};

void Box3DPhysicsSettingsTab::_bind_methods() {
	// Reached through EditorUndoRedoManager, which dispatches by method name.
	ClassDB::bind_method(D_METHOD("_materials_committed", "select"), &Box3DPhysicsSettingsTab::_materials_committed);
}

Ref<Box3DSurfaceMaterialLibrary> Box3DPhysicsSettingsTab::_ensure_library() {
	Box3DPhysics *physics = Box3DPhysics::get_singleton();
	ERR_FAIL_NULL_V(physics, Ref<Box3DSurfaceMaterialLibrary>());

	const String path = Box3DPhysics::get_surface_material_library_path();
	ERR_FAIL_COND_V_MSG(path.is_empty(), Ref<Box3DSurfaceMaterialLibrary>(), "Box3D: 'physics/box3d/surface_material_library' is empty, so there is nowhere to store surface materials.");

	if (library.is_null()) {
		library = physics->get_surface_material_library();
	}
	if (library.is_null()) {
		library.instantiate();
		// Claim the path in the resource cache before the first save so that the reload
		// following every mutation resolves back to this instance instead of loading a
		// second copy from disk and stranding the undo history on the old one.
		library->set_path(path, true);
	}
	return library;
}

TypedArray<Box3DSurfaceMaterial> Box3DPhysicsSettingsTab::_current_materials() const {
	if (library.is_null()) {
		return TypedArray<Box3DSurfaceMaterial>();
	}
	return library->get_materials().duplicate();
}

void Box3DPhysicsSettingsTab::_commit(const String &p_action_name, const TypedArray<Box3DSurfaceMaterial> &p_materials, const StringName &p_select) {
	ERR_FAIL_COND(library.is_null());
	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	ERR_FAIL_NULL(undo_redo);

	undo_redo->create_action(p_action_name);
	undo_redo->add_do_method(library.ptr(), "set_materials", p_materials);
	undo_redo->add_undo_method(library.ptr(), "set_materials", _current_materials());
	undo_redo->add_do_method(this, "_materials_committed", p_select);
	undo_redo->add_undo_method(this, "_materials_committed", selected_name);
	undo_redo->commit_action();
}

void Box3DPhysicsSettingsTab::_materials_committed(const StringName &p_select) {
	selected_name = p_select;
	// This save supersedes anything the debounce was still holding.
	save_timer->stop();
	_save_library();
	_refresh();
}

void Box3DPhysicsSettingsTab::_save_library() {
	Box3DPhysics *physics = Box3DPhysics::get_singleton();
	if (physics == nullptr || library.is_null()) {
		return;
	}
	const String path = Box3DPhysics::get_surface_material_library_path();
	ERR_FAIL_COND_MSG(path.is_empty(), "Box3D: 'physics/box3d/surface_material_library' is empty, so surface materials cannot be saved.");

	committing = true;
	// ResourceSaver's save callback is what tells EditorFileSystem about the new file,
	// so nothing here has to poke the FileSystem dock.
	const Error err = ResourceSaver::save(library, path);
	if (err == OK) {
		// Rebuilding the registry is what refreshes material dropdowns elsewhere in
		// the editor, which listen for `surface_materials_changed`.
		physics->reload_surface_material_library();
		const Ref<Box3DSurfaceMaterialLibrary> reloaded = physics->get_surface_material_library();
		if (reloaded.is_valid()) {
			library = reloaded;
		}
	}
	committing = false;
	ERR_FAIL_COND_MSG(err != OK, vformat("Box3D: could not save the surface material library to \"%s\" (error %d).", path, err));
}

void Box3DPhysicsSettingsTab::_refresh() {
	Box3DPhysics *physics = Box3DPhysics::get_singleton();
	if (physics) {
		const Ref<Box3DSurfaceMaterialLibrary> current = physics->get_surface_material_library();
		// A library created here but not saved yet is not known to the registry, so
		// only adopt the registry's copy when it actually has one.
		if (current.is_valid()) {
			library = current;
		}
	}

	updating = true;
	material_tree->clear();
	TreeItem *root = material_tree->create_item();
	TreeItem *select_item = nullptr;
	TreeItem *first_item = nullptr;
	int item_count = 0;

	const Color id_color = get_theme_color(SNAME("font_disabled_color"), EditorStringName(Editor));
	if (library.is_valid()) {
		const TypedArray<Box3DSurfaceMaterial> materials = library->get_materials();
		for (int i = 0; i < materials.size(); i++) {
			const Ref<Box3DSurfaceMaterial> surface_material = materials[i];
			if (surface_material.is_null()) {
				continue;
			}
			const StringName name = surface_material->get_material_name();

			TreeItem *item = material_tree->create_item(root);
			item->set_text(0, String(name));
			item->set_editable(0, true);
			item->set_metadata(0, name);
			item->set_tooltip_text(0, TTR("Double-click to rename."));

			item->set_text(1, itos(surface_material->get_material_id()));
			item->set_text_alignment(1, HORIZONTAL_ALIGNMENT_RIGHT);
			item->set_custom_color(1, id_color);
			item->set_selectable(1, false);
			item->set_tooltip_text(1, TTR("Engine-assigned material id, baked into shapes and recordings."));

			item_count++;
			if (first_item == nullptr) {
				first_item = item;
			}
			if (name == selected_name) {
				select_item = item;
			}
		}
	}

	if (select_item == nullptr) {
		select_item = first_item;
	}
	if (select_item == nullptr) {
		selected_name = StringName();
	} else {
		select_item->select(0);
		selected_name = select_item->get_metadata(0);
	}
	updating = false;

	split->set_visible(item_count > 0);
	empty_state->set_visible(item_count == 0);

	const bool has_selection = selected_name != StringName();
	duplicate_button->set_disabled(!has_selection);
	delete_button->set_disabled(!has_selection);

	_update_inspector();
}

void Box3DPhysicsSettingsTab::_update_inspector() {
	Ref<Box3DSurfaceMaterial> surface_material;
	if (library.is_valid()) {
		surface_material = library->find_material(selected_name);
	}
	if (proxy->get_material() == surface_material) {
		return;
	}
	// The proxy object never changes, so the inspector has to be cleared to notice
	// that it is now describing a different material.
	inspector->edit(nullptr);
	proxy->set_material(surface_material);
	if (surface_material.is_valid()) {
		inspector->edit(proxy);
	}
}

void Box3DPhysicsSettingsTab::_item_selected() {
	// Selecting an item while repopulating is this tab's own doing, and _refresh()
	// finishes the job itself.
	if (updating) {
		return;
	}
	TreeItem *item = material_tree->get_selected();
	if (item == nullptr) {
		return;
	}
	_flush_pending_save();
	selected_name = item->get_metadata(0);
	duplicate_button->set_disabled(false);
	delete_button->set_disabled(false);
	_update_inspector();
}

void Box3DPhysicsSettingsTab::_item_renamed() {
	if (updating || library.is_null()) {
		return;
	}
	TreeItem *item = material_tree->get_edited();
	if (item == nullptr || material_tree->get_edited_column() != 0) {
		return;
	}

	const StringName old_name = item->get_metadata(0);
	const String new_name = item->get_text(0).strip_edges();
	if (StringName(new_name) == old_name) {
		// Still reset the cell, in case only surrounding whitespace was typed.
		updating = true;
		item->set_text(0, String(old_name));
		updating = false;
		return;
	}

	if (new_name.is_empty()) {
		updating = true;
		item->set_text(0, String(old_name));
		updating = false;
		EditorNode::get_singleton()->show_warning(TTR("The name of a surface material cannot be empty."));
		return;
	}
	if (library->find_material_index(new_name) != -1) {
		updating = true;
		item->set_text(0, String(old_name));
		updating = false;
		EditorNode::get_singleton()->show_warning(vformat(TTR("A surface material named \"%s\" already exists."), new_name));
		return;
	}

	const Ref<Box3DSurfaceMaterial> surface_material = library->find_material(old_name);
	if (surface_material.is_null()) {
		return;
	}

	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	ERR_FAIL_NULL(undo_redo);
	undo_redo->create_action(TTR("Rename Surface Material"));
	undo_redo->add_do_property(surface_material.ptr(), "material_name", StringName(new_name));
	undo_redo->add_undo_property(surface_material.ptr(), "material_name", old_name);
	undo_redo->add_do_method(this, "_materials_committed", StringName(new_name));
	undo_redo->add_undo_method(this, "_materials_committed", old_name);
	undo_redo->commit_action();
}

void Box3DPhysicsSettingsTab::_append_material(const Ref<Box3DSurfaceMaterial> &p_source, const StringName &p_name_base, const String &p_action_name) {
	const Ref<Box3DSurfaceMaterialLibrary> lib = _ensure_library();
	if (lib.is_null()) {
		return;
	}

	Ref<Box3DSurfaceMaterial> surface_material;
	if (p_source.is_valid()) {
		surface_material = p_source->duplicate();
	} else {
		surface_material.instantiate();
	}
	surface_material->set_material_name(lib->make_unique_name(p_name_base));
	surface_material->set_material_id(lib->allocate_material_id());

	TypedArray<Box3DSurfaceMaterial> materials = _current_materials();
	materials.push_back(surface_material);
	_commit(p_action_name, materials, surface_material->get_material_name());
}

void Box3DPhysicsSettingsTab::_add_pressed() {
	_append_material(Ref<Box3DSurfaceMaterial>(), "New Material", TTR("Add Surface Material"));
}

void Box3DPhysicsSettingsTab::_duplicate_pressed() {
	const Ref<Box3DSurfaceMaterialLibrary> lib = _ensure_library();
	if (lib.is_null()) {
		return;
	}
	const Ref<Box3DSurfaceMaterial> source = lib->find_material(selected_name);
	if (source.is_null()) {
		return;
	}
	_append_material(source, selected_name, TTR("Duplicate Surface Material"));
}

void Box3DPhysicsSettingsTab::_delete_pressed() {
	if (selected_name == StringName()) {
		return;
	}
	delete_dialog->set_text(vformat(TTR("Remove the surface material \"%s\"?\n\nShapes and Box3DSurfaceOverride3D nodes that still reference this name will fall back to the default material."), String(selected_name)));
	delete_dialog->popup_centered(Size2(460, 0) * EDSCALE);
}

void Box3DPhysicsSettingsTab::_delete_confirmed() {
	if (library.is_null()) {
		return;
	}
	const int index = library->find_material_index(selected_name);
	if (index == -1) {
		return;
	}

	TypedArray<Box3DSurfaceMaterial> materials = _current_materials();
	materials.remove_at(index);

	StringName next_selection;
	if (materials.size() > 0) {
		const Ref<Box3DSurfaceMaterial> neighbor = materials[MIN(index, (int)materials.size() - 1)];
		if (neighbor.is_valid()) {
			next_selection = neighbor->get_material_name();
		}
	}
	_commit(TTR("Remove Surface Material"), materials, next_selection);
}

void Box3DPhysicsSettingsTab::_inspector_property_edited(const String &p_property) {
	// Fired from both sides of the inspector's own undo action, so this covers
	// redo and undo of a field edit as well as the edit itself. Dragging a slider
	// emits one of these per step, hence the debounce rather than a direct save.
	save_timer->start();
}

void Box3DPhysicsSettingsTab::_flush_pending_save() {
	if (save_timer->is_stopped()) {
		return;
	}
	save_timer->stop();
	_save_library();
}

void Box3DPhysicsSettingsTab::_surface_materials_changed() {
	if (committing) {
		return;
	}
	save_timer->stop();
	library.unref();
	_refresh();
}

void Box3DPhysicsSettingsTab::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			Box3DPhysics *physics = Box3DPhysics::get_singleton();
			if (physics) {
				physics->connect(SNAME("surface_materials_changed"), callable_mp(this, &Box3DPhysicsSettingsTab::_surface_materials_changed));
			}
			_refresh();
		} break;

		case NOTIFICATION_EXIT_TREE: {
			_flush_pending_save();
			Box3DPhysics *physics = Box3DPhysics::get_singleton();
			if (physics) {
				physics->disconnect(SNAME("surface_materials_changed"), callable_mp(this, &Box3DPhysicsSettingsTab::_surface_materials_changed));
			}
		} break;

		case NOTIFICATION_VISIBILITY_CHANGED: {
			if (is_visible_in_tree()) {
				_refresh();
			} else {
				// Closing the Project Settings dialog must not drop an edit that the
				// debounce had not written out yet.
				_flush_pending_save();
			}
		} break;

		case NOTIFICATION_THEME_CHANGED: {
			add_button->set_button_icon(get_editor_theme_icon(SNAME("Add")));
			empty_add_button->set_button_icon(get_editor_theme_icon(SNAME("Add")));
			duplicate_button->set_button_icon(get_editor_theme_icon(SNAME("Duplicate")));
			delete_button->set_button_icon(get_editor_theme_icon(SNAME("Remove")));

			// The id column carries a theme color, so it has to be recolored in place.
			const Color id_color = get_theme_color(SNAME("font_disabled_color"), EditorStringName(Editor));
			TreeItem *root = material_tree->get_root();
			for (TreeItem *item = root ? root->get_first_child() : nullptr; item; item = item->get_next()) {
				item->set_custom_color(1, id_color);
			}
		} break;

		case NOTIFICATION_PREDELETE: {
			_flush_pending_save();
			inspector->edit(nullptr);
		} break;
	}
}

Box3DPhysicsSettingsTab::Box3DPhysicsSettingsTab() {
	set_name(TTRC("Physics"));

	HBoxContainer *toolbar = memnew(HBoxContainer);
	add_child(toolbar);

	Label *title = memnew(Label(TTRC("Surface Materials")));
	title->set_theme_type_variation("HeaderSmall");
	toolbar->add_child(title);
	toolbar->add_spacer();

	add_button = memnew(Button);
	add_button->set_text(TTRC("Add"));
	add_button->connect(SceneStringName(pressed), callable_mp(this, &Box3DPhysicsSettingsTab::_add_pressed));
	toolbar->add_child(add_button);

	duplicate_button = memnew(Button);
	duplicate_button->set_text(TTRC("Duplicate"));
	duplicate_button->set_disabled(true);
	duplicate_button->connect(SceneStringName(pressed), callable_mp(this, &Box3DPhysicsSettingsTab::_duplicate_pressed));
	toolbar->add_child(duplicate_button);

	delete_button = memnew(Button);
	delete_button->set_text(TTRC("Delete"));
	delete_button->set_disabled(true);
	delete_button->connect(SceneStringName(pressed), callable_mp(this, &Box3DPhysicsSettingsTab::_delete_pressed));
	toolbar->add_child(delete_button);

	split = memnew(HSplitContainer);
	split->set_v_size_flags(SIZE_EXPAND_FILL);
	add_child(split);

	material_tree = memnew(Tree);
	material_tree->set_custom_minimum_size(Size2(220, 0) * EDSCALE);
	material_tree->set_v_size_flags(SIZE_EXPAND_FILL);
	material_tree->set_hide_root(true);
	material_tree->set_hide_folding(true);
	material_tree->set_select_mode(Tree::SELECT_SINGLE);
	material_tree->set_columns(2);
	material_tree->set_column_titles_visible(false);
	material_tree->set_column_expand(0, true);
	material_tree->set_column_clip_content(0, true);
	// The id is a short number, so it gets a fixed lane instead of half the width.
	material_tree->set_column_expand(1, false);
	material_tree->set_column_custom_minimum_width(1, 48 * EDSCALE);
	material_tree->connect("item_selected", callable_mp(this, &Box3DPhysicsSettingsTab::_item_selected));
	material_tree->connect("item_edited", callable_mp(this, &Box3DPhysicsSettingsTab::_item_renamed));
	split->add_child(material_tree);

	MarginContainer *inspector_margin = memnew(MarginContainer);
	inspector_margin->set_theme_type_variation("NoBorderHorizontal");
	inspector_margin->set_h_size_flags(SIZE_EXPAND_FILL);
	split->add_child(inspector_margin);

	inspector = memnew(EditorInspector);
	inspector->set_v_size_flags(SIZE_EXPAND_FILL);
	inspector->set_scroll_hint_mode(ScrollContainer::SCROLL_HINT_MODE_ALL);
	// Pulls the property tooltips out of the XML class reference.
	inspector->set_use_doc_hints(true);
	inspector->set_object_class(Box3DSurfaceMaterial::get_class_static());
	inspector->connect("property_edited", callable_mp(this, &Box3DPhysicsSettingsTab::_inspector_property_edited));
	inspector_margin->add_child(inspector);

	empty_state = memnew(VBoxContainer);
	empty_state->set_alignment(BoxContainer::ALIGNMENT_CENTER);
	empty_state->set_v_size_flags(SIZE_EXPAND_FILL);
	add_child(empty_state);

	Label *empty_title = memnew(Label(TTRC("No surface materials yet.")));
	empty_title->set_theme_type_variation("HeaderMedium");
	empty_title->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);
	empty_state->add_child(empty_title);

	Label *empty_hint = memnew(Label(TTRC("Surface materials name the friction, restitution, rolling resistance and conveyor velocity that Box3D applies where two shapes touch. Assign them per shape or per triangle, and tune them here for the whole project.")));
	empty_hint->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);
	empty_hint->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	empty_hint->set_custom_minimum_size(Size2(460, 0) * EDSCALE);
	empty_hint->set_h_size_flags(SIZE_SHRINK_CENTER);
	empty_state->add_child(empty_hint);

	empty_state->add_child(memnew(HSeparator));

	empty_add_button = memnew(Button);
	empty_add_button->set_text(TTRC("Add Surface Material"));
	empty_add_button->set_h_size_flags(SIZE_SHRINK_CENTER);
	empty_add_button->connect(SceneStringName(pressed), callable_mp(this, &Box3DPhysicsSettingsTab::_add_pressed));
	empty_state->add_child(empty_add_button);

	delete_dialog = memnew(ConfirmationDialog);
	delete_dialog->set_title(TTRC("Remove Surface Material"));
	delete_dialog->set_ok_button_text(TTRC("Remove"));
	delete_dialog->connect(SceneStringName(confirmed), callable_mp(this, &Box3DPhysicsSettingsTab::_delete_confirmed));
	add_child(delete_dialog);

	save_timer = memnew(Timer);
	save_timer->set_wait_time(0.5);
	save_timer->set_one_shot(true);
	save_timer->connect("timeout", callable_mp(this, &Box3DPhysicsSettingsTab::_save_library));
	add_child(save_timer);

	proxy = memnew(Box3DSurfaceMaterialProxy);
}

Box3DPhysicsSettingsTab::~Box3DPhysicsSettingsTab() {
	memdelete(proxy);
}
