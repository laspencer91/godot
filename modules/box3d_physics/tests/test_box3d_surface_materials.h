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

TEST_CASE("[Box3D][SurfaceMaterials] Library always owns fifteen permanent slots") {
	Ref<Box3DSurfaceMaterialLibrary> library;
	library.instantiate();

	const TypedArray<Box3DSurfaceMaterial> slots = library->get_materials();
	CHECK(slots.size() == 15);
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
	library->set_materials(legacy);

	CHECK(library->get_materials().size() == 15);
	CHECK(library->get_material_slot(7) == retained);
	CHECK(retained->get_material_id() == 8);
	CHECK(library->find_material("Metal") == moved);
	CHECK(moved->get_material_id() >= 1);
	CHECK(moved->get_material_id() <= 15);
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
	TypedArray<Box3DSurfaceMaterial> authored;
	for (int i = 0; i < 16; i++) {
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
	CHECK(library->get_materials().size() == 16);
	CHECK(library->find_material("Material 16").is_valid());
}

} // namespace TestBox3DSurfaceMaterials
