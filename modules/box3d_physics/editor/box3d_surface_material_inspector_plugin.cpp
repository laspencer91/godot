/**************************************************************************/
/*  box3d_surface_material_inspector_plugin.cpp                           */
/**************************************************************************/

#include "box3d_surface_material_inspector_plugin.h"

#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "scene/gui/option_button.h"
#include "scene/scene_string_names.h"

static String _material_label(const Ref<Box3DSurfaceMaterial> &p_material) {
	if (p_material.is_null()) {
		return String();
	}
	const String name = String(p_material->get_material_name());
	if (!name.is_empty()) {
		return name;
	}
	const String path = p_material->get_path();
	return path.is_empty() ? TTR("<unnamed>") : path.get_file();
}

void Box3DSurfaceMaterialProperty::_set_read_only(bool p_read_only) {
	options->set_disabled(p_read_only);
}

void Box3DSurfaceMaterialProperty::_option_selected(int p_which) {
	if (updating) {
		return;
	}
	const Ref<Box3DSurfaceMaterial> surface_material = options->get_item_metadata(p_which);
	emit_changed(get_edited_property(), surface_material);
}

void Box3DSurfaceMaterialProperty::_populate_options() {
	options->clear();

	Box3DPhysics *box3d_physics = Box3DPhysics::get_singleton();
	const TypedArray<Box3DSurfaceMaterial> materials = box3d_physics != nullptr ? box3d_physics->get_materials() : TypedArray<Box3DSurfaceMaterial>();

	// With no library the picker would otherwise be a lone "<none>" with no clue about
	// where materials come from.
	options->add_item(materials.is_empty() ? TTR("No surface materials (add them in Project Settings > Physics)") : TTR("<none>"));
	options->set_item_metadata(0, Variant());

	for (int i = 0; i < materials.size(); i++) {
		const Ref<Box3DSurfaceMaterial> surface_material = materials[i];
		options->add_item(_material_label(surface_material));
		options->set_item_metadata(-1, surface_material);
	}
}

void Box3DSurfaceMaterialProperty::update_property() {
	updating = true;
	_populate_options();

	const Ref<Box3DSurfaceMaterial> current = get_edited_property_value();
	int selected_index = 0;
	if (current.is_valid()) {
		selected_index = -1;
		for (int i = 1; i < options->get_item_count(); i++) {
			if (Ref<Box3DSurfaceMaterial>(options->get_item_metadata(i)) == current) {
				selected_index = i;
				break;
			}
		}
		if (selected_index == -1) {
			// Sub-resources authored before the library existed, or a material dropped from
			// it since. Keep the value visible instead of silently clearing it.
			options->add_item(vformat(TTR("%s (not in library)"), _material_label(current)));
			options->set_item_metadata(-1, current);
			selected_index = options->get_item_count() - 1;
		}
	}
	options->select(selected_index);
	updating = false;
}

void Box3DSurfaceMaterialProperty::_notification(int p_what) {
	// Built inside the cases: every other notification (draw, resize, theme) would
	// otherwise allocate a Callable for nothing.
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			Box3DPhysics *box3d_physics = Box3DPhysics::get_singleton();
			if (box3d_physics) {
				box3d_physics->connect(SNAME("surface_materials_changed"), callable_mp(this, &Box3DSurfaceMaterialProperty::update_property));
			}
		} break;
		case NOTIFICATION_EXIT_TREE: {
			Box3DPhysics *box3d_physics = Box3DPhysics::get_singleton();
			if (box3d_physics) {
				box3d_physics->disconnect(SNAME("surface_materials_changed"), callable_mp(this, &Box3DSurfaceMaterialProperty::update_property));
			}
		} break;
	}
}

Box3DSurfaceMaterialProperty::Box3DSurfaceMaterialProperty() {
	options = memnew(OptionButton);
	options->set_clip_text(true);
	options->set_fit_to_longest_item(false);
	options->set_flat(true);
	options->set_theme_type_variation(SNAME("EditorInspectorButton"));
	options->set_auto_translate_mode(AUTO_TRANSLATE_MODE_DISABLED);
	options->set_search_bar_enabled(true);
	options->set_search_bar_min_item_count(10);
	add_child(options);
	add_focusable(options);
	options->connect(SceneStringName(item_selected), callable_mp(this, &Box3DSurfaceMaterialProperty::_option_selected));
}

bool Box3DSurfaceMaterialInspectorPlugin::can_handle(Object *p_object) {
	return true;
}

bool Box3DSurfaceMaterialInspectorPlugin::parse_property(Object *p_object, const Variant::Type p_type, const String &p_path, const PropertyHint p_hint, const String &p_hint_text, const BitField<PropertyUsageFlags> p_usage, const bool p_wide) {
	if (p_type != Variant::OBJECT || p_hint != PROPERTY_HINT_RESOURCE_TYPE) {
		return false;
	}
	// A multi-type hint means the property accepts more than surface materials, so the
	// generic resource picker stays.
	if (p_hint_text.get_slice_count(",") != 1) {
		return false;
	}
	const String type = p_hint_text.strip_edges();
	if (type != Box3DSurfaceMaterial::get_class_static() && !ClassDB::is_parent_class(type, Box3DSurfaceMaterial::get_class_static())) {
		return false;
	}

	add_property_editor(p_path, memnew(Box3DSurfaceMaterialProperty));
	return true;
}
