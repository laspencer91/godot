/**************************************************************************/
/*  box3d_physics_settings_tab.h                                          */
/**************************************************************************/

#pragma once

#include "../box3d_surface_materials.h"

#include "scene/gui/box_container.h"

class Box3DSurfaceMaterialProxy;
class Button;
class ConfirmationDialog;
class CreateDialog;
class EditorInspector;
class HSplitContainer;
class Timer;
class Tree;

// Project Settings > Physics. Owns the surface material library end to end: the
// file at `physics/box3d/surface_material_library` is created, edited and saved
// from here, so no project ever has to hand-edit the `.tres`.
class Box3DPhysicsSettingsTab : public VBoxContainer {
	GDCLASS(Box3DPhysicsSettingsTab, VBoxContainer);

	HSplitContainer *split = nullptr;
	VBoxContainer *empty_state = nullptr;
	Tree *material_tree = nullptr;
	EditorInspector *inspector = nullptr;
	Box3DSurfaceMaterialProxy *proxy = nullptr;

	Button *add_button = nullptr;
	Button *duplicate_button = nullptr;
	Button *delete_button = nullptr;
	Button *empty_add_button = nullptr;

	// One project-wide choice of gameplay data class, reflected by every material's
	// `gameplay` picker through Box3DSurfaceMaterial::_validate_property.
	Button *gameplay_class_button = nullptr;
	Button *gameplay_class_clear_button = nullptr;
	CreateDialog *gameplay_class_dialog = nullptr;

	ConfirmationDialog *delete_dialog = nullptr;
	// Field edits arrive one per drag step, so coalesce them instead of rewriting and
	// reparsing the library on every increment.
	Timer *save_timer = nullptr;

	Ref<Box3DSurfaceMaterialLibrary> library;
	int selected_slot = 0;
	// Set while this tab is the one writing the library, so the registry rebuild it
	// triggers does not bounce back in as an external change.
	bool committing = false;
	// Set while the tree is being repopulated, so the `set_text` calls that does are
	// not mistaken for a user rename.
	bool updating = false;

	Ref<Box3DSurfaceMaterialLibrary> _ensure_library();
	TypedArray<Box3DSurfaceMaterial> _current_materials() const;
	void _commit(const String &p_action_name, const TypedArray<Box3DSurfaceMaterial> &p_materials, int p_select_slot);
	void _materials_committed(int p_select_slot);
	void _save_library();
	void _flush_pending_save();

	void _refresh();
	void _update_inspector();
	void _register_gameplay_property_docs();
	void _item_selected();
	void _item_renamed();

	int _find_empty_slot() const;
	void _configure_slot(int p_slot, const Ref<Box3DSurfaceMaterial> &p_source, const StringName &p_name_base, const String &p_action_name);
	void _add_pressed();
	void _duplicate_pressed();
	void _delete_pressed();
	void _delete_confirmed();

	void _update_gameplay_class_button();
	void _gameplay_class_pressed();
	void _gameplay_class_picked();
	void _gameplay_class_cleared();
	void _set_gameplay_class(const String &p_class);
	// The setting holds either a global class name or a script path, so both forms
	// resolve through here.
	static Ref<Script> _load_gameplay_script();
	Ref<Box3DSurfaceGameplayData> _instantiate_gameplay_data() const;
	void _backfill_gameplay_data();

	void _inspector_property_edited(const String &p_property);
	void _undo_redo_inspector_callback(Object *p_undo_redo, Object *p_modified_object, const String &p_property, const Variant &p_value);
	void _surface_materials_changed();
	void _scripts_reloaded();

protected:
	static void _bind_methods();
	void _notification(int p_what);

public:
	Box3DPhysicsSettingsTab();
	~Box3DPhysicsSettingsTab();
};
