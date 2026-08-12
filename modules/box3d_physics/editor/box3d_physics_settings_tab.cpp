/**************************************************************************/
/*  box3d_physics_settings_tab.cpp                                        */
/**************************************************************************/

#include "box3d_physics_settings_tab.h"

#include "core/config/project_settings.h"
#include "core/io/resource_loader.h"
#include "core/io/resource_saver.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/object/script_language.h"
#include "editor/editor_node.h"
#include "editor/doc/editor_help.h"
#include "editor/editor_string_names.h"
#include "editor/file_system/editor_file_system.h"
#include "editor/editor_undo_redo_manager.h"
#include "editor/gui/create_dialog.h"
#include "editor/inspector/editor_inspector.h"
#include "editor/themes/editor_scale.h"
#include "scene/gui/button.h"
#include "scene/gui/dialogs.h"
#include "scene/gui/label.h"
#include "scene/gui/margin_container.h"
#include "scene/gui/separator.h"
#include "scene/gui/split_container.h"
#include "scene/gui/tree.h"
#include "scene/property_utils.h"
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
		if (!valid) {
			// Not one of the material's own properties, so it came from the flattened
			// gameplay data below.
			const Ref<Box3DSurfaceGameplayData> gameplay = material->get_gameplay();
			if (gameplay.is_valid()) {
				gameplay->set(p_name, p_value, &valid);
			}
		}
		return valid;
	}

	bool _get(const StringName &p_name, Variant &r_ret) const {
		if (material.is_null()) {
			return false;
		}
		bool valid = false;
		r_ret = material->get(p_name, &valid);
		if (!valid) {
			const Ref<Box3DSurfaceGameplayData> gameplay = material->get_gameplay();
			if (gameplay.is_valid()) {
				r_ret = gameplay->get(p_name, &valid);
			}
		}
		return valid;
	}

	void _get_property_list(List<PropertyInfo> *p_list) const {
		if (material.is_null()) {
			return;
		}
		// Names the doc class for everything below it, so the inspector's tooltips resolve
		// against Box3DSurfaceMaterial rather than against this proxy.
		p_list->push_back(PropertyInfo(Variant::NIL, Box3DSurfaceMaterial::get_class_static(), PROPERTY_HINT_NONE, Box3DSurfaceMaterial::get_class_static(), PROPERTY_USAGE_CATEGORY));

		List<PropertyInfo> class_properties;
		ClassDB::get_property_list(Box3DSurfaceMaterial::get_class_static(), &class_properties, true);
		for (const PropertyInfo &property : class_properties) {
			// `gameplay` is chosen once for the whole project, so a per-material picker would
			// only offer ways to get it wrong. Its fields are flattened in below instead.
			if (property.name == SNAME("material_name") || property.name == SNAME("material_id") || property.name == SNAME("gameplay")) {
				continue;
			}
			p_list->push_back(property);
		}

		const Ref<Box3DSurfaceGameplayData> gameplay = material->get_gameplay();
		const Ref<Script> gameplay_script = gameplay.is_valid() ? gameplay->get_script() : Variant();
		if (gameplay_script.is_null()) {
			return;
		}

		// Sourced from the instance rather than from Script::get_script_property_list(): only
		// the instance route reflects @export_group ordering and, with validate_property()
		// below, whatever the script's own _validate_property() decides. A script that
		// narrows a field to an enum at edit time (say, FMOD surface labels read from the
		// Studio project) has to work here exactly as it does in the ordinary inspector.
		List<PropertyInfo> instance_properties;
		gameplay->get_property_list(&instance_properties);

		List<PropertyInfo> script_properties;
		bool has_fields = false;
		for (PropertyInfo property : instance_properties) {
			// Its own class categories would re-divide a list that is already one section.
			if (property.usage & PROPERTY_USAGE_CATEGORY) {
				continue;
			}
			if (property.usage & (PROPERTY_USAGE_GROUP | PROPERTY_USAGE_SUBGROUP)) {
				script_properties.push_back(property);
				continue;
			}
			// Everything the base type already owns: `script`, and Resource's own path/name
			// bookkeeping. The material published everything native above.
			if (!(property.usage & PROPERTY_USAGE_EDITOR) || property.name == SNAME("script") || ClassDB::has_property(Box3DSurfaceGameplayData::get_class_static(), property.name)) {
				continue;
			}
			gameplay->validate_property(property);
			if (!(property.usage & PROPERTY_USAGE_EDITOR)) {
				// The script hid it from this context.
				continue;
			}
			script_properties.push_back(property);
			has_fields = true;
		}
		if (!has_fields) {
			return;
		}

		// A divider so the project's fields never read as built-in Box3D physics settings, and
		// the doc class for them. hint_string is the script path rather than the class name:
		// the inspector loads it to recover the doc class, which is what makes each field's
		// `##` comment show up as its tooltip.
		p_list->push_back(PropertyInfo(Variant::NIL, get_gameplay_doc_class(gameplay_script), PROPERTY_HINT_NONE, gameplay_script->get_path(), PROPERTY_USAGE_CATEGORY));
		for (const PropertyInfo &property : script_properties) {
			p_list->push_back(property);
		}
	}

