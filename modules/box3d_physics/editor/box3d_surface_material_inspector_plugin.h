/**************************************************************************/
/*  box3d_surface_material_inspector_plugin.h                             */
/**************************************************************************/

#pragma once

#include "../box3d_surface_materials.h"

#include "editor/inspector/editor_inspector.h"

class OptionButton;

class Box3DSurfaceMaterialProperty : public EditorProperty {
	GDCLASS(Box3DSurfaceMaterialProperty, EditorProperty);

	OptionButton *options = nullptr;
	bool updating = false;

	void _option_selected(int p_which);
	void _populate_options();

protected:
	void _notification(int p_what);
	virtual void _set_read_only(bool p_read_only) override;

public:
	virtual void update_property() override;

	Box3DSurfaceMaterialProperty();
};

class Box3DSurfaceMaterialInspectorPlugin : public EditorInspectorPlugin {
	GDCLASS(Box3DSurfaceMaterialInspectorPlugin, EditorInspectorPlugin);

public:
	bool can_handle(Object *p_object) override;
	bool parse_property(Object *p_object, const Variant::Type p_type, const String &p_path, const PropertyHint p_hint, const String &p_hint_text, const BitField<PropertyUsageFlags> p_usage, const bool p_wide) override;
};
