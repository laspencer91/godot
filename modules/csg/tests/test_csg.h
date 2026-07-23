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

#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/resource_loader.h"
#include "core/io/resource_saver.h"
#include "core/object/message_queue.h"
#include "core/os/os.h"
#include "core/templates/hash_set.h"
#include "scene/main/scene_tree.h"
#include "scene/main/window.h"
#include "scene/resources/3d/primitive_meshes.h"
#include "scene/resources/material.h"
#include "scene/resources/mesh.h"
#include "scene/resources/packed_scene.h"
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

struct CSGSynchronousSchedulerScope {
	CSGSynchronousSchedulerScope() {
		CSGShape3D::set_async_evaluation_force_synchronous(true);
	}

	~CSGSynchronousSchedulerScope() {
		CSGShape3D::set_async_evaluation_force_synchronous(false);
	}
};

struct CSGTemporaryFileCleanup {
	Vector<String> paths;

	~CSGTemporaryFileCleanup() {
		for (const String &path : paths) {
			if (FileAccess::exists(path)) {
				DirAccess::remove_absolute(path);
			}
		}
	}
};

#ifndef PHYSICS_3D_DISABLED
static CSGBox3D *_make_collision_box_root() {
	CSGBox3D *root = memnew(CSGBox3D);
	SceneTree::get_singleton()->get_root()->add_child(root);
	root->set_use_collision(true);
	root->update_shape();
	// Drain the output update queued by set_use_collision() before counters are
	// reset for scheduler assertions.
	MessageQueue::get_singleton()->flush();
	return root;
}
#endif // PHYSICS_3D_DISABLED

static Ref<ArrayMesh> _make_two_surface_box_mesh() {
	Ref<BoxMesh> box;
	box.instantiate();
	Ref<ArrayMesh> mesh;
	mesh.instantiate();

	for (int surface_i = 0; surface_i < 2; surface_i++) {
		Array arrays;
		arrays.resize(Mesh::ARRAY_MAX);
		box->create_mesh_array(arrays, Vector3(1, 1, 1));
		Vector<Vector3> vertices = arrays[Mesh::ARRAY_VERTEX];
		for (Vector3 &vertex : vertices) {
			vertex.x += surface_i == 0 ? -1.0f : 1.0f;
		}
		arrays[Mesh::ARRAY_VERTEX] = vertices;
		mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
	}

	return mesh;
}