public:
	void set_material(const Ref<Box3DSurfaceMaterial> &p_material) { material = p_material; }
	Ref<Box3DSurfaceMaterial> get_material() const { return material; }

	// The doc class the inspector resolves the flattened gameplay tooltips against. Kept here
	// so the category emitted above and the descriptions the tab registers cannot drift apart.
	static String get_gameplay_doc_class(const Ref<Script> &p_script) {
		if (p_script.is_null()) {
			return String();
		}
		const StringName doc_class = p_script->get_doc_class_name();
		if (doc_class != StringName()) {
			return String(doc_class);
		}
		const StringName global_name = p_script->get_global_name();
		if (global_name != StringName()) {
			return String(global_name);
		}
		return p_script->get_path().get_file();
	}
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

void Box3DPhysicsSettingsTab::_commit(const String &p_action_name, const TypedArray<Box3DSurfaceMaterial> &p_materials, int p_select_slot) {
	ERR_FAIL_COND(library.is_null());
	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	ERR_FAIL_NULL(undo_redo);

	undo_redo->create_action(p_action_name);
	undo_redo->add_do_method(library.ptr(), "set_materials", p_materials);
	undo_redo->add_undo_method(library.ptr(), "set_materials", _current_materials());
	undo_redo->add_do_method(this, "_materials_committed", p_select_slot);
	undo_redo->add_undo_method(this, "_materials_committed", selected_slot);
	undo_redo->commit_action();
}

void Box3DPhysicsSettingsTab::_materials_committed(int p_select_slot) {
	selected_slot = CLAMP(p_select_slot, 0, Box3DSurfaceMaterialLibrary::get_material_slot_count() - 1);
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
	ERR_FAIL_COND_MSG(!library->is_slot_layout_valid(), vformat("Box3D: the surface material library has more than %d configured materials and cannot be saved as a fixed-slot library.", Box3DSurfaceMaterialLibrary::get_material_slot_count()));

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
	if (library.is_null()) {
		// Materialize the in-memory 15-slot layout for a new project. The library remains
		// unsaved until the first slot is configured.
		_ensure_library();
	}
	_backfill_gameplay_data();

	updating = true;
	material_tree->clear();
	TreeItem *root = material_tree->create_item();
	TreeItem *select_item = nullptr;

	const Color id_color = get_theme_color(SNAME("font_disabled_color"), EditorStringName(Editor));
	if (library.is_valid()) {
		const TypedArray<Box3DSurfaceMaterial> materials = library->get_materials();
		for (int i = 0; i < MIN(materials.size(), Box3DSurfaceMaterialLibrary::get_material_slot_count()); i++) {
			const Ref<Box3DSurfaceMaterial> surface_material = materials[i];
			if (surface_material.is_null()) {
				continue;
			}
			const StringName name = surface_material->get_material_name();

			TreeItem *item = material_tree->create_item(root);
			item->set_text(0, name == StringName() ? TTR("<Unconfigured>") : String(name));
			item->set_editable(0, true);
			item->set_metadata(0, i);
			item->set_tooltip_text(0, name == StringName() ? TTR("Double-click to configure this slot.") : TTR("Double-click to rename."));

			item->set_text(1, itos(surface_material->get_material_id()));
			item->set_text_alignment(1, HORIZONTAL_ALIGNMENT_RIGHT);
			item->set_custom_color(1, id_color);
			item->set_selectable(1, false);
			item->set_tooltip_text(1, TTR("Engine-assigned material id, baked into shapes and recordings."));

			if (i == selected_slot) {
				select_item = item;
			}
		}
	}

	if (select_item == nullptr) {
		select_item = root->get_first_child();
	}
	if (select_item != nullptr) {
		select_item->select(0);
		selected_slot = select_item->get_metadata(0);
	}
	updating = false;

	split->show();
	empty_state->hide();

	const Ref<Box3DSurfaceMaterial> selected = library.is_valid() ? library->get_material_slot(selected_slot) : Ref<Box3DSurfaceMaterial>();
	const bool configured = selected.is_valid() && selected->get_material_name() != StringName();
	add_button->set_disabled(_find_empty_slot() == -1);
	duplicate_button->set_disabled(!configured || _find_empty_slot() == -1);
	delete_button->set_disabled(!configured);

	_update_inspector();
}

