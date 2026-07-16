/**************************************************************************/
/*  blockout_material_registry.cpp                                        */
/**************************************************************************/
/*  G-Level LE2: ten project-overridable procedural blockout slots.       */
/**************************************************************************/

#include "blockout_material_registry.h"

#include "core/io/resource_loader.h"
#include "core/object/class_db.h"
#include "editor/level/material_index.h"
#include "editor/settings/editor_settings.h"
#include "scene/resources/image_texture.h"

bool BlockoutMaterialRegistry::PathComparator::operator()(const String &p_a, const String &p_b) const {
	const int file_order = p_a.get_file().naturalnocasecmp_to(p_b.get_file());
	return file_order == 0 ? p_a < p_b : file_order < 0;
}

String BlockoutMaterialRegistry::_get_override_folder() {
	EditorSettings *settings = EditorSettings::get_singleton();
	if (!settings || !settings->has_setting("level_editor/material_browser/blockout_override_folder")) {
		return String();
	}
	return String(settings->get("level_editor/material_browser/blockout_override_folder")).trim_suffix("/");
}

Ref<StandardMaterial3D> BlockoutMaterialRegistry::_get_or_create_builtin(int p_slot) {
	ERR_FAIL_INDEX_V(p_slot, SLOT_COUNT, Ref<StandardMaterial3D>());
	if (builtins[p_slot].is_valid()) {
		return builtins[p_slot];
	}

	static const Color colors[SLOT_COUNT] = {
		Color("4a4d52"), Color("696d73"), Color("92969c"), Color("c2c5c9"),
		Color("d46a2d"), Color("e89b3f"), Color("3f72bb"), Color("53a4cf"),
		Color("4f9a62"), Color("78b94d")
	};
	static const char *names[SLOT_COUNT] = {
		"Blockout 1 - Charcoal", "Blockout 2 - Dark Gray", "Blockout 3 - Mid Gray", "Blockout 4 - Light Gray",
		"Blockout 5 - Orange", "Blockout 6 - Amber", "Blockout 7 - Blue", "Blockout 8 - Cyan",
		"Blockout 9 - Green", "Blockout 0 - Lime"
	};

	const Color base = colors[p_slot];
	const Color alternate = base.lerp(Color(1, 1, 1), 0.16f);
	const Color grid = base.lerp(Color(0, 0, 0), 0.58f);
	const Color axis = p_slot % 2 == 0 ? Color(1.0, 0.82, 0.22) : Color(0.95, 0.95, 0.95);
	Ref<Image> image = Image::create_empty(64, 64, false, Image::FORMAT_RGBA8);
	for (int y = 0; y < 64; y++) {
		for (int x = 0; x < 64; x++) {
			Color pixel = (((x / 8) + (y / 8)) & 1) == 0 ? base : alternate;
			if (x % 16 == 0 || y % 16 == 0) {
				pixel = grid;
			}
			if (x == 32 || y == 32) {
				pixel = axis;
			}
			image->set_pixel(x, y, pixel);
		}
	}

	Ref<ImageTexture> texture = ImageTexture::create_from_image(image);
	texture->set_name(names[p_slot]);
	Ref<StandardMaterial3D> material;
	material.instantiate();
	material->set_name(names[p_slot]);
	material->set_texture(BaseMaterial3D::TEXTURE_ALBEDO, texture);
	material->set_texture_filter(BaseMaterial3D::TEXTURE_FILTER_NEAREST_WITH_MIPMAPS);
	material->set_flag(BaseMaterial3D::FLAG_USE_TEXTURE_REPEAT, true);
	material->set_roughness(0.9f);
	material->set_metallic(0.0f);
	builtins[p_slot] = material;
	return material;
}

void BlockoutMaterialRegistry::_bind_methods() {
	ClassDB::bind_method(D_METHOD("resolve_slots"), &BlockoutMaterialRegistry::resolve_slots);
	ClassDB::bind_method(D_METHOD("get_slot", "slot"), &BlockoutMaterialRegistry::get_slot);
	ClassDB::bind_method(D_METHOD("get_slot_path", "slot"), &BlockoutMaterialRegistry::get_slot_path);
	ClassDB::bind_method(D_METHOD("get_resolved_materials"), &BlockoutMaterialRegistry::get_resolved_materials);
	ClassDB::bind_method(D_METHOD("get_resolved_paths"), &BlockoutMaterialRegistry::get_resolved_paths);
}

void BlockoutMaterialRegistry::initialize(const Ref<MaterialIndex> &p_material_index) {
	material_index = p_material_index;
}

void BlockoutMaterialRegistry::resolve_slots() {
	Vector<String> overrides;
	const String override_folder = _get_override_folder();
	if (!override_folder.is_empty() && material_index.is_valid()) {
		for (const KeyValue<String, MaterialIndexEntry> &indexed : material_index->get_entries()) {
			if (indexed.value.folder == override_folder) {
				overrides.push_back(indexed.key);
			}
		}
		overrides.sort_custom<PathComparator>();
	}

	int override_index = 0;
	for (int slot = 0; slot < SLOT_COUNT; slot++) {
		slots[slot].unref();
		slot_paths[slot].clear();
		while (override_index < overrides.size() && slots[slot].is_null()) {
			const String path = overrides[override_index++];
			Ref<Material> material = ResourceLoader::load(path);
			if (material.is_valid()) {
				slots[slot] = material;
				slot_paths[slot] = path;
			}
		}
		if (slots[slot].is_null()) {
			slots[slot] = _get_or_create_builtin(slot);
		}
	}
}

Ref<Material> BlockoutMaterialRegistry::get_slot(int p_slot) {
	ERR_FAIL_INDEX_V(p_slot, SLOT_COUNT, Ref<Material>());
	resolve_slots();
	return slots[p_slot];
}

String BlockoutMaterialRegistry::get_slot_path(int p_slot) {
	ERR_FAIL_INDEX_V(p_slot, SLOT_COUNT, String());
	resolve_slots();
	return slot_paths[p_slot];
}

TypedArray<Material> BlockoutMaterialRegistry::get_resolved_materials() {
	resolve_slots();
	TypedArray<Material> result;
	for (int slot = 0; slot < SLOT_COUNT; slot++) {
		result.push_back(slots[slot]);
	}
	return result;
}

PackedStringArray BlockoutMaterialRegistry::get_resolved_paths() {
	resolve_slots();
	PackedStringArray result;
	for (int slot = 0; slot < SLOT_COUNT; slot++) {
		result.push_back(slot_paths[slot]);
	}
	return result;
}

BlockoutMaterialRegistry::~BlockoutMaterialRegistry() = default;
