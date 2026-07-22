/**************************************************************************/
/*  test_csg.h                                                            */
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

#include "../csg_shape.h"
#ifdef DEV_ENABLED
#include "../csg_debug_counters.h"
#endif // DEV_ENABLED

#include "scene/main/scene_tree.h"
#include "scene/main/window.h"
#include "scene/resources/material.h"
#include "scene/resources/mesh.h"
#include "tests/test_macros.h"

namespace TestCSG {

static CSGBox3D *_add_box(
		CSGShape3D *p_parent,
		const Vector3 &p_size,
		const Vector3 &p_position,
		CSGShape3D::Operation p_operation = CSGShape3D::OPERATION_UNION,
		const Ref<Material> &p_material = Ref<Material>()) {
	CSGBox3D *box = memnew(CSGBox3D);
	box->set_size(p_size);
	box->set_position(p_position);
	box->set_operation(p_operation);
	if (p_material.is_valid()) {
		box->set_material(p_material);
	}
	p_parent->add_child(box);
	return box;
}

static double _calculate_brush_volume(const Vector<Vector3> &p_faces) {
	double signed_volume = 0.0;
	for (int i = 0; i < p_faces.size(); i += 3) {
		signed_volume += p_faces[i].dot(p_faces[i + 1].cross(p_faces[i + 2])) / 6.0;
	}
	return Math::abs(signed_volume);
}

static void _reset_csg_counters() {
#ifdef DEV_ENABLED
	CSGDebugCounters::reset();
#endif // DEV_ENABLED
}

TEST_CASE("[SceneTree][CSG] Phase 0 operation ordering") {
	CSGBox3D *root = memnew(CSGBox3D);
	root->set_size(Vector3(4, 4, 4));
	CSGBox3D *subtraction = _add_box(root, Vector3(2, 2, 2), Vector3(), CSGShape3D::OPERATION_SUBTRACTION);
	CSGBox3D *addition = _add_box(root, Vector3(1, 1, 1), Vector3(), CSGShape3D::OPERATION_UNION);
	SceneTree::get_singleton()->get_root()->add_child(root);

	Vector<Vector3> faces = root->get_brush_faces();
	CHECK(_calculate_brush_volume(faces) == doctest::Approx(57.0));

	root->move_child(addition, 0);
	faces = root->get_brush_faces();
	CHECK(_calculate_brush_volume(faces) == doctest::Approx(56.0));
	CHECK_EQ(root->get_child(0), addition);
	CHECK_EQ(root->get_child(1), subtraction);

	root->queue_free();
}

TEST_CASE("[SceneTree][CSG] Phase 0 Add Subtract Intersect grouping") {
	CSGCombiner3D *root = memnew(CSGCombiner3D);
	_add_box(root, Vector3(4, 4, 4), Vector3(), CSGShape3D::OPERATION_UNION);
	_add_box(root, Vector3(2, 2, 2), Vector3(), CSGShape3D::OPERATION_SUBTRACTION);
	_add_box(root, Vector3(1, 2, 2), Vector3(1.5, 0, 0), CSGShape3D::OPERATION_INTERSECTION);
	SceneTree::get_singleton()->get_root()->add_child(root);

	_reset_csg_counters();
	Vector<Vector3> faces = root->get_brush_faces();
	CHECK(_calculate_brush_volume(faces) == doctest::Approx(4.0));
	CHECK(root->get_aabb().is_equal_approx(AABB(Vector3(1, -1, -1), Vector3(1, 2, 2))));

#ifdef DEV_ENABLED
	CSGDebugCounters counters = CSGDebugCounters::get();
	// The current full rebuild packs each of the three primitive leaves once.
	CHECK_EQ(counters.local_primitive_brush_packs, 3);
	CHECK_EQ(counters.transformed_wrapper_constructions, 3);
	CHECK_EQ(counters.batch_boolean_calls, 6);
	CHECK_EQ(counters.operation_switch_flushes, 2);
	CHECK_EQ(counters.root_materializations, 1);
#endif // DEV_ENABLED

	root->queue_free();
}

TEST_CASE("[SceneTree][CSG] Phase 0 nested combiner transforms") {
	CSGCombiner3D *root = memnew(CSGCombiner3D);
	CSGCombiner3D *nested = memnew(CSGCombiner3D);
	nested->set_transform(Transform3D(Basis(Vector3(0, 0, 1), Math::deg_to_rad(90.0f)), Vector3(3, 0, 0)));
	root->add_child(nested);
	_add_box(nested, Vector3(2, 4, 6), Vector3(1, 0, 0));
	SceneTree::get_singleton()->get_root()->add_child(root);

	_reset_csg_counters();
	Vector<Vector3> faces = root->get_brush_faces();
	CHECK_EQ(faces.size(), 36);
	CHECK(root->get_aabb().is_equal_approx(AABB(Vector3(1, 0, -3), Vector3(4, 2, 6))));

#ifdef DEV_ENABLED
	CSGDebugCounters counters = CSGDebugCounters::get();
	CHECK_EQ(counters.local_primitive_brush_packs, 1);
	CHECK_EQ(counters.transformed_wrapper_constructions, 2);
	CHECK_EQ(counters.batch_boolean_calls, 3);
	CHECK_EQ(counters.operation_switch_flushes, 0);
	CHECK_EQ(counters.root_materializations, 1);
#endif // DEV_ENABLED

	root->queue_free();
}

TEST_CASE("[SceneTree][CSG] Phase 0 material and UV finalization") {
	Ref<StandardMaterial3D> material_a;
	material_a.instantiate();
	Ref<StandardMaterial3D> material_b;
	material_b.instantiate();

	CSGCombiner3D *material_root = memnew(CSGCombiner3D);
	_add_box(material_root, Vector3(1, 1, 1), Vector3(-1, 0, 0), CSGShape3D::OPERATION_UNION, material_a);
	_add_box(material_root, Vector3(1, 1, 1), Vector3(1, 0, 0), CSGShape3D::OPERATION_UNION, material_b);
	SceneTree::get_singleton()->get_root()->add_child(material_root);
	material_root->update_shape();

	Ref<ArrayMesh> material_mesh = material_root->bake_static_mesh();
	REQUIRE(material_mesh.is_valid());
	CHECK_EQ(material_mesh->get_surface_count(), 2);
	bool found_material_a = false;
	bool found_material_b = false;
	for (int i = 0; i < material_mesh->get_surface_count(); i++) {
		Ref<Material> surface_material = material_mesh->surface_get_material(i);
		found_material_a |= surface_material.ptr() == material_a.ptr();
		found_material_b |= surface_material.ptr() == material_b.ptr();
	}
	CHECK(found_material_a);
	CHECK(found_material_b);

	CSGBox3D *uv_root = memnew(CSGBox3D);
	SceneTree::get_singleton()->get_root()->add_child(uv_root);
	_reset_csg_counters();
	uv_root->update_shape();

	Ref<ArrayMesh> uv_mesh = uv_root->bake_static_mesh();
	REQUIRE(uv_mesh.is_valid());
	REQUIRE_EQ(uv_mesh->get_surface_count(), 1);
	Array arrays = uv_mesh->surface_get_arrays(0);
	PackedVector2Array uvs = arrays[Mesh::ARRAY_TEX_UV];
	PackedFloat32Array tangents = arrays[Mesh::ARRAY_TANGENT];
	CHECK_EQ(uvs.size(), 36);
	CHECK_EQ(tangents.size(), uvs.size() * 4);

	int uv_00_count = 0;
	int uv_01_count = 0;
	int uv_11_count = 0;
	int uv_10_count = 0;
	for (const Vector2 &uv : uvs) {
		if (uv.is_equal_approx(Vector2(0, 0))) {
			uv_00_count++;
		} else if (uv.is_equal_approx(Vector2(0, 1))) {
			uv_01_count++;
		} else if (uv.is_equal_approx(Vector2(1, 1))) {
			uv_11_count++;
		} else if (uv.is_equal_approx(Vector2(1, 0))) {
			uv_10_count++;
		} else {
			FAIL_CHECK("The default CSGBox3D emitted a UV outside its four existing corner values.");
		}
	}
	CHECK_EQ(uv_00_count, 12);
	CHECK_EQ(uv_01_count, 6);
	CHECK_EQ(uv_11_count, 12);
	CHECK_EQ(uv_10_count, 6);

#ifdef DEV_ENABLED
	CSGDebugCounters counters = CSGDebugCounters::get();
	CHECK_EQ(counters.root_materializations, 1);
	CHECK_EQ(counters.uv_finalizations, 1);
	CHECK_EQ(counters.tangent_finalizations, 1);
#endif // DEV_ENABLED

	material_root->queue_free();
	uv_root->queue_free();
}

TEST_CASE("[SceneTree][CSG] Phase 0 empty combiners and hidden children") {
	CSGCombiner3D *empty_root = memnew(CSGCombiner3D);
	SceneTree::get_singleton()->get_root()->add_child(empty_root);
	CHECK(empty_root->get_brush_faces().is_empty());
	CHECK_EQ(empty_root->get_aabb(), AABB());
	empty_root->update_shape();
	Ref<ArrayMesh> empty_mesh = empty_root->bake_static_mesh();
	REQUIRE(empty_mesh.is_valid());
	CHECK_EQ(empty_mesh->get_surface_count(), 0);

	CSGCombiner3D *visibility_root = memnew(CSGCombiner3D);
	_add_box(visibility_root, Vector3(2, 2, 2), Vector3(2, 0, 0));
	CSGBox3D *hidden = _add_box(visibility_root, Vector3(2, 2, 2), Vector3(-4, 0, 0));
	hidden->hide();
	SceneTree::get_singleton()->get_root()->add_child(visibility_root);

	_reset_csg_counters();
	Vector<Vector3> faces = visibility_root->get_brush_faces();
	CHECK_EQ(faces.size(), 36);
	CHECK(visibility_root->get_aabb().is_equal_approx(AABB(Vector3(1, -1, -1), Vector3(2, 2, 2))));

#ifdef DEV_ENABLED
	CSGDebugCounters counters = CSGDebugCounters::get();
	CHECK_EQ(counters.local_primitive_brush_packs, 1);
	CHECK_EQ(counters.transformed_wrapper_constructions, 1);
#endif // DEV_ENABLED

	empty_root->queue_free();
	visibility_root->queue_free();
}

#ifndef PHYSICS_3D_DISABLED
TEST_CASE("[SceneTree][CSG] Phase 0 collision shape and AABB") {
	CSGBox3D *root = memnew(CSGBox3D);
	root->set_size(Vector3(2, 4, 6));
	root->set_use_collision(true);
	SceneTree::get_singleton()->get_root()->add_child(root);

	_reset_csg_counters();
	root->update_shape();
	CHECK(root->get_aabb().is_equal_approx(AABB(Vector3(-1, -2, -3), Vector3(2, 4, 6))));
	CHECK(root->_get_root_collision_instance().is_valid());

	Ref<ConcavePolygonShape3D> collision_shape = root->bake_collision_shape();
	REQUIRE(collision_shape.is_valid());
	CHECK_EQ(collision_shape->get_faces().size(), 36);

#ifdef DEV_ENABLED
	CSGDebugCounters counters = CSGDebugCounters::get();
	CHECK_EQ(counters.collision_rebuilds, 1);
#endif // DEV_ENABLED

	root->queue_free();
}
#endif // PHYSICS_3D_DISABLED

TEST_CASE("[SceneTree][CSG] CSGPolygon3D") {
	SUBCASE("[SceneTree][CSG] CSGPolygon3D: using accurate path tangent for polygon rotation") {
		const float polygon_radius = 10.0f;

		const Vector3 expected_min_bounds = Vector3(-polygon_radius, -polygon_radius, 0);
		const Vector3 expected_max_bounds = Vector3(100 + polygon_radius, polygon_radius, 100);
		const AABB expected_aabb = AABB(expected_min_bounds, expected_max_bounds - expected_min_bounds);

		Ref<Curve3D> curve;
		curve.instantiate();
		curve->add_point(
				// p_position
				Vector3(0, 0, 0),
				// p_in
				Vector3(),
				// p_out
				Vector3(0, 0, 60));
		curve->add_point(
				// p_position
				Vector3(100, 0, 100),
				// p_in
				Vector3(0, 0, -60),
				// p_out
				Vector3());

		Path3D *path = memnew(Path3D);
		path->set_curve(curve);

		CSGPolygon3D *csg_polygon_3d = memnew(CSGPolygon3D);
		SceneTree::get_singleton()->get_root()->add_child(csg_polygon_3d);

		csg_polygon_3d->add_child(path);
		csg_polygon_3d->set_path_node(csg_polygon_3d->get_path_to(path));
		csg_polygon_3d->set_mode(CSGPolygon3D::Mode::MODE_PATH);

		PackedVector2Array polygon;
		polygon.append(Vector2(-polygon_radius, 0));
		polygon.append(Vector2(0, polygon_radius));
		polygon.append(Vector2(polygon_radius, 0));
		polygon.append(Vector2(0, -polygon_radius));
		csg_polygon_3d->set_polygon(polygon);

		csg_polygon_3d->set_path_rotation(CSGPolygon3D::PathRotation::PATH_ROTATION_PATH);
		csg_polygon_3d->set_path_rotation_accurate(true);

		// Minimize the number of extrusions.
		// This decreases the number of samples taken from the curve.
		// Having fewer samples increases the inaccuracy of the line between samples as an approximation of the tangent of the curve.
		// With correct polygon orientation, the bounding box for the given curve should be independent of the number of extrusions.
		csg_polygon_3d->set_path_interval_type(CSGPolygon3D::PathIntervalType::PATH_INTERVAL_DISTANCE);
		csg_polygon_3d->set_path_interval(1000.0f);

		// Call get_brush_faces to force the bounding box to update.
		csg_polygon_3d->get_brush_faces();

		CHECK(csg_polygon_3d->get_aabb().is_equal_approx(expected_aabb));

		// Perform the bounding box check again with a greater number of extrusions.
		csg_polygon_3d->set_path_interval(1.0f);
		csg_polygon_3d->get_brush_faces();

		CHECK(csg_polygon_3d->get_aabb().is_equal_approx(expected_aabb));

		csg_polygon_3d->remove_child(path);
		SceneTree::get_singleton()->get_root()->remove_child(csg_polygon_3d);

		memdelete(csg_polygon_3d);
		memdelete(path);
	}
}

} // namespace TestCSG