void Box3DPhysicsSettingsTab::_register_gameplay_property_docs() {
	// The gameplay fields are flattened onto a proxy that is neither a Resource nor scripted,
	// so EditorInspector cannot reach their script the way it does for a normally inspected
	// custom resource (editor_inspector.cpp:4950-4962): all it has is the category name, which
	// it looks up in EditorHelp's DocTools with no fallback. Hand it the `##` comments off the
	// script we already hold, the way the TileSet proxy inspectors describe their own fields.
	const Ref<Script> script = _load_gameplay_script();
	if (script.is_null()) {
		return;
	}
	const Vector<DocData::ClassDoc> docs = script->get_documentation();
	if (docs.is_empty()) {
		return;
	}
	// A script's own class doc is the last entry; anything before it is an inner class.
	const DocData::ClassDoc &class_doc = docs[docs.size() - 1];
	const DocData::ClassDoc *registered = EditorHelp::get_doc(class_doc.name);

	// Which name the tooltip is keyed by depends on what the script yields, so register under
	// every candidate rather than betting on one.
	Vector<String> doc_classes;
	doc_classes.push_back(Box3DSurfaceMaterialProxy::get_gameplay_doc_class(script));
	doc_classes.push_back(class_doc.name);
	doc_classes.push_back(Box3DSurfaceMaterialProxy::get_class_static());

	for (const DocData::PropertyDoc &property : class_doc.properties) {
		if (property.description.is_empty()) {
			continue;
		}
		// Descriptions are prepended to whatever the doc system finds on its own, so skip the
		// ones it can already resolve rather than showing them twice.
		bool already_documented = false;
		if (registered != nullptr) {
			for (const DocData::PropertyDoc &known : registered->properties) {
				if (known.name == property.name && !known.description.is_empty()) {
					already_documented = true;
					break;
				}
			}
		}
		if (already_documented) {
			continue;
		}
		for (const String &doc_class : doc_classes) {
			if (!doc_class.is_empty()) {
				inspector->add_custom_property_description(doc_class, property.name, property.description);
			}
		}
	}
}