static bool _get_surface_arrays_for_material(const Ref<ArrayMesh> &p_mesh, const Ref<Material> &p_material, PackedVector3Array &r_vertices, PackedVector2Array &r_uvs) {
	if (p_mesh.is_null()) {
		return false;
	}
	for (int surface_i = 0; surface_i < p_mesh->get_surface_count(); surface_i++) {
		if (p_mesh->surface_get_material(surface_i).ptr() != p_material.ptr()) {
			continue;
		}
		const Array arrays = p_mesh->surface_get_arrays(surface_i);
		r_vertices = arrays[Mesh::ARRAY_VERTEX];
		r_uvs = arrays[Mesh::ARRAY_TEX_UV];
		return true;
	}
	return false;
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
	// Phase 1 retains each leaf and composes only the three mixed-operation groups.
	CHECK_EQ(counters.local_primitive_brush_packs, 3);
	CHECK_EQ(counters.leaf_manifold_repacks, 3);
	CHECK_EQ(counters.transformed_wrapper_constructions, 3);
	CHECK_EQ(counters.expression_node_reconstructions, 4);
	CHECK_EQ(counters.batch_boolean_calls, 3);
	CHECK_EQ(counters.operation_switch_flushes, 2);
	CHECK_EQ(counters.root_materializations, 1);
	CHECK_EQ(counters.non_root_materializations, 0);
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
	CHECK_EQ(counters.leaf_manifold_repacks, 1);
	CHECK_EQ(counters.transformed_wrapper_constructions, 2);
	CHECK_EQ(counters.expression_node_reconstructions, 3);
	CHECK_EQ(counters.batch_boolean_calls, 2);
	CHECK_EQ(counters.operation_switch_flushes, 0);
	CHECK_EQ(counters.root_materializations, 1);
	CHECK_EQ(counters.non_root_materializations, 0);
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

TEST_CASE("[SceneTree][CSG] Phase 1 transform resize and clean rebuild invalidation") {
	CSGCombiner3D *root = memnew(CSGCombiner3D);
	CSGBox3D *left = _add_box(root, Vector3(1, 1, 1), Vector3(-1, 0, 0));
	_add_box(root, Vector3(1, 1, 1), Vector3(1, 0, 0));
	SceneTree::get_singleton()->get_root()->add_child(root);
	CHECK_FALSE(root->get_brush_faces().is_empty());

	_reset_csg_counters();
	left->set_position(Vector3(-2, 0, 0));
	CHECK_FALSE(root->get_brush_faces().is_empty());
#ifdef DEV_ENABLED
	CSGDebugCounters counters = CSGDebugCounters::get();
	CHECK_EQ(counters.local_primitive_brush_packs, 0);
	CHECK_EQ(counters.leaf_manifold_repacks, 0);
	CHECK_EQ(counters.transformed_wrapper_constructions, 1);
	CHECK_EQ(counters.expression_node_reconstructions, 1);
	CHECK_EQ(counters.batch_boolean_calls, 1);
	CHECK_EQ(counters.root_materializations, 1);
	CHECK_EQ(counters.non_root_materializations, 0);
#endif // DEV_ENABLED

	_reset_csg_counters();
	left->set_size(Vector3(2, 1, 1));
	CHECK_FALSE(root->get_brush_faces().is_empty());
#ifdef DEV_ENABLED
	counters = CSGDebugCounters::get();
	CHECK_EQ(counters.local_primitive_brush_packs, 1);
	CHECK_EQ(counters.leaf_manifold_repacks, 1);
	CHECK_EQ(counters.transformed_wrapper_constructions, 1);
	CHECK_EQ(counters.expression_node_reconstructions, 2);
	CHECK_EQ(counters.batch_boolean_calls, 1);
	CHECK_EQ(counters.root_materializations, 1);
	CHECK_EQ(counters.non_root_materializations, 0);
#endif // DEV_ENABLED

	_reset_csg_counters();
	root->update_shape();
#ifdef DEV_ENABLED
	counters = CSGDebugCounters::get();
	CHECK_EQ(counters.local_primitive_brush_packs, 0);
	CHECK_EQ(counters.leaf_manifold_repacks, 0);
	CHECK_EQ(counters.transformed_wrapper_constructions, 0);
	CHECK_EQ(counters.expression_node_reconstructions, 0);
	CHECK_EQ(counters.batch_boolean_calls, 0);
	CHECK_EQ(counters.root_materializations, 0);
	CHECK_EQ(counters.non_root_materializations, 0);
#endif // DEV_ENABLED

	root->queue_free();
}

TEST_CASE("[SceneTree][CSG] Phase 1 deep change retains clean subtree expressions") {
	CSGCombiner3D *root = memnew(CSGCombiner3D);
	CSGCombiner3D *clean_branch = memnew(CSGCombiner3D);
	root->add_child(clean_branch);
	_add_box(clean_branch, Vector3(1, 1, 1), Vector3(-2, 0, 0));

	CSGCombiner3D *changed_outer = memnew(CSGCombiner3D);
	root->add_child(changed_outer);
	CSGCombiner3D *changed_inner = memnew(CSGCombiner3D);
	changed_outer->add_child(changed_inner);
	CSGBox3D *changed_leaf = _add_box(changed_inner, Vector3(1, 1, 1), Vector3(2, 0, 0));
	SceneTree::get_singleton()->get_root()->add_child(root);
	CHECK_FALSE(root->get_brush_faces().is_empty());

	_reset_csg_counters();
	changed_leaf->set_size(Vector3(2, 1, 1));
	CHECK_FALSE(root->get_brush_faces().is_empty());
#ifdef DEV_ENABLED
	CSGDebugCounters counters = CSGDebugCounters::get();
	CHECK_EQ(counters.local_primitive_brush_packs, 1);
	CHECK_EQ(counters.leaf_manifold_repacks, 1);
	CHECK_EQ(counters.transformed_wrapper_constructions, 3);
	// Changed leaf + its two authored combiners + root; the clean branch is retained.
	CHECK_EQ(counters.expression_node_reconstructions, 4);
	CHECK_EQ(counters.batch_boolean_calls, 3);
	CHECK_EQ(counters.root_materializations, 1);
	CHECK_EQ(counters.non_root_materializations, 0);
#endif // DEV_ENABLED

	root->queue_free();
}

TEST_CASE("[SceneTree][CSG] Phase 1 material changes retain the root expression") {
	Ref<StandardMaterial3D> material_a;
	material_a.instantiate();
	Ref<StandardMaterial3D> material_b;
	material_b.instantiate();

	CSGCombiner3D *root = memnew(CSGCombiner3D);
	CSGBox3D *box = _add_box(root, Vector3(1, 1, 1), Vector3(), CSGShape3D::OPERATION_UNION, material_a);
	SceneTree::get_singleton()->get_root()->add_child(root);
	root->update_shape();

	_reset_csg_counters();
	box->set_material(material_b);
	root->update_shape();
	Ref<ArrayMesh> mesh = root->bake_static_mesh();
	REQUIRE(mesh.is_valid());
	REQUIRE_EQ(mesh->get_surface_count(), 1);
	CHECK_EQ(mesh->surface_get_material(0).ptr(), material_b.ptr());
#ifdef DEV_ENABLED
	CSGDebugCounters counters = CSGDebugCounters::get();
	CHECK_EQ(counters.local_primitive_brush_packs, 0);
	CHECK_EQ(counters.leaf_manifold_repacks, 0);
	CHECK_EQ(counters.transformed_wrapper_constructions, 0);
	CHECK_EQ(counters.expression_node_reconstructions, 0);
	CHECK_EQ(counters.batch_boolean_calls, 0);
	CHECK_EQ(counters.root_materializations, 1);
	CHECK_EQ(counters.non_root_materializations, 0);
#endif // DEV_ENABLED

	root->queue_free();
}

TEST_CASE("[SceneTree][CSG] Phase 1 mesh material override reuses source IDs") {
	Ref<StandardMaterial3D> source_material;
	source_material.instantiate();
	Ref<StandardMaterial3D> material_override;
	material_override.instantiate();
	Ref<BoxMesh> source_mesh;
	source_mesh.instantiate();
	source_mesh->set_material(source_material);

	CSGMesh3D *mesh_shape = memnew(CSGMesh3D);
	mesh_shape->set_mesh(source_mesh);
	mesh_shape->set_material(material_override);
	SceneTree::get_singleton()->get_root()->add_child(mesh_shape);
	mesh_shape->update_shape();
	Ref<ArrayMesh> mesh = mesh_shape->bake_static_mesh();
	REQUIRE(mesh.is_valid());
	REQUIRE_EQ(mesh->get_surface_count(), 1);
	CHECK_EQ(mesh->surface_get_material(0).ptr(), material_override.ptr());

	_reset_csg_counters();
	mesh_shape->set_material(Ref<Material>());
	mesh_shape->update_shape();
	mesh = mesh_shape->bake_static_mesh();
	REQUIRE(mesh.is_valid());
	REQUIRE_EQ(mesh->get_surface_count(), 1);
	CHECK_EQ(mesh->surface_get_material(0).ptr(), source_material.ptr());
#ifdef DEV_ENABLED
	CSGDebugCounters counters = CSGDebugCounters::get();
	CHECK_EQ(counters.local_primitive_brush_packs, 0);
	CHECK_EQ(counters.leaf_manifold_repacks, 0);
	CHECK_EQ(counters.transformed_wrapper_constructions, 0);
	CHECK_EQ(counters.expression_node_reconstructions, 0);
	CHECK_EQ(counters.batch_boolean_calls, 0);
	CHECK_EQ(counters.root_materializations, 1);
#endif // DEV_ENABLED

	mesh_shape->queue_free();
}

TEST_CASE("[SceneTree][CSG] Phase 1 non-root brush materialization is explicit") {
	CSGCombiner3D *root = memnew(CSGCombiner3D);
	CSGCombiner3D *nested = memnew(CSGCombiner3D);
	root->add_child(nested);
	_add_box(nested, Vector3(1, 2, 3), Vector3());
	SceneTree::get_singleton()->get_root()->add_child(root);
	CHECK_FALSE(root->get_brush_faces().is_empty());

	_reset_csg_counters();
	CHECK_EQ(nested->get_brush_faces().size(), 36);
#ifdef DEV_ENABLED
	CSGDebugCounters counters = CSGDebugCounters::get();
	CHECK_EQ(counters.local_primitive_brush_packs, 0);
	CHECK_EQ(counters.leaf_manifold_repacks, 0);
	CHECK_EQ(counters.expression_node_reconstructions, 0);
	CHECK_EQ(counters.batch_boolean_calls, 0);
	CHECK_EQ(counters.root_materializations, 0);
	CHECK_EQ(counters.non_root_materializations, 1);
#endif // DEV_ENABLED

	_reset_csg_counters();
	CHECK_EQ(nested->get_brush_faces().size(), 36);
#ifdef DEV_ENABLED
	counters = CSGDebugCounters::get();
	CHECK_EQ(counters.non_root_materializations, 0);
#endif // DEV_ENABLED

	root->queue_free();
}

TEST_CASE("[SceneTree][CSG] Phase 1 operation order and visibility reuse clean leaves") {
	CSGCombiner3D *root = memnew(CSGCombiner3D);
	_add_box(root, Vector3(3, 3, 3), Vector3());
	CSGBox3D *operand = _add_box(root, Vector3(1, 1, 1), Vector3());
	SceneTree::get_singleton()->get_root()->add_child(root);
	CHECK_FALSE(root->get_brush_faces().is_empty());

	_reset_csg_counters();
	operand->set_operation(CSGShape3D::OPERATION_SUBTRACTION);
	CHECK_FALSE(root->get_brush_faces().is_empty());
#ifdef DEV_ENABLED
	CSGDebugCounters counters = CSGDebugCounters::get();
	CHECK_EQ(counters.leaf_manifold_repacks, 0);
	CHECK_EQ(counters.transformed_wrapper_constructions, 0);
	CHECK_EQ(counters.expression_node_reconstructions, 1);
	CHECK_EQ(counters.root_materializations, 1);
#endif // DEV_ENABLED

	_reset_csg_counters();
	root->move_child(operand, 0);
	CHECK_FALSE(root->get_brush_faces().is_empty());
#ifdef DEV_ENABLED
	counters = CSGDebugCounters::get();
	CHECK_EQ(counters.leaf_manifold_repacks, 0);
	CHECK_EQ(counters.transformed_wrapper_constructions, 0);
	CHECK_EQ(counters.expression_node_reconstructions, 1);
	CHECK_EQ(counters.root_materializations, 1);
#endif // DEV_ENABLED

	_reset_csg_counters();
	operand->hide();
	CHECK_FALSE(root->get_brush_faces().is_empty());
#ifdef DEV_ENABLED
	counters = CSGDebugCounters::get();
	CHECK_EQ(counters.leaf_manifold_repacks, 0);
	CHECK_EQ(counters.transformed_wrapper_constructions, 0);
	CHECK_EQ(counters.expression_node_reconstructions, 1);
	CHECK_EQ(counters.root_materializations, 1);
#endif // DEV_ENABLED

	root->queue_free();
}

TEST_CASE("[SceneTree][CSG] Phase 2 boolean surface provenance") {
	auto check_operation = [](CSGShape3D::Operation p_operation, const Vector3 &p_first_size, const Vector3 &p_first_position, const Vector3 &p_second_size, const Vector3 &p_second_position) {
		CSGCombiner3D *root = memnew(CSGCombiner3D);
		CSGBox3D *first = _add_box(root, p_first_size, p_first_position);
		CSGBox3D *second = _add_box(root, p_second_size, p_second_position, p_operation);
		SceneTree::get_singleton()->get_root()->add_child(root);

		Vector<Vector3> faces = root->get_brush_faces();
		REQUIRE(!faces.is_empty());
		REQUIRE_EQ(root->get_result_triangle_count(), (uint32_t)(faces.size() / 3));
		const uint64_t generation = root->get_result_generation();
		bool saw_first = false;
		bool saw_second = false;
		for (uint32_t triangle_i = 0; triangle_i < root->get_result_triangle_count(); triangle_i++) {
			CSGSurfaceKey surface;
			uint32_t face_id = 0;
			CSGOriginToken token = 0;
			REQUIRE(root->resolve_result_triangle(triangle_i, generation, surface, face_id, &token));
			CHECK(CSGShape3D::is_surface_key_valid(surface));
			CHECK(surface.semantic_surface < CSGBox3D::SURFACE_COUNT);
			CHECK_EQ(face_id, surface.semantic_surface);

			CSGOriginToken source_token = 0;
			if (surface.source_shape == first->get_instance_id()) {
				saw_first = true;
				REQUIRE(first->get_surface_origin_token(surface.semantic_surface, source_token));
			} else if (surface.source_shape == second->get_instance_id()) {
				saw_second = true;
				REQUIRE(second->get_surface_origin_token(surface.semantic_surface, source_token));
			} else {
				FAIL_CHECK("Boolean output lost its source CSG node.");
			}
			CHECK_EQ(token, source_token);
		}
		CHECK(saw_first);
		CHECK(saw_second);

		CSGSurfaceKey stale_surface;
		uint32_t stale_face_id = 0;
		CHECK_FALSE(root->resolve_result_triangle(0, generation + 1, stale_surface, stale_face_id));
		root->queue_free();
	};

	SUBCASE("Union") {
		check_operation(CSGShape3D::OPERATION_UNION, Vector3(1, 1, 1), Vector3(-1, 0, 0), Vector3(1, 1, 1), Vector3(1, 0, 0));
	}
	SUBCASE("Subtraction") {
		check_operation(CSGShape3D::OPERATION_SUBTRACTION, Vector3(4, 4, 4), Vector3(), Vector3(2, 2, 2), Vector3());
	}
	SUBCASE("Intersection") {
		check_operation(CSGShape3D::OPERATION_INTERSECTION, Vector3(3, 3, 3), Vector3(-0.5, -0.5, -0.5), Vector3(3, 3, 3), Vector3(0.5, 0.5, 0.5));
	}
}

TEST_CASE("[SceneTree][CSG] Phase 2 nested transform provenance") {
	CSGCombiner3D *root = memnew(CSGCombiner3D);
	CSGCombiner3D *nested = memnew(CSGCombiner3D);
	root->add_child(nested);
	CSGBox3D *box = _add_box(nested, Vector3(1, 2, 3), Vector3(0.5, 0, 0));
	SceneTree::get_singleton()->get_root()->add_child(root);

	Vector<CSGOriginToken> tokens;
	tokens.resize(CSGBox3D::SURFACE_COUNT);
	for (uint32_t surface_i = 0; surface_i < CSGBox3D::SURFACE_COUNT; surface_i++) {
		REQUIRE(box->get_surface_origin_token(surface_i, tokens.write[surface_i]));
	}
	CHECK_FALSE(root->get_brush_faces().is_empty());
	const uint64_t first_generation = root->get_result_generation();

	_reset_csg_counters();
	nested->set_transform(Transform3D(Basis(Vector3(0, 1, 0), Math::deg_to_rad(35.0f)), Vector3(2, 1, -3)));
	CHECK_FALSE(root->get_brush_faces().is_empty());
	CHECK(root->get_result_generation() > first_generation);

	for (uint32_t surface_i = 0; surface_i < CSGBox3D::SURFACE_COUNT; surface_i++) {
		CSGOriginToken token = 0;
		REQUIRE(box->get_surface_origin_token(surface_i, token));
		CHECK_EQ(token, tokens[surface_i]);
	}
	const uint64_t generation = root->get_result_generation();
	for (uint32_t triangle_i = 0; triangle_i < root->get_result_triangle_count(); triangle_i++) {
		CSGSurfaceKey surface;
		uint32_t face_id = 0;
		CSGOriginToken token = 0;
		REQUIRE(root->resolve_result_triangle(triangle_i, generation, surface, face_id, &token));
		CHECK_EQ(surface.source_shape, box->get_instance_id());
		CHECK_EQ(face_id, surface.semantic_surface);
		CHECK_EQ(token, tokens[surface.semantic_surface]);
	}

#ifdef DEV_ENABLED
	CSGDebugCounters counters = CSGDebugCounters::get();
	CHECK_EQ(counters.local_primitive_brush_packs, 0);
	CHECK_EQ(counters.leaf_manifold_repacks, 0);
#endif // DEV_ENABLED

	root->queue_free();
}

TEST_CASE("[SceneTree][CSG] Phase 2 geometry stability and schema invalidation") {
	CSGBox3D *box = memnew(CSGBox3D);
	SceneTree::get_singleton()->get_root()->add_child(box);
	CHECK_FALSE(box->get_brush_faces().is_empty());

	CSGSurfaceKey box_key;
	CSGOriginToken box_token = 0;
	REQUIRE(box->get_surface_key(CSGBox3D::SURFACE_POSITIVE_X, box_key));
	REQUIRE(box->get_surface_origin_token(CSGBox3D::SURFACE_POSITIVE_X, box_token));
	box->set_size(Vector3(2, 3, 4));
	CHECK_FALSE(box->get_brush_faces().is_empty());

	CSGSurfaceKey resized_key;
	CSGOriginToken resized_token = 0;
	REQUIRE(box->get_surface_key(CSGBox3D::SURFACE_POSITIVE_X, resized_key));
	REQUIRE(box->get_surface_origin_token(CSGBox3D::SURFACE_POSITIVE_X, resized_token));
	CHECK(box_key == resized_key);
	CHECK_EQ(box_token, resized_token);
	CHECK(CSGShape3D::is_surface_key_valid(box_key));
	box->queue_free();

	Ref<BoxMesh> one_surface_mesh;
	one_surface_mesh.instantiate();
	CSGMesh3D *mesh_shape = memnew(CSGMesh3D);
	mesh_shape->set_mesh(one_surface_mesh);
	SceneTree::get_singleton()->get_root()->add_child(mesh_shape);
	CHECK_FALSE(mesh_shape->get_brush_faces().is_empty());
	REQUIRE_EQ(mesh_shape->get_surface_schema_size(), 1);

	CSGSurfaceKey old_key;
	CSGOriginToken old_token = 0;
	REQUIRE(mesh_shape->get_surface_key(CSGMesh3D::SURFACE_SOURCE_MESH_BASE, old_key));
	REQUIRE(mesh_shape->get_surface_origin_token(CSGMesh3D::SURFACE_SOURCE_MESH_BASE, old_token));
	CHECK(CSGShape3D::is_surface_key_valid(old_key));
	const uint64_t old_result_generation = mesh_shape->get_result_generation();
	const uint32_t old_schema_generation = mesh_shape->get_surface_schema_generation();

	Ref<ArrayMesh> two_surface_mesh = _make_two_surface_box_mesh();
	mesh_shape->set_mesh(two_surface_mesh);
	CHECK_EQ(mesh_shape->get_surface_schema_size(), 2);
	CHECK_EQ(mesh_shape->get_surface_schema_generation(), old_schema_generation + 1);
	CHECK_FALSE(CSGShape3D::is_surface_key_valid(old_key));
	CSGSurfaceKey stale_surface;
	uint32_t stale_face_id = 0;
	CHECK_FALSE(mesh_shape->resolve_result_triangle(0, old_result_generation, stale_surface, stale_face_id));

	CSGOriginToken new_token = 0;
	REQUIRE(mesh_shape->get_surface_origin_token(CSGMesh3D::SURFACE_SOURCE_MESH_BASE, new_token));
	CHECK_NE(new_token, old_token);
	CHECK_FALSE(mesh_shape->get_brush_faces().is_empty());
	bool saw_surface_0 = false;
	bool saw_surface_1 = false;
	const uint64_t new_result_generation = mesh_shape->get_result_generation();
	for (uint32_t triangle_i = 0; triangle_i < mesh_shape->get_result_triangle_count(); triangle_i++) {
		CSGSurfaceKey surface;
		uint32_t face_id = 0;
		REQUIRE(mesh_shape->resolve_result_triangle(triangle_i, new_result_generation, surface, face_id));
		CHECK_EQ(surface.source_shape, mesh_shape->get_instance_id());
		CHECK_EQ(surface.schema_generation, mesh_shape->get_surface_schema_generation());
		saw_surface_0 |= surface.semantic_surface == 0;
		saw_surface_1 |= surface.semantic_surface == 1;
	}
	CHECK(saw_surface_0);
	CHECK(saw_surface_1);
	mesh_shape->queue_free();

	CSGBox3D *deleted_box = memnew(CSGBox3D);
	CSGSurfaceKey deleted_key;
	REQUIRE(deleted_box->get_surface_key(CSGBox3D::SURFACE_POSITIVE_X, deleted_key));
	CHECK(CSGShape3D::is_surface_key_valid(deleted_key));
	memdelete(deleted_box);
	CHECK_FALSE(CSGShape3D::is_surface_key_valid(deleted_key));
}

TEST_CASE("[SceneTree][CSG] Phase 2 cylinder side facet IDs") {
	CSGCylinder3D *cylinder = memnew(CSGCylinder3D);
	cylinder->set_sides(12);
	SceneTree::get_singleton()->get_root()->add_child(cylinder);
	CHECK_FALSE(cylinder->get_brush_faces().is_empty());

	HashSet<uint32_t> side_face_ids;
	bool saw_top = false;
	bool saw_bottom = false;
	const uint64_t generation = cylinder->get_result_generation();
	for (uint32_t triangle_i = 0; triangle_i < cylinder->get_result_triangle_count(); triangle_i++) {
		CSGSurfaceKey surface;
		uint32_t face_id = 0;
		CSGOriginToken token = 0;
		REQUIRE(cylinder->resolve_result_triangle(triangle_i, generation, surface, face_id, &token));
		CHECK_EQ(surface.source_shape, cylinder->get_instance_id());
		CSGOriginToken source_token = 0;
		REQUIRE(cylinder->get_surface_origin_token(surface.semantic_surface, source_token));
		CHECK_EQ(token, source_token);
		if (surface.semantic_surface == CSGCylinder3D::SURFACE_SIDE) {
			side_face_ids.insert(face_id);
		} else if (surface.semantic_surface == CSGCylinder3D::SURFACE_TOP) {
			saw_top = true;
		} else if (surface.semantic_surface == CSGCylinder3D::SURFACE_BOTTOM) {
			saw_bottom = true;
		}
	}
	CHECK(side_face_ids.size() > 1);
	CHECK(saw_top);
	CHECK(saw_bottom);
	cylinder->queue_free();
}

TEST_CASE("[SceneTree][CSG] Phase 2 provenance material resolution avoids boolean repacking") {
	Ref<StandardMaterial3D> material_a;
	material_a.instantiate();
	Ref<StandardMaterial3D> material_b;
	material_b.instantiate();
	Ref<StandardMaterial3D> material_c;
	material_c.instantiate();

	CSGCombiner3D *root = memnew(CSGCombiner3D);
	CSGBox3D *left = _add_box(root, Vector3(1, 1, 1), Vector3(-1, 0, 0), CSGShape3D::OPERATION_UNION, material_a);
	_add_box(root, Vector3(1, 1, 1), Vector3(1, 0, 0), CSGShape3D::OPERATION_UNION, material_b);
	SceneTree::get_singleton()->get_root()->add_child(root);
	root->update_shape();
	CSGOriginToken token_before = 0;
	REQUIRE(left->get_surface_origin_token(CSGBox3D::SURFACE_POSITIVE_X, token_before));

	_reset_csg_counters();
	left->set_material(material_c);
	root->update_shape();
	CSGOriginToken token_after = 0;
	REQUIRE(left->get_surface_origin_token(CSGBox3D::SURFACE_POSITIVE_X, token_after));
	CHECK_EQ(token_before, token_after);

	Ref<ArrayMesh> mesh = root->bake_static_mesh();
	REQUIRE(mesh.is_valid());
	REQUIRE_EQ(mesh->get_surface_count(), 2);
	bool found_material_b = false;
	bool found_material_c = false;
	for (int surface_i = 0; surface_i < mesh->get_surface_count(); surface_i++) {
		Ref<Material> surface_material = mesh->surface_get_material(surface_i);
		found_material_b |= surface_material.ptr() == material_b.ptr();
		found_material_c |= surface_material.ptr() == material_c.ptr();
	}
	CHECK(found_material_b);
	CHECK(found_material_c);

	const uint64_t generation = root->get_result_generation();
	for (uint32_t triangle_i = 0; triangle_i < root->get_result_triangle_count(); triangle_i++) {
		CSGSurfaceKey surface;
		uint32_t face_id = 0;
		REQUIRE(root->resolve_result_triangle(triangle_i, generation, surface, face_id));
	}

#ifdef DEV_ENABLED
	CSGDebugCounters counters = CSGDebugCounters::get();
	CHECK_EQ(counters.local_primitive_brush_packs, 0);
	CHECK_EQ(counters.leaf_manifold_repacks, 0);
	CHECK_EQ(counters.transformed_wrapper_constructions, 0);
	CHECK_EQ(counters.expression_node_reconstructions, 0);
	CHECK_EQ(counters.batch_boolean_calls, 0);
	CHECK_EQ(counters.root_materializations, 1);
#endif // DEV_ENABLED

	root->queue_free();
}

#ifndef PHYSICS_3D_DISABLED
TEST_CASE("[SceneTree][CSG] Phase 4 scheduler coalesces with final quality") {
	CSGSynchronousSchedulerScope force_sync;
	CSGBox3D *root = _make_collision_box_root();
	const uint64_t generation_before = root->get_result_generation();

	_reset_csg_counters();
	root->request_async_evaluation(CSGEvalQuality::INTERACTIVE);
	root->request_async_evaluation(CSGEvalQuality::FINAL);
#ifdef DEV_ENABLED
	CSGDebugCounters before_landing = CSGDebugCounters::get();
	CHECK_EQ(before_landing.scheduler_requests, 2);
	CHECK_EQ(before_landing.scheduler_completions, 0);
	CHECK_EQ(before_landing.scheduler_coalesces, 1);
#endif // DEV_ENABLED

	MessageQueue::get_singleton()->flush();
	CHECK_EQ(root->get_result_generation(), generation_before + 1);
	Ref<ConcavePolygonShape3D> collision = root->bake_collision_shape();
	REQUIRE(collision.is_valid());
	CHECK_EQ(collision->get_faces().size(), 36);
#ifdef DEV_ENABLED
	CSGDebugCounters counters = CSGDebugCounters::get();
	CHECK_EQ(counters.scheduler_requests, 2);
	CHECK_EQ(counters.scheduler_completions, 2);
	CHECK_EQ(counters.scheduler_coalesces, 1);
	CHECK_EQ(counters.scheduler_stale_drops, 1);
	CHECK_EQ(counters.collision_rebuilds, 1);
#endif // DEV_ENABLED

	root->queue_free();
	MessageQueue::get_singleton()->flush();
}

TEST_CASE("[SceneTree][CSG] Phase 4 interactive skips collision and tangents") {
	CSGSynchronousSchedulerScope force_sync;
	CSGBox3D *root = _make_collision_box_root();
	Ref<ConcavePolygonShape3D> collision_before = root->bake_collision_shape();
	REQUIRE(collision_before.is_valid());
	const Vector<Vector3> faces_before = collision_before->get_faces();

	_reset_csg_counters();
	root->request_async_evaluation(CSGEvalQuality::INTERACTIVE);
	MessageQueue::get_singleton()->flush();
	Ref<ConcavePolygonShape3D> collision_interactive = root->bake_collision_shape();
	REQUIRE(collision_interactive.is_valid());
	CHECK_EQ(collision_interactive->get_faces(), faces_before);
#ifdef DEV_ENABLED
	CSGDebugCounters interactive_counters = CSGDebugCounters::get();
	CHECK_EQ(interactive_counters.collision_rebuilds, 0);
	CHECK_EQ(interactive_counters.tangent_finalizations, 0);
	CHECK_EQ(interactive_counters.scheduler_completions, 1);
#endif // DEV_ENABLED

	root->request_async_evaluation(CSGEvalQuality::FINAL);
	MessageQueue::get_singleton()->flush();
	Ref<ConcavePolygonShape3D> collision_final = root->bake_collision_shape();
	REQUIRE(collision_final.is_valid());
	CHECK_EQ(collision_final->get_faces(), faces_before);
#ifdef DEV_ENABLED
	CSGDebugCounters final_counters = CSGDebugCounters::get();
	CHECK_EQ(final_counters.collision_rebuilds, 1);
	CHECK_EQ(final_counters.tangent_finalizations, 1);
	CHECK_EQ(final_counters.scheduler_completions, 2);
#endif // DEV_ENABLED

	root->queue_free();
	MessageQueue::get_singleton()->flush();
}

TEST_CASE("[SceneTree][CSG] Phase 4 scheduler publishes only the latest of 100 requests") {
	CSGSynchronousSchedulerScope force_sync;
	CSGBox3D *root = _make_collision_box_root();
	const uint64_t generation_before = root->get_result_generation();

	_reset_csg_counters();
	for (int request_i = 0; request_i < 100; request_i++) {
		const CSGEvalQuality quality = request_i == 50 ? CSGEvalQuality::FINAL : CSGEvalQuality::INTERACTIVE;
		root->request_async_evaluation(quality);
	}
	MessageQueue::get_singleton()->flush();
	CHECK_EQ(root->get_result_generation(), generation_before + 1);
#ifdef DEV_ENABLED
	CSGDebugCounters counters = CSGDebugCounters::get();
	CHECK_EQ(counters.scheduler_requests, 100);
	CHECK_EQ(counters.scheduler_completions, 2);
	CHECK_EQ(counters.scheduler_coalesces, 99);
	CHECK_EQ(counters.scheduler_stale_drops, 1);
	// The FINAL request cannot be downgraded by the 49 newer interactive ones.
	CHECK_EQ(counters.collision_rebuilds, 1);
#endif // DEV_ENABLED

	root->queue_free();
	MessageQueue::get_singleton()->flush();
}
#endif // PHYSICS_3D_DISABLED

TEST_CASE("[SceneTree][CSG] Phase 4 synchronous update rejects an in-flight snapshot") {
	CSGSynchronousSchedulerScope force_sync;
	CSGBox3D *root = memnew(CSGBox3D);
	SceneTree::get_singleton()->get_root()->add_child(root);
	root->update_shape();
	MessageQueue::get_singleton()->flush();

	_reset_csg_counters();
	root->request_async_evaluation(CSGEvalQuality::FINAL);
	root->set_size(Vector3(2, 3, 4));
	root->update_shape();
	const uint64_t synchronous_generation = root->get_result_generation();
	MessageQueue::get_singleton()->flush();
	CHECK_EQ(root->get_result_generation(), synchronous_generation);
	CHECK(root->get_aabb().is_equal_approx(AABB(Vector3(-1, -1.5, -2), Vector3(2, 3, 4))));
#ifdef DEV_ENABLED
	CSGDebugCounters counters = CSGDebugCounters::get();
	CHECK_EQ(counters.scheduler_requests, 1);
	CHECK_EQ(counters.scheduler_completions, 1);
	CHECK_EQ(counters.scheduler_stale_drops, 1);
#endif // DEV_ENABLED

	root->queue_free();
	MessageQueue::get_singleton()->flush();
}

TEST_CASE("[SceneTree][CSG] Phase 4 async final supersedes its queued synchronous update") {
	CSGSynchronousSchedulerScope force_sync;
	CSGBox3D *root = memnew(CSGBox3D);
	SceneTree::get_singleton()->get_root()->add_child(root);
	root->update_shape();
	MessageQueue::get_singleton()->flush();
	const uint64_t generation_before = root->get_result_generation();

	_reset_csg_counters();
	root->set_size(Vector3(2, 3, 4));
	root->request_final_async_evaluation();
	MessageQueue::get_singleton()->flush();
	CHECK_EQ(root->get_result_generation(), generation_before + 1);
	CHECK(root->get_aabb().is_equal_approx(AABB(Vector3(-1, -1.5, -2), Vector3(2, 3, 4))));
#ifdef DEV_ENABLED
	CSGDebugCounters counters = CSGDebugCounters::get();
	CHECK_EQ(counters.scheduler_requests, 1);
	CHECK_EQ(counters.scheduler_completions, 1);
	CHECK_EQ(counters.scheduler_stale_drops, 0);
	CHECK_EQ(counters.root_materializations, 0);
	CHECK_EQ(counters.uv_finalizations, 0);
#endif // DEV_ENABLED

	root->queue_free();
	MessageQueue::get_singleton()->flush();
}

TEST_CASE("[SceneTree][CSG] Phase 4 model edit after async request is not wrongly suppressed") {
	CSGSynchronousSchedulerScope force_sync;
	CSGBox3D *root = memnew(CSGBox3D);
	SceneTree::get_singleton()->get_root()->add_child(root);
	root->update_shape();
	MessageQueue::get_singleton()->flush();
	// A prior async cycle creates the scheduler so later edits invalidate it.
	root->request_async_evaluation(CSGEvalQuality::FINAL);
	MessageQueue::get_singleton()->flush();

	_reset_csg_counters();
	// A queued (unflushed) synchronous update exists when the async request
	// captures its suppression count.
	root->set_size(Vector3(2, 3, 4));
	root->request_final_async_evaluation();
	// A second model edit invalidates the in-flight async; its queued synchronous
	// update must still run so the newest size is not lost to stale geometry.
	root->set_size(Vector3(5, 6, 7));
	MessageQueue::get_singleton()->flush();
	CHECK(root->get_aabb().is_equal_approx(AABB(Vector3(-2.5, -3, -3.5), Vector3(5, 6, 7))));
#ifdef DEV_ENABLED
	CSGDebugCounters counters = CSGDebugCounters::get();
	CHECK_EQ(counters.scheduler_stale_drops, 1);
#endif // DEV_ENABLED

	root->queue_free();
	MessageQueue::get_singleton()->flush();
}

TEST_CASE("[SceneTree][CSG] Phase 4 root deletion flushes queued evaluation") {
	CSGSynchronousSchedulerScope force_sync;
	CSGBox3D *root = memnew(CSGBox3D);

	_reset_csg_counters();
	root->request_async_evaluation(CSGEvalQuality::FINAL);
	memdelete(root);
	MessageQueue::get_singleton()->flush();
#ifdef DEV_ENABLED
	CSGDebugCounters counters = CSGDebugCounters::get();
	CHECK_EQ(counters.scheduler_requests, 1);
	CHECK_EQ(counters.scheduler_completions, 0);
	CHECK_EQ(counters.scheduler_stale_drops, 0);
#endif // DEV_ENABLED
}

// CSG-5: A newly parented extrusion operand publishes selectable cap provenance.
TEST_CASE("[SceneTree][CSG] Phase 5 new union child receives cap provenance") {
	CSGSynchronousSchedulerScope force_sync;
	CSGBox3D *root = memnew(CSGBox3D);
	SceneTree::get_singleton()->get_root()->add_child(root);
	root->update_shape();
	MessageQueue::get_singleton()->flush();
	const uint64_t generation_before = root->get_result_generation();

	CSGBox3D *child = _add_box(root, Vector3(2, 2, 2), Vector3(2, 0, 0), CSGShape3D::OPERATION_UNION);
	const ObjectID child_id = child->get_instance_id();
	root->update_shape();
	CHECK(root->get_result_generation() > generation_before);

	bool found_child_cap = false;
	const uint64_t generation = root->get_result_generation();
	for (uint32_t triangle_i = 0; triangle_i < root->get_result_triangle_count(); triangle_i++) {
		CSGSurfaceKey surface;
		uint32_t face_id = 0;
		if (root->resolve_result_triangle(triangle_i, generation, surface, face_id) && surface.source_shape == child_id && surface.semantic_surface == CSGBox3D::SURFACE_POSITIVE_X) {
			found_child_cap = true;
			break;
		}
	}
	CHECK(found_child_cap);

	root->queue_free();
	MessageQueue::get_singleton()->flush();
}

TEST_CASE("[SceneTree][CSG] Phase 6 sparse surface settings are inert and default-eliding") {
	CSGBox3D *box = memnew(CSGBox3D);
	const uint32_t schema_generation = box->get_surface_schema_generation();
	const uint32_t surface = CSGBox3D::SURFACE_POSITIVE_X;

	CHECK_FALSE(box->has_surface_setting(surface));
	box->set_surface_setting(surface, CSGSurfaceSetting());
	CHECK_FALSE(box->has_surface_setting(surface));

	box->set_surface_offset(surface, Vector2(0.25, -0.5));
	REQUIRE(box->has_surface_setting(surface));
	CHECK_EQ(box->get_surface_setting(surface).offset, Vector2(0.25, -0.5));
	CHECK_EQ(box->get_surface_schema_generation(), schema_generation);

	List<PropertyInfo> properties;
	box->get_property_list(&properties);
	bool found_material = false;
	bool found_uv_mode = false;
	bool found_hidden_stored_offset = false;
	for (const PropertyInfo &raw_property : properties) {
		PropertyInfo property = raw_property;
		box->validate_property(property);
		if (property.name == "surface_settings/0/material") {
			found_material = true;
			CHECK((property.usage & PROPERTY_USAGE_EDITOR) != 0);
			CHECK((property.usage & PROPERTY_USAGE_STORAGE) == 0);
		} else if (property.name == "surface_settings/0/uv_mode") {
			found_uv_mode = true;
			CHECK((property.usage & PROPERTY_USAGE_EDITOR) != 0);
			CHECK((property.usage & PROPERTY_USAGE_STORAGE) == 0);
		} else if (property.name == "surface_settings/0/offset") {
			found_hidden_stored_offset = true;
			CHECK((property.usage & PROPERTY_USAGE_EDITOR) == 0);
			CHECK((property.usage & PROPERTY_USAGE_STORAGE) != 0);
		}
	}
	CHECK(found_material);
	CHECK(found_uv_mode);
	CHECK(found_hidden_stored_offset);
	CHECK(box->property_can_revert("surface_settings/0/offset"));
	CHECK_EQ((Vector2)box->property_get_revert("surface_settings/0/offset"), Vector2());

	box->set_surface_uv_mode(surface, CSGPrimitive3D::SURFACE_UV_MODE_PLANAR);
	properties.clear();
	box->get_property_list(&properties);
	bool found_visible_offset = false;
	for (const PropertyInfo &raw_property : properties) {
		PropertyInfo property = raw_property;
		box->validate_property(property);
		if (property.name == "surface_settings/0/offset") {
			found_visible_offset = true;
			CHECK((property.usage & PROPERTY_USAGE_EDITOR) != 0);
			CHECK((property.usage & PROPERTY_USAGE_STORAGE) != 0);
		}
	}
	CHECK(found_visible_offset);

	box->set_surface_offset(surface, Vector2());
	box->set_surface_uv_mode(surface, CSGPrimitive3D::SURFACE_UV_MODE_LEGACY);
	CHECK_FALSE(box->has_surface_setting(surface));
	CHECK_EQ(box->get_surface_schema_generation(), schema_generation);
	memdelete(box);
}

TEST_CASE("[SceneTree][CSG] Phase 6 surface settings serialize sparsely and round-trip") {
	const String test_output_dir = OS::get_singleton()->get_executable_path().get_base_dir();
	const String untouched_path = test_output_dir.path_join("csg_phase6_untouched.tscn");
	const String authored_path = test_output_dir.path_join("csg_phase6_authored.tscn");
	CSGTemporaryFileCleanup file_cleanup;
	file_cleanup.paths.push_back(untouched_path);
	file_cleanup.paths.push_back(authored_path);
	CSGBox3D *untouched = memnew(CSGBox3D);
	untouched->set_name("UntouchedCSGBox");
	Ref<PackedScene> untouched_scene;
	untouched_scene.instantiate();
	REQUIRE_EQ(untouched_scene->pack(untouched), OK);
	REQUIRE_EQ(ResourceSaver::save(untouched_scene, untouched_path), OK);
	const String untouched_before = FileAccess::get_file_as_string(untouched_path);
	CHECK_FALSE(untouched_before.contains("surface_settings/"));
	CHECK_FALSE(untouched_before.contains("surface_schema_version"));
	memdelete(untouched);

	Error load_error = OK;
	Ref<PackedScene> untouched_loaded = ResourceLoader::load(untouched_path, "PackedScene", ResourceFormatLoader::CacheMode::CACHE_MODE_IGNORE, &load_error);
	REQUIRE_EQ(load_error, OK);
	REQUIRE(untouched_loaded.is_valid());
	if (untouched_loaded.is_null()) {
		return;
	}
	Node *untouched_instance = untouched_loaded->instantiate();
	REQUIRE(untouched_instance != nullptr);
	if (!untouched_instance) {
		return;
	}
	Ref<PackedScene> untouched_repacked;
	untouched_repacked.instantiate();
	REQUIRE_EQ(untouched_repacked->pack(untouched_instance), OK);
	REQUIRE_EQ(ResourceSaver::save(untouched_repacked, untouched_path), OK);
	CHECK_EQ(FileAccess::get_file_as_string(untouched_path), untouched_before);
	memdelete(untouched_instance);

	CSGBox3D *authored = memnew(CSGBox3D);
	authored->set_name("AuthoredCSGBox");
	Ref<StandardMaterial3D> material;
	material.instantiate();
	material->set_albedo(Color(0.15, 0.35, 0.75));

	CSGSurfaceSetting setting;
	setting.material = material;
	setting.uv_mode = CSGPrimitive3D::SURFACE_UV_MODE_PLANAR;
	setting.uv_space = CSGPrimitive3D::SURFACE_UV_SPACE_WORLD;
	setting.meters_per_tile = Vector2(2.5, 3.5);
	setting.offset = Vector2(0.125, -0.75);
	setting.rotation = Math::deg_to_rad(22.5);
	setting.texture_lock = true;
	authored->set_surface_setting(CSGBox3D::SURFACE_NEGATIVE_Z, setting);
	authored->set_surface_material(CSGBox3D::SURFACE_POSITIVE_Y, material);
	authored->set_surface_schema_version(0);

	Ref<PackedScene> authored_scene;
	authored_scene.instantiate();
	REQUIRE_EQ(authored_scene->pack(authored), OK);
	REQUIRE_EQ(ResourceSaver::save(authored_scene, authored_path), OK);
	const String authored_text = FileAccess::get_file_as_string(authored_path);
	CHECK(authored_text.contains("surface_schema_version = 0"));
	CHECK(authored_text.contains("surface_settings/5/material"));
	CHECK(authored_text.contains("surface_settings/5/uv_mode"));
	CHECK(authored_text.contains("surface_settings/5/uv_space"));
	CHECK(authored_text.contains("surface_settings/5/meters_per_tile"));
	CHECK(authored_text.contains("surface_settings/5/offset"));
	CHECK(authored_text.contains("surface_settings/5/rotation"));
	CHECK(authored_text.contains("surface_settings/5/texture_lock"));
	CHECK(authored_text.contains("surface_settings/2/material"));
	CHECK_FALSE(authored_text.contains("surface_settings/2/uv_mode"));
	memdelete(authored);

	Ref<PackedScene> authored_loaded = ResourceLoader::load(authored_path, "PackedScene", ResourceFormatLoader::CacheMode::CACHE_MODE_IGNORE, &load_error);
	REQUIRE_EQ(load_error, OK);
	REQUIRE(authored_loaded.is_valid());
	if (authored_loaded.is_null()) {
		return;
	}
	CSGBox3D *loaded_box = Object::cast_to<CSGBox3D>(authored_loaded->instantiate());
	REQUIRE(loaded_box != nullptr);
	if (!loaded_box) {
		return;
	}
	CHECK_EQ(loaded_box->get_surface_schema_version(), 0);
	REQUIRE(loaded_box->has_surface_setting(CSGBox3D::SURFACE_NEGATIVE_Z));
	const CSGSurfaceSetting loaded_setting = loaded_box->get_surface_setting(CSGBox3D::SURFACE_NEGATIVE_Z);
	REQUIRE(loaded_setting.material.is_valid());
	Ref<StandardMaterial3D> loaded_material = loaded_setting.material;
	REQUIRE(loaded_material.is_valid());
	CHECK_EQ(loaded_material->get_albedo(), material->get_albedo());
	CHECK_EQ(loaded_setting.uv_mode, CSGPrimitive3D::SURFACE_UV_MODE_PLANAR);
	CHECK_EQ(loaded_setting.uv_space, CSGPrimitive3D::SURFACE_UV_SPACE_WORLD);
	CHECK_EQ(loaded_setting.meters_per_tile, setting.meters_per_tile);
	CHECK_EQ(loaded_setting.offset, setting.offset);
	CHECK(loaded_setting.rotation == doctest::Approx(setting.rotation));
	CHECK(loaded_setting.texture_lock);

	CSGBox3D *duplicated_box = Object::cast_to<CSGBox3D>(loaded_box->duplicate());
	REQUIRE(duplicated_box != nullptr);
	REQUIRE(duplicated_box->has_surface_setting(CSGBox3D::SURFACE_NEGATIVE_Z));
	CHECK_EQ(duplicated_box->get_surface_setting(CSGBox3D::SURFACE_NEGATIVE_Z).offset, setting.offset);
	CHECK_EQ(duplicated_box->get_surface_setting(CSGBox3D::SURFACE_POSITIVE_Y).material.ptr(), loaded_box->get_surface_setting(CSGBox3D::SURFACE_POSITIVE_Y).material.ptr());
	memdelete(duplicated_box);
	memdelete(loaded_box);
}

TEST_CASE("[SceneTree][CSG] Phase 6 material resolution is override then node then inherited") {
	Ref<StandardMaterial3D> inherited_material;
	inherited_material.instantiate();
	Ref<StandardMaterial3D> node_material;
	node_material.instantiate();
	Ref<StandardMaterial3D> surface_material;
	surface_material.instantiate();

	CSGBox3D *box = memnew(CSGBox3D);
	box->set_material(inherited_material);
	SceneTree::get_singleton()->get_root()->add_child(box);
	box->update_shape();
	const uint32_t schema_generation = box->get_surface_schema_generation();

	auto mesh_has_material = [](const Ref<ArrayMesh> &p_mesh, const Ref<Material> &p_material) {
		for (int surface_i = 0; surface_i < p_mesh->get_surface_count(); surface_i++) {
			if (p_mesh->surface_get_material(surface_i).ptr() == p_material.ptr()) {
				return true;
			}
		}
		return false;
	};

	_reset_csg_counters();
	box->set_surface_material(CSGBox3D::SURFACE_POSITIVE_X, surface_material);
	box->update_shape();
	Ref<ArrayMesh> mesh = box->bake_static_mesh();
	REQUIRE(mesh.is_valid());
	CHECK(mesh_has_material(mesh, surface_material));
	CHECK(mesh_has_material(mesh, inherited_material));

	box->set_material(node_material);
	box->update_shape();
	mesh = box->bake_static_mesh();
	REQUIRE(mesh.is_valid());
	CHECK(mesh_has_material(mesh, surface_material));
	CHECK(mesh_has_material(mesh, node_material));
	CHECK_FALSE(mesh_has_material(mesh, inherited_material));

	box->clear_surface_setting(CSGBox3D::SURFACE_POSITIVE_X);
	box->update_shape();
	mesh = box->bake_static_mesh();
	REQUIRE(mesh.is_valid());
	CHECK_EQ(mesh->get_surface_count(), 1);
	CHECK(mesh_has_material(mesh, node_material));
	CHECK_EQ(box->get_surface_schema_generation(), schema_generation);

	box->set_material(Ref<Material>());
	box->update_shape();
	mesh = box->bake_static_mesh();
	REQUIRE(mesh.is_valid());
	CHECK_EQ(mesh->get_surface_count(), 1);
	CHECK(mesh_has_material(mesh, inherited_material));
	CHECK_EQ(box->get_resolved_surface_material(CSGBox3D::SURFACE_POSITIVE_X).ptr(), inherited_material.ptr());
	CHECK_EQ(box->get_surface_schema_generation(), schema_generation);

#ifdef DEV_ENABLED
	CSGDebugCounters counters = CSGDebugCounters::get();
	CHECK_EQ(counters.local_primitive_brush_packs, 0);
	CHECK_EQ(counters.leaf_manifold_repacks, 0);
	CHECK_EQ(counters.batch_boolean_calls, 0);
#endif // DEV_ENABLED

	box->queue_free();
}

TEST_CASE("[SceneTree][CSG] Phase 6 subtractive cut faces use cutter surface material") {
	Ref<StandardMaterial3D> root_material;
	root_material.instantiate();
	Ref<StandardMaterial3D> cutter_material;
	cutter_material.instantiate();
	Ref<StandardMaterial3D> cut_override;
	cut_override.instantiate();
	Ref<StandardMaterial3D> replacement_override;
	replacement_override.instantiate();

	CSGBox3D *root = memnew(CSGBox3D);
	root->set_size(Vector3(4, 4, 4));
	root->set_material(root_material);
	CSGBox3D *cutter = _add_box(root, Vector3(2, 2, 2), Vector3(), CSGShape3D::OPERATION_SUBTRACTION, cutter_material);
	cutter->set_surface_material(CSGBox3D::SURFACE_POSITIVE_X, cut_override);
	SceneTree::get_singleton()->get_root()->add_child(root);
	root->update_shape();

	bool found_cut_surface_provenance = false;
	const uint64_t generation = root->get_result_generation();
	for (uint32_t triangle_i = 0; triangle_i < root->get_result_triangle_count(); triangle_i++) {
		CSGSurfaceKey surface;
		uint32_t face_id = 0;
		REQUIRE(root->resolve_result_triangle(triangle_i, generation, surface, face_id));
		if (surface.source_shape == cutter->get_instance_id() && surface.semantic_surface == CSGBox3D::SURFACE_POSITIVE_X) {
			found_cut_surface_provenance = true;
		}
	}
	CHECK(found_cut_surface_provenance);

	Ref<ArrayMesh> mesh = root->bake_static_mesh();
	REQUIRE(mesh.is_valid());
	bool found_cut_override = false;
	for (int surface_i = 0; surface_i < mesh->get_surface_count(); surface_i++) {
		found_cut_override |= mesh->surface_get_material(surface_i).ptr() == cut_override.ptr();
	}
	CHECK(found_cut_override);

	const uint32_t schema_generation = cutter->get_surface_schema_generation();
	_reset_csg_counters();
	cutter->set_surface_material(CSGBox3D::SURFACE_POSITIVE_X, replacement_override);
	root->update_shape();
	mesh = root->bake_static_mesh();
	REQUIRE(mesh.is_valid());
	bool found_replacement_override = false;
	for (int surface_i = 0; surface_i < mesh->get_surface_count(); surface_i++) {
		found_replacement_override |= mesh->surface_get_material(surface_i).ptr() == replacement_override.ptr();
	}
	CHECK(found_replacement_override);
	CHECK_EQ(cutter->get_surface_schema_generation(), schema_generation);
#ifdef DEV_ENABLED
	CSGDebugCounters counters = CSGDebugCounters::get();
	CHECK_EQ(counters.batch_boolean_calls, 0);
	CHECK_EQ(counters.root_materializations, 1);
#endif // DEV_ENABLED

	root->queue_free();
}

TEST_CASE("[SceneTree][CSG] Phase 6 planar UVs are deterministic and UV edits avoid booleans") {
	Ref<StandardMaterial3D> node_material;
	node_material.instantiate();
	Ref<StandardMaterial3D> planar_material;
	planar_material.instantiate();

	CSGBox3D *box = memnew(CSGBox3D);
	box->set_size(Vector3(2, 2, 2));
	box->set_material(node_material);
	CSGSurfaceSetting setting;
	setting.material = planar_material;
	setting.uv_mode = CSGPrimitive3D::SURFACE_UV_MODE_PLANAR;
	setting.uv_space = CSGPrimitive3D::SURFACE_UV_SPACE_ROOT;
	setting.meters_per_tile = Vector2(2, 4);
	setting.offset = Vector2(0.25, -0.5);
	box->set_surface_setting(CSGBox3D::SURFACE_POSITIVE_Z, setting);
	SceneTree::get_singleton()->get_root()->add_child(box);
	box->update_shape();

	PackedVector3Array vertices;
	PackedVector2Array uvs;
	REQUIRE(_get_surface_arrays_for_material(box->bake_static_mesh(), planar_material, vertices, uvs));
	REQUIRE_EQ(vertices.size(), uvs.size());
	REQUIRE_EQ(vertices.size(), 6);
	bool differs_from_legacy_corners = false;
	for (int vertex_i = 0; vertex_i < vertices.size(); vertex_i++) {
		const Vector2 expected(vertices[vertex_i].x / 2.0 + 0.25, vertices[vertex_i].y / 4.0 - 0.5);
		CHECK(uvs[vertex_i].is_equal_approx(expected));
		differs_from_legacy_corners |= !uvs[vertex_i].is_equal_approx(Vector2(0, 0)) && !uvs[vertex_i].is_equal_approx(Vector2(0, 1)) && !uvs[vertex_i].is_equal_approx(Vector2(1, 0)) && !uvs[vertex_i].is_equal_approx(Vector2(1, 1));
	}
	CHECK(differs_from_legacy_corners);

	_reset_csg_counters();
	box->set_surface_offset(CSGBox3D::SURFACE_POSITIVE_Z, Vector2(-0.75, 1.25));
	box->update_shape();
	REQUIRE(_get_surface_arrays_for_material(box->bake_static_mesh(), planar_material, vertices, uvs));
	for (int vertex_i = 0; vertex_i < vertices.size(); vertex_i++) {
		const Vector2 expected(vertices[vertex_i].x / 2.0 - 0.75, vertices[vertex_i].y / 4.0 + 1.25);
		CHECK(uvs[vertex_i].is_equal_approx(expected));
	}
#ifdef DEV_ENABLED
	CSGDebugCounters counters = CSGDebugCounters::get();
	CHECK_EQ(counters.local_primitive_brush_packs, 0);
	CHECK_EQ(counters.leaf_manifold_repacks, 0);
	CHECK_EQ(counters.batch_boolean_calls, 0);
	CHECK_EQ(counters.root_materializations, 1);
	CHECK_EQ(counters.uv_finalizations, 1);
	CHECK_EQ(counters.tangent_finalizations, 1);
#endif // DEV_ENABLED

	box->queue_free();
}

TEST_CASE("[SceneTree][CSG] Phase 6 root-space planar mapping stays anchored across operand transforms") {
	Ref<StandardMaterial3D> node_material;
	node_material.instantiate();
	Ref<StandardMaterial3D> planar_material;
	planar_material.instantiate();

	CSGCombiner3D *root = memnew(CSGCombiner3D);
	CSGBox3D *box = _add_box(root, Vector3(2, 2, 2), Vector3(1.25, -0.5, 0.75), CSGShape3D::OPERATION_UNION, node_material);
	box->set_rotation(Vector3(0.2, 0.45, -0.15));
	CSGSurfaceSetting setting;
	setting.material = planar_material;
	setting.uv_mode = CSGPrimitive3D::SURFACE_UV_MODE_PLANAR;
	setting.uv_space = CSGPrimitive3D::SURFACE_UV_SPACE_ROOT;
	setting.meters_per_tile = Vector2(1.5, 2.5);
	setting.offset = Vector2(0.125, -0.375);
	box->set_surface_setting(CSGBox3D::SURFACE_POSITIVE_Z, setting);
	SceneTree::get_singleton()->get_root()->add_child(root);
	root->update_shape();

	auto check_root_mapping = [&](const Ref<ArrayMesh> &p_mesh) {
		PackedVector3Array vertices;
		PackedVector2Array uvs;
		REQUIRE(_get_surface_arrays_for_material(p_mesh, planar_material, vertices, uvs));
		REQUIRE_EQ(vertices.size(), uvs.size());
		REQUIRE_EQ(vertices.size(), 6);
		for (int vertex_i = 0; vertex_i < vertices.size(); vertex_i++) {
			const Vector2 expected(vertices[vertex_i].x / 1.5 + 0.125, vertices[vertex_i].y / 2.5 - 0.375);
			CHECK(uvs[vertex_i].is_equal_approx(expected));
		}
	};

	check_root_mapping(root->bake_static_mesh());
	box->set_position(Vector3(-0.75, 1.1, 0.25));
	root->update_shape();
	check_root_mapping(root->bake_static_mesh());
	root->queue_free();
}

TEST_CASE("[SceneTree][CSG] Phase 6 subtractive cut faces inherit cutter planar UVs") {
	Ref<StandardMaterial3D> root_material;
	root_material.instantiate();
	Ref<StandardMaterial3D> cutter_material;
	cutter_material.instantiate();
	Ref<StandardMaterial3D> cut_material;
	cut_material.instantiate();

	CSGBox3D *root = memnew(CSGBox3D);
	root->set_size(Vector3(4, 4, 4));
	root->set_material(root_material);
	CSGBox3D *cutter = _add_box(root, Vector3(2, 2, 2), Vector3(), CSGShape3D::OPERATION_SUBTRACTION, cutter_material);
	CSGSurfaceSetting setting;
	setting.material = cut_material;
	setting.uv_mode = CSGPrimitive3D::SURFACE_UV_MODE_PLANAR;
	setting.uv_space = CSGPrimitive3D::SURFACE_UV_SPACE_ROOT;
	setting.meters_per_tile = Vector2(2, 3);
	setting.offset = Vector2(0.5, -0.25);
	cutter->set_surface_setting(CSGBox3D::SURFACE_POSITIVE_X, setting);
	SceneTree::get_singleton()->get_root()->add_child(root);
	root->update_shape();

	PackedVector3Array vertices;
	PackedVector2Array uvs;
	REQUIRE(_get_surface_arrays_for_material(root->bake_static_mesh(), cut_material, vertices, uvs));
	REQUIRE_EQ(vertices.size(), uvs.size());
	REQUIRE_EQ(vertices.size(), 6);
	for (int vertex_i = 0; vertex_i < vertices.size(); vertex_i++) {
		const Vector2 expected(-vertices[vertex_i].z / 2.0 + 0.5, vertices[vertex_i].y / 3.0 - 0.25);
		CHECK(uvs[vertex_i].is_equal_approx(expected));
	}
	root->queue_free();
}

TEST_CASE("[SceneTree][CSG] Phase 6 world-space planar UVs refinalize on root transform") {
	Ref<StandardMaterial3D> node_material;
	node_material.instantiate();
	Ref<StandardMaterial3D> planar_material;
	planar_material.instantiate();

	CSGBox3D *box = memnew(CSGBox3D);
	box->set_size(Vector3(2, 2, 2));
	box->set_material(node_material);
	CSGSurfaceSetting setting;
	setting.material = planar_material;
	setting.uv_mode = CSGPrimitive3D::SURFACE_UV_MODE_PLANAR;
	setting.uv_space = CSGPrimitive3D::SURFACE_UV_SPACE_WORLD;
	box->set_surface_setting(CSGBox3D::SURFACE_POSITIVE_Z, setting);
	box->set_position(Vector3(3, -2, 4));
	SceneTree::get_singleton()->get_root()->add_child(box);
	box->update_shape();

	auto check_world_mapping = [&](const Vector3 &p_root_position) {
		PackedVector3Array vertices;
		PackedVector2Array uvs;
		REQUIRE(_get_surface_arrays_for_material(box->bake_static_mesh(), planar_material, vertices, uvs));
		REQUIRE_EQ(vertices.size(), uvs.size());
		for (int vertex_i = 0; vertex_i < vertices.size(); vertex_i++) {
			const Vector2 expected(vertices[vertex_i].x + p_root_position.x, vertices[vertex_i].y + p_root_position.y);
			CHECK(uvs[vertex_i].is_equal_approx(expected));
		}
	};

	check_world_mapping(box->get_position());
	_reset_csg_counters();
	box->set_position(Vector3(-5, 7, 1));
	box->update_shape();
	check_world_mapping(box->get_position());
#ifdef DEV_ENABLED
	CSGDebugCounters counters = CSGDebugCounters::get();
	CHECK_EQ(counters.batch_boolean_calls, 0);
	CHECK_EQ(counters.root_materializations, 1);
#endif // DEV_ENABLED
	box->queue_free();
}

TEST_CASE("[SceneTree][CSG] Phase 8 world-space planar UVs refinalize on ancestor move") {
	Ref<StandardMaterial3D> node_material;
	node_material.instantiate();
	Ref<StandardMaterial3D> planar_material;
	planar_material.instantiate();

	Node3D *parent = memnew(Node3D);
	SceneTree::get_singleton()->get_root()->add_child(parent);
	CSGBox3D *box = memnew(CSGBox3D);
	box->set_size(Vector3(2, 2, 2));
	box->set_material(node_material);
	box->set_position(Vector3(1.5, -0.75, 2));
	CSGSurfaceSetting setting;
	setting.material = planar_material;
	setting.uv_mode = CSGPrimitive3D::SURFACE_UV_MODE_PLANAR;
	setting.uv_space = CSGPrimitive3D::SURFACE_UV_SPACE_WORLD;
	box->set_surface_setting(CSGBox3D::SURFACE_POSITIVE_Z, setting);
	parent->add_child(box);
	REQUIRE(box->is_root_shape());

	SceneTree::get_singleton()->flush_transform_notifications();
	MessageQueue::get_singleton()->flush();
	box->update_shape();

	auto check_world_mapping = [&]() {
		PackedVector3Array vertices;
		PackedVector2Array uvs;
		REQUIRE(_get_surface_arrays_for_material(box->bake_static_mesh(), planar_material, vertices, uvs));
		REQUIRE_EQ(vertices.size(), uvs.size());
		const Vector3 global_origin = box->get_global_position();
		for (int vertex_i = 0; vertex_i < vertices.size(); vertex_i++) {
			const Vector2 expected(vertices[vertex_i].x + global_origin.x, vertices[vertex_i].y + global_origin.y);
			CHECK(uvs[vertex_i].is_equal_approx(expected));
		}
	};

	check_world_mapping();
	_reset_csg_counters();
	parent->set_position(Vector3(4, -3, 1));
	SceneTree::get_singleton()->flush_transform_notifications();
	MessageQueue::get_singleton()->flush();
	check_world_mapping();
#ifdef DEV_ENABLED
	CSGDebugCounters counters = CSGDebugCounters::get();
	CHECK_EQ(counters.uv_finalizations, 1);
	CHECK_EQ(counters.batch_boolean_calls, 0);
#endif // DEV_ENABLED

	parent->queue_free();
	MessageQueue::get_singleton()->flush();
}

TEST_CASE("[SceneTree][CSG] Phase 6 root-space texture lock survives one-sided box push pull") {
	Ref<StandardMaterial3D> node_material;
	node_material.instantiate();
	Ref<StandardMaterial3D> dragged_material;
	dragged_material.instantiate();
	Ref<StandardMaterial3D> fixed_material;
	fixed_material.instantiate();

	CSGCombiner3D *root = memnew(CSGCombiner3D);
	CSGBox3D *box = _add_box(root, Vector3(2, 2, 2), Vector3(), CSGShape3D::OPERATION_UNION, node_material);
	CSGSurfaceSetting dragged_setting;
	dragged_setting.material = dragged_material;
	dragged_setting.uv_mode = CSGPrimitive3D::SURFACE_UV_MODE_PLANAR;
	dragged_setting.uv_space = CSGPrimitive3D::SURFACE_UV_SPACE_ROOT;
	dragged_setting.meters_per_tile = Vector2(1.5, 2.5);
	dragged_setting.offset = Vector2(0.125, -0.375);
	dragged_setting.texture_lock = true;
	box->set_surface_setting(CSGBox3D::SURFACE_POSITIVE_X, dragged_setting);
	CSGSurfaceSetting fixed_setting = dragged_setting;
	fixed_setting.material = fixed_material;
	box->set_surface_setting(CSGBox3D::SURFACE_NEGATIVE_X, fixed_setting);
	SceneTree::get_singleton()->get_root()->add_child(root);
	root->update_shape();

	PackedVector3Array dragged_vertices_before;
	PackedVector2Array dragged_uvs_before;
	PackedVector3Array fixed_vertices_before;
	PackedVector2Array fixed_uvs_before;
	REQUIRE(_get_surface_arrays_for_material(root->bake_static_mesh(), dragged_material, dragged_vertices_before, dragged_uvs_before));
	REQUIRE(_get_surface_arrays_for_material(root->bake_static_mesh(), fixed_material, fixed_vertices_before, fixed_uvs_before));

	box->set_size(Vector3(4, 2, 2));
	box->set_transform(Transform3D(Basis(), Vector3(1, 0, 0)));
	root->update_shape();
	PackedVector3Array dragged_vertices_after;
	PackedVector2Array dragged_uvs_after;
	PackedVector3Array fixed_vertices_after;
	PackedVector2Array fixed_uvs_after;
	REQUIRE(_get_surface_arrays_for_material(root->bake_static_mesh(), dragged_material, dragged_vertices_after, dragged_uvs_after));
	REQUIRE(_get_surface_arrays_for_material(root->bake_static_mesh(), fixed_material, fixed_vertices_after, fixed_uvs_after));
	REQUIRE_EQ(dragged_uvs_after.size(), dragged_uvs_before.size());
	REQUIRE_EQ(fixed_uvs_after.size(), fixed_uvs_before.size());
	for (int vertex_i = 0; vertex_i < dragged_uvs_before.size(); vertex_i++) {
		CHECK(dragged_uvs_after[vertex_i].is_equal_approx(dragged_uvs_before[vertex_i]));
		CHECK(dragged_vertices_after[vertex_i].x == doctest::Approx(dragged_vertices_before[vertex_i].x + 2.0));
	}
	for (int vertex_i = 0; vertex_i < fixed_uvs_before.size(); vertex_i++) {
		CHECK(fixed_uvs_after[vertex_i].is_equal_approx(fixed_uvs_before[vertex_i]));
		CHECK(fixed_vertices_after[vertex_i].is_equal_approx(fixed_vertices_before[vertex_i]));
	}
	root->queue_free();
}

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
