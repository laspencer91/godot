/**************************************************************************/
/*  blockout_material_registry.h                                          */
/**************************************************************************/
/*  G-Level LE2: ten project-overridable procedural blockout slots.       */
/**************************************************************************/

#pragma once

#include "core/object/ref_counted.h"
#include "scene/resources/material.h"

class MaterialIndex;

class BlockoutMaterialRegistry : public RefCounted {
	GDCLASS(BlockoutMaterialRegistry, RefCounted);

public:
	static constexpr int SLOT_COUNT = 10;

private:
	Ref<MaterialIndex> material_index;
	Ref<StandardMaterial3D> builtins[SLOT_COUNT];
	Ref<Material> slots[SLOT_COUNT];
	String slot_paths[SLOT_COUNT];

	Ref<StandardMaterial3D> _get_or_create_builtin(int p_slot);
	static String _get_override_folder();
	struct PathComparator {
		bool operator()(const String &p_a, const String &p_b) const;
	};

protected:
	static void _bind_methods();

public:
	void initialize(const Ref<MaterialIndex> &p_material_index);
	void resolve_slots();
	Ref<Material> get_slot(int p_slot);
	String get_slot_path(int p_slot);
	TypedArray<Material> get_resolved_materials();
	PackedStringArray get_resolved_paths();
	~BlockoutMaterialRegistry();
};