void Box3DPhysicsSettingsTab::_update_inspector() {
	// Ahead of the early-out, so a script reload refreshes the text even when the selection
	// has not changed.
	_register_gameplay_property_docs();

	Ref<Box3DSurfaceMaterial> surface_material;
	if (library.is_valid()) {
		surface_material = library->get_material_slot(selected_slot);
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
	selected_slot = item->get_metadata(0);
	const Ref<Box3DSurfaceMaterial> selected = library.is_valid() ? library->get_material_slot(selected_slot) : Ref<Box3DSurfaceMaterial>();
	const bool configured = selected.is_valid() && selected->get_material_name() != StringName();
	duplicate_button->set_disabled(!configured || _find_empty_slot() == -1);
	delete_button->set_disabled(!configured);
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

	const int slot = item->get_metadata(0);
	const Ref<Box3DSurfaceMaterial> surface_material = library->get_material_slot(slot);
	if (surface_material.is_null()) {
		return;
	}
	const StringName old_name = surface_material->get_material_name();
	const String new_name = item->get_text(0).strip_edges();
	if (old_name == StringName() && new_name == TTR("<Unconfigured>")) {
		return;
	}
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
	if (old_name == StringName()) {
		_configure_slot(slot, Ref<Box3DSurfaceMaterial>(), StringName(new_name), TTR("Configure Surface Material Slot"));
		return;
	}

	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	ERR_FAIL_NULL(undo_redo);
	undo_redo->create_action(TTR("Rename Surface Material"));
	undo_redo->add_do_property(surface_material.ptr(), "material_name", StringName(new_name));
	undo_redo->add_undo_property(surface_material.ptr(), "material_name", old_name);
	undo_redo->add_do_method(this, "_materials_committed", slot);
	undo_redo->add_undo_method(this, "_materials_committed", slot);
	undo_redo->commit_action();
}

int Box3DPhysicsSettingsTab::_find_empty_slot() const {
	if (library.is_null() || !library->is_slot_layout_valid()) {
		return -1;
	}
	for (int i = 0; i < Box3DSurfaceMaterialLibrary::get_material_slot_count(); i++) {
		const Ref<Box3DSurfaceMaterial> slot_material = library->get_material_slot(i);
		if (slot_material.is_valid() && slot_material->get_material_name() == StringName()) {
			return i;
		}
	}
	return -1;
}

void Box3DPhysicsSettingsTab::_configure_slot(int p_slot, const Ref<Box3DSurfaceMaterial> &p_source, const StringName &p_name_base, const String &p_action_name) {
	const Ref<Box3DSurfaceMaterialLibrary> lib = _ensure_library();
	if (lib.is_null() || p_slot < 0 || p_slot >= Box3DSurfaceMaterialLibrary::get_material_slot_count()) {
		return;
	}

	Ref<Box3DSurfaceMaterial> surface_material;
	if (p_source.is_valid()) {
		surface_material = p_source->duplicate(true);
	} else {
		surface_material.instantiate();
	}
	surface_material->set_material_name(lib->make_unique_name(p_name_base));
	surface_material->set_material_id(p_slot + 1);
	// A duplicate already carries the source's data, so only a fresh material needs one.
	// Without this every new material would start with an empty slot the user has to fill
	// by hand, which is the chore this setting exists to remove.
	if (p_source.is_null() && surface_material->get_gameplay().is_null()) {
		surface_material->set_gameplay(_instantiate_gameplay_data());
	}

	TypedArray<Box3DSurfaceMaterial> materials = _current_materials();
	materials[p_slot] = surface_material;
	_commit(p_action_name, materials, p_slot);
}

void Box3DPhysicsSettingsTab::_add_pressed() {
	_configure_slot(_find_empty_slot(), Ref<Box3DSurfaceMaterial>(), "New Material", TTR("Configure Surface Material Slot"));
}

void Box3DPhysicsSettingsTab::_duplicate_pressed() {
	const Ref<Box3DSurfaceMaterialLibrary> lib = _ensure_library();
	if (lib.is_null()) {
		return;
	}
	const Ref<Box3DSurfaceMaterial> source = lib->get_material_slot(selected_slot);
	if (source.is_null()) {
		return;
	}
	_configure_slot(_find_empty_slot(), source, source->get_material_name(), TTR("Duplicate Surface Material"));
}

void Box3DPhysicsSettingsTab::_delete_pressed() {
	const Ref<Box3DSurfaceMaterial> slot_material = library.is_valid() ? library->get_material_slot(selected_slot) : Ref<Box3DSurfaceMaterial>();
	if (slot_material.is_null() || slot_material->get_material_name() == StringName()) {
		return;
	}
	delete_dialog->set_text(vformat(TTR("Clear surface material slot %d (\"%s\")?\n\nThe slot and its numeric id remain reserved. Shapes and Box3DSurfaceOverride3D nodes that reference it will fall back to the default material."), selected_slot + 1, String(slot_material->get_material_name())));
	delete_dialog->popup_centered(Size2(460, 0) * EDSCALE);
}

void Box3DPhysicsSettingsTab::_delete_confirmed() {
	if (library.is_null()) {
		return;
	}
	if (selected_slot < 0 || selected_slot >= Box3DSurfaceMaterialLibrary::get_material_slot_count()) {
		return;
	}

	TypedArray<Box3DSurfaceMaterial> materials = _current_materials();
	Ref<Box3DSurfaceMaterial> empty;
	empty.instantiate();
	empty->set_material_id(selected_slot + 1);
	materials[selected_slot] = empty;
	_commit(TTR("Clear Surface Material Slot"), materials, selected_slot);
}

void Box3DPhysicsSettingsTab::_inspector_property_edited(const String &p_property) {
	// Fired from both sides of the inspector's own undo action, so this covers
	// redo and undo of a field edit as well as the edit itself. Dragging a slider
	// emits one of these per step, hence the debounce rather than a direct save.
	save_timer->start();
}

Ref<Script> Box3DPhysicsSettingsTab::_load_gameplay_script() {
	const String setting = Box3DPhysics::get_surface_gameplay_data_class();
	if (setting.is_empty()) {
		return Ref<Script>();
	}
	// A class name is the friendlier form and the one the picker writes, but requiring
	// `class_name` to use the feature at all would be a trap: a plain `extends
	// Box3DSurfaceGameplayData` script is perfectly valid and can only be named by path.
	const String path = ScriptServer::is_global_class(setting) ? ScriptServer::get_global_class_path(setting) : setting;
	if (path.is_empty() || !ResourceLoader::exists(path)) {
		return Ref<Script>();
	}
	return ResourceLoader::load(path, "Script");
}

Ref<Box3DSurfaceGameplayData> Box3DPhysicsSettingsTab::_instantiate_gameplay_data() const {
	// Deliberately not can_instantiate(): the editor disables scripting, so that is false for
	// every script without @tool. The object is a native Resource carrying the script, which
	// is how EditorData::script_class_instance() and every custom resource already work.
	const Ref<Script> script = _load_gameplay_script();
	if (script.is_null()) {
		return Ref<Box3DSurfaceGameplayData>();
	}
	if (!ClassDB::is_parent_class(script->get_instance_base_type(), Box3DSurfaceGameplayData::get_class_static())) {
		WARN_PRINT(vformat("Box3D: '%s' does not extend Box3DSurfaceGameplayData; new materials will have no gameplay data.", Box3DPhysics::get_surface_gameplay_data_class()));
		return Ref<Box3DSurfaceGameplayData>();
	}

	Variant instance = ClassDB::instantiate(script->get_instance_base_type());
	Object *object = instance;
	if (object == nullptr) {
		return Ref<Box3DSurfaceGameplayData>();
	}
	// Stamps the custom-type script that the inspector reads to label the resource, which a
	// bare set_script would miss. Mirrors EditorData::script_class_instance(), which cannot
	// be used directly because it only accepts global class names.
	PropertyUtils::assign_custom_type_script(object, script);
	object->set_script(script);
	return Ref<Box3DSurfaceGameplayData>(instance);
}

void Box3DPhysicsSettingsTab::_backfill_gameplay_data() {
	// The gameplay fields are flattened into the inspector, so there is no picker a user
	// could reach for: a material with no data would simply show nothing and offer no way
	// to fix it. Materials that predate the setting, or predate this project choosing a
	// class, converge here instead. Not an undo action — it establishes the invariant the
	// setting already implies rather than making a decision of its own.
	if (library.is_null()) {
		return;
	}
	const TypedArray<Box3DSurfaceMaterial> materials = library->get_materials();
	bool changed = false;
	for (int i = 0; i < materials.size(); i++) {
		const Ref<Box3DSurfaceMaterial> surface_material = materials[i];
		if (surface_material.is_null() || surface_material->get_material_name() == StringName() || surface_material->get_gameplay().is_valid()) {
			continue;
		}
		const Ref<Box3DSurfaceGameplayData> gameplay_data = _instantiate_gameplay_data();
		if (gameplay_data.is_null()) {
			// No class configured, or it no longer resolves. Leave the materials alone.
			return;
		}
		surface_material->set_gameplay(gameplay_data);
		changed = true;
	}
	if (changed) {
		save_timer->start();
	}
}

void Box3DPhysicsSettingsTab::_update_gameplay_class_button() {
	const String gameplay_class = Box3DPhysics::get_surface_gameplay_data_class();
	gameplay_class_clear_button->set_visible(!gameplay_class.is_empty());

	if (gameplay_class.is_empty()) {
		gameplay_class_button->set_text(TTR("<none>"));
		gameplay_class_button->remove_theme_color_override(SNAME("font_color"));
		gameplay_class_button->set_tooltip_text(TTR("Pick a Box3DSurfaceGameplayData subclass to attach project-specific data to every surface material."));
		return;
	}

	const bool is_global = ScriptServer::is_global_class(gameplay_class);
	// A path is long and mostly redundant in a narrow field, so show just the file name and
	// keep the full value in the tooltip.
	gameplay_class_button->set_text(is_global ? gameplay_class : gameplay_class.get_file());

	const Ref<Script> script = _load_gameplay_script();
	if (script.is_null()) {
		// Deleting the script, or dropping the class_name the setting refers to, leaves this
		// dangling. Show that rather than silently behaving as if unset.
		gameplay_class_button->add_theme_color_override(SNAME("font_color"), get_theme_color(SNAME("error_color"), EditorStringName(Editor)));
		gameplay_class_button->set_tooltip_text(vformat(TTR("'%s' could not be loaded. The script may have been deleted or moved, or lost its class_name."), gameplay_class));
		return;
	}
	if (!ClassDB::is_parent_class(script->get_instance_base_type(), Box3DSurfaceGameplayData::get_class_static())) {
		gameplay_class_button->add_theme_color_override(SNAME("font_color"), get_theme_color(SNAME("error_color"), EditorStringName(Editor)));
		gameplay_class_button->set_tooltip_text(vformat(TTR("'%s' does not extend Box3DSurfaceGameplayData."), gameplay_class));
		return;
	}

	gameplay_class_button->remove_theme_color_override(SNAME("font_color"));
	if (is_global) {
		gameplay_class_button->set_tooltip_text(vformat(TTR("New materials get a %s instance, and every material's Gameplay slot is limited to it."), gameplay_class));
	} else {
		// Hint narrowing needs a class name: PROPERTY_HINT_RESOURCE_TYPE resolves global
		// classes only, and nothing maps a script path back to one.
		gameplay_class_button->set_tooltip_text(vformat(TTR("New materials get an instance of %s.\nAdd a class_name to this script to also limit each material's Gameplay slot to it."), gameplay_class));
	}
}

void Box3DPhysicsSettingsTab::_gameplay_class_pressed() {
	gameplay_class_dialog->popup_create(false, true, Box3DPhysics::get_surface_gameplay_data_class());
}

void Box3DPhysicsSettingsTab::_gameplay_class_picked() {
	_set_gameplay_class(gameplay_class_dialog->get_selected_type());
}

void Box3DPhysicsSettingsTab::_gameplay_class_cleared() {
	// CreateDialog has no "none" entry, so without this the only way back to unset would be
	// hand-editing project.godot.
	_set_gameplay_class(String());
}

void Box3DPhysicsSettingsTab::_set_gameplay_class(const String &p_class) {
	ProjectSettings::get_singleton()->set_setting("physics/box3d/surface_gameplay_data", p_class);
	ProjectSettings::get_singleton()->save();
	_update_gameplay_class_button();
	_backfill_gameplay_data();

	// The Gameplay hint is resolved while the property list is built, so the inspector has
	// to be rebuilt from scratch to pick up the new class. Clearing the proxy makes
	// _update_inspector() see a change instead of taking its early-out.
	proxy->set_material(Ref<Box3DSurfaceMaterial>());
	inspector->edit(nullptr);
	_update_inspector();
}

void Box3DPhysicsSettingsTab::_undo_redo_inspector_callback(Object *p_undo_redo, Object *p_modified_object, const String &p_property, const Variant &p_value) {
	// `gameplay` is edited through a nested inspector that EditorPropertyResource points
	// straight at the sub-resource, so those edits never reach this tab's own
	// `property_edited` connection and would be lost on save. This hook runs inside
	// EditorInspector::_edit_set for every inspector, nested ones included.
	const Ref<Box3DSurfaceMaterial> surface_material = proxy->get_material();
	if (surface_material.is_null()) {
		return;
	}
	// Only one level deep: a gameplay resource that itself nests further resources would
	// need those tracked too, which is not worth the bookkeeping until someone wants it.
	const Ref<Resource> gameplay = surface_material->get_gameplay();
	if (gameplay.is_null() || p_modified_object != gameplay.ptr()) {
		return;
	}
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

void Box3DPhysicsSettingsTab::_scripts_reloaded() {
	// Adding an @export to the gameplay script changes which rows this tab should show, and
	// the flattened list is built once per edit() call, so it has to be rebuilt by hand.
	if (!is_visible_in_tree()) {
		return;
	}
	_update_gameplay_class_button();
	_backfill_gameplay_data();
	proxy->set_material(Ref<Box3DSurfaceMaterial>());
	inspector->edit(nullptr);
	_update_inspector();
}

void Box3DPhysicsSettingsTab::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			Box3DPhysics *physics = Box3DPhysics::get_singleton();
			if (physics) {
				physics->connect(SNAME("surface_materials_changed"), callable_mp(this, &Box3DPhysicsSettingsTab::_surface_materials_changed));
			}
			EditorNode::get_editor_data().add_undo_redo_inspector_hook_callback(callable_mp(this, &Box3DPhysicsSettingsTab::_undo_redo_inspector_callback));
			EditorFileSystem::get_singleton()->connect("filesystem_changed", callable_mp(this, &Box3DPhysicsSettingsTab::_scripts_reloaded));
			_update_gameplay_class_button();
			_refresh();
		} break;

		case NOTIFICATION_EXIT_TREE: {
			_flush_pending_save();
			Box3DPhysics *physics = Box3DPhysics::get_singleton();
			if (physics) {
				physics->disconnect(SNAME("surface_materials_changed"), callable_mp(this, &Box3DPhysicsSettingsTab::_surface_materials_changed));
			}
			EditorNode::get_editor_data().remove_undo_redo_inspector_hook_callback(callable_mp(this, &Box3DPhysicsSettingsTab::_undo_redo_inspector_callback));
			EditorFileSystem::get_singleton()->disconnect("filesystem_changed", callable_mp(this, &Box3DPhysicsSettingsTab::_scripts_reloaded));
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
			gameplay_class_clear_button->set_button_icon(get_editor_theme_icon(SNAME("Clear")));

			// Inline rename lays a LineEdit over the cell, and it insets its text by its own
			// stylebox margin rather than by the tree's inner item margin, so the name visibly
			// jumps when the editor opens. Match the cell to the editor. The gap differs by
			// theme and spacing preset, hence reading it instead of hardcoding.
			const Ref<StyleBox> rename_style = get_theme_stylebox(SNAME("normal"), SNAME("TreeLineEdit"));
			if (rename_style.is_valid()) {
				material_tree->add_theme_constant_override(SNAME("inner_item_margin_left"), Math::round(rename_style->get_margin(SIDE_LEFT)));
				material_tree->add_theme_constant_override(SNAME("inner_item_margin_right"), Math::round(rename_style->get_margin(SIDE_RIGHT)));
			}
			// Carries the error color when the class is missing, so it has to be recolored.
			_update_gameplay_class_button();

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

	// Above the Surface Materials toolbar: it configures the whole tab, not the list.
	HBoxContainer *gameplay_class_row = memnew(HBoxContainer);
	add_child(gameplay_class_row);
	// Right-aligned and only as wide as it needs to be: stretched across the full row it
	// reads like a heading for the material list below it rather than a single setting.
	gameplay_class_row->add_spacer();

	Label *gameplay_class_label = memnew(Label(TTRC("Gameplay Data Class")));
	gameplay_class_row->add_child(gameplay_class_label);

	gameplay_class_button = memnew(Button);
	gameplay_class_button->set_custom_minimum_size(Size2(200, 0) * EDSCALE);
	gameplay_class_button->set_clip_text(true);
	gameplay_class_button->set_text_alignment(HORIZONTAL_ALIGNMENT_LEFT);
	gameplay_class_button->connect(SceneStringName(pressed), callable_mp(this, &Box3DPhysicsSettingsTab::_gameplay_class_pressed));
	gameplay_class_row->add_child(gameplay_class_button);

	gameplay_class_clear_button = memnew(Button);
	gameplay_class_clear_button->set_flat(true);
	gameplay_class_clear_button->set_tooltip_text(TTRC("Clear the gameplay data class."));
	gameplay_class_clear_button->connect(SceneStringName(pressed), callable_mp(this, &Box3DPhysicsSettingsTab::_gameplay_class_cleared));
	gameplay_class_row->add_child(gameplay_class_clear_button);

	gameplay_class_dialog = memnew(CreateDialog);
	gameplay_class_dialog->set_base_type(Box3DSurfaceGameplayData::get_class_static());
	gameplay_class_dialog->connect("create", callable_mp(this, &Box3DPhysicsSettingsTab::_gameplay_class_picked));
	add_child(gameplay_class_dialog);

	HBoxContainer *toolbar = memnew(HBoxContainer);
	add_child(toolbar);

	Label *title = memnew(Label(TTRC("Surface Materials")));
	title->set_theme_type_variation("HeaderSmall");
	toolbar->add_child(title);
	toolbar->add_spacer();

	add_button = memnew(Button);
	add_button->set_text(TTRC("Configure Next"));
	add_button->connect(SceneStringName(pressed), callable_mp(this, &Box3DPhysicsSettingsTab::_add_pressed));
	toolbar->add_child(add_button);

	duplicate_button = memnew(Button);
	duplicate_button->set_text(TTRC("Duplicate"));
	duplicate_button->set_disabled(true);
	duplicate_button->connect(SceneStringName(pressed), callable_mp(this, &Box3DPhysicsSettingsTab::_duplicate_pressed));
	toolbar->add_child(duplicate_button);

	delete_button = memnew(Button);
	delete_button->set_text(TTRC("Clear"));
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
	// Without this the inline editor opens on the click that SELECTS a material, because Tree
	// tests `c.selected` after the click rather than before it (tree.cpp:3331). Reselect mode
	// tests `already_selected` too, giving click-to-switch and click-again-to-rename.
	material_tree->set_allow_reselect(true);
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
	// Off by default, and without it the divider between the Box3D fields and the project's
	// own gameplay fields is dropped silently.
	inspector->set_show_categories(true, true);
	// Deliberately no set_object_class(): it pins the doc class for every row, so the
	// flattened gameplay properties would be looked up on Box3DSurfaceMaterial and find
	// nothing. The category rows the proxy emits carry the doc class per section instead.
	inspector->connect("property_edited", callable_mp(this, &Box3DPhysicsSettingsTab::_inspector_property_edited));
	inspector_margin->add_child(inspector);

	empty_state = memnew(VBoxContainer);
	empty_state->set_alignment(BoxContainer::ALIGNMENT_CENTER);
	empty_state->set_v_size_flags(SIZE_EXPAND_FILL);
	add_child(empty_state);

	Label *empty_title = memnew(Label(TTRC("Surface material slots are unavailable.")));
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
	empty_add_button->set_text(TTRC("Configure Surface Material"));
	empty_add_button->set_h_size_flags(SIZE_SHRINK_CENTER);
	empty_add_button->connect(SceneStringName(pressed), callable_mp(this, &Box3DPhysicsSettingsTab::_add_pressed));
	empty_state->add_child(empty_add_button);

	delete_dialog = memnew(ConfirmationDialog);
	delete_dialog->set_title(TTRC("Clear Surface Material Slot"));
	delete_dialog->set_ok_button_text(TTRC("Clear"));
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
