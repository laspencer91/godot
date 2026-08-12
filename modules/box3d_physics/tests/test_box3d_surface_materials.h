/**************************************************************************/
/*  test_box3d_surface_materials.h                                        */
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

#pragma once

#include "../box3d_surface_materials.h"

#include "tests/test_macros.h"

namespace TestBox3DSurfaceMaterials {

// The slot count is deliberately raisable, so assert against it rather than against a literal.
TEST_CASE("[Box3D][SurfaceMaterials] Library always owns a full bank of permanent slots") {
	Ref<Box3DSurfaceMaterialLibrary> library;
	library.instantiate();

	const TypedArray<Box3DSurfaceMaterial> slots = library->get_materials();
	CHECK(slots.size() == Box3DSurfaceMaterialLibrary::get_material_slot_count());
	for (int i = 0; i < slots.size(); i++) {
		const Ref<Box3DSurfaceMaterial> material = slots[i];
		REQUIRE(material.is_valid());
		CHECK(material->get_material_id() == i + 1);
		CHECK(material->get_material_name() == StringName());
	}
	CHECK(library->find_material_index(StringName()) == -1);
	CHECK(library->find_material(StringName()).is_null());
}

TEST_CASE("[Box3D][SurfaceMaterials] Legacy ids migrate into fixed slots") {
	Ref<Box3DSurfaceMaterial> retained;
	retained.instantiate();
	retained->set_material_name("Stone");
	retained->set_material_id(8);

	Ref<Box3DSurfaceMaterial> moved;
	moved.instantiate();
	moved->set_material_name("Metal");
	moved->set_material_id(42);

	TypedArray<Box3DSurfaceMaterial> legacy;
	legacy.push_back(retained);
	legacy.push_back(moved);

	Ref<Box3DSurfaceMaterialLibrary> library;
	library.instantiate();
	// "Metal" carries an id past the bank, so it cannot keep it. Being moved is the expected
	// outcome, and it is reported rather than silent — hence the warning suppressed here.
	ERR_PRINT_OFF;
	library->set_materials(legacy);
	ERR_PRINT_ON;

	CHECK(library->get_materials().size() == Box3DSurfaceMaterialLibrary::get_material_slot_count());
	CHECK(library->get_material_slot(7) == retained);
	CHECK(retained->get_material_id() == 8);
	CHECK(library->find_material("Metal") == moved);
	CHECK(moved->get_material_id() >= 1);
	CHECK(moved->get_material_id() <= Box3DSurfaceMaterialLibrary::get_material_slot_count());
}

TEST_CASE("[Box3D][SurfaceMaterials] Deep duplicate owns independent gameplay data") {
	Ref<Box3DSurfaceGameplayData> gameplay;
	gameplay.instantiate();

	Ref<Box3DSurfaceMaterial> source;
	source.instantiate();
	source->set_material_name("Source");
	source->set_gameplay(gameplay);

	Ref<Box3DSurfaceMaterial> duplicate = source->duplicate(true);
	REQUIRE(duplicate.is_valid());
	REQUIRE(duplicate->get_gameplay().is_valid());
	CHECK(duplicate->get_gameplay() != source->get_gameplay());
}

TEST_CASE("[Box3D][SurfaceMaterials] Oversized libraries preserve authored data") {
	// One past the bank, whatever the bank currently is.
	const int oversized_count = Box3DSurfaceMaterialLibrary::get_material_slot_count() + 1;
	const StringName last_name = StringName("Material " + itos(oversized_count));

	TypedArray<Box3DSurfaceMaterial> authored;
	for (int i = 0; i < oversized_count; i++) {
		Ref<Box3DSurfaceMaterial> material;
		material.instantiate();
		material->set_material_name(StringName("Material " + itos(i + 1)));
		material->set_material_id(i + 1);
		authored.push_back(material);
	}

	Ref<Box3DSurfaceMaterialLibrary> library;
	library.instantiate();
	ERR_PRINT_OFF;
	library->set_materials(authored);
	ERR_PRINT_ON;

	CHECK_FALSE(library->is_slot_layout_valid());
	CHECK(library->get_materials().size() == oversized_count);
	CHECK(library->find_material(last_name).is_valid());
}

TEST_CASE("[Box3D][SurfaceMaterials] Surface maps store ids") {
	Ref<Box3DSurfaceMap> map;
	map.instantiate();

	PackedInt32Array ids;
	ids.push_back(4);
	ids.push_back(0);
	map->set_material_ids(ids);

	CHECK(map->get_material_ids() == ids);
}

TEST_CASE("[Box3D][SurfaceMaterials] Legacy name maps convert to ids on first read") {
	Ref<Box3DSurfaceMap> map;
	map.instantiate();

	PackedStringArray names;
	names.push_back("NoSuchMaterial");
	// Assigning names is the load path for a pre-id resource. Reading ids back converts, and
	// the unresolvable entry degrades to the default material rather than staying a name.
	map->set_material_names(names);

	ERR_PRINT_OFF;
	const PackedInt32Array ids = map->get_material_ids();
	ERR_PRINT_ON;

	REQUIRE(ids.size() == 1);
	CHECK(ids[0] == 0);
	// Converted in place: a second read no longer has names to migrate.
	CHECK(map->get_material_ids() == ids);
}

} // namespace TestBox3DSurfaceMaterials
