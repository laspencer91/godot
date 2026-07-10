/**************************************************************************/
/*  test_box3d_ragdoll.h                                                  */
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

#include "../box3d_ragdoll.h"

#ifdef TOOLS_ENABLED
#include "../editor/box3d_ragdoll_profile_dialog.h"
#endif

#include "scene/3d/mesh_instance_3d.h"
#include "scene/main/scene_tree.h"
#include "scene/resources/3d/skin.h"
#include "scene/resources/animation.h"
#include "scene/resources/animation_library.h"
#include "scene/resources/mesh.h"
#include "tests/test_macros.h"

namespace TestBox3DRagdoll {

static Dictionary _find_profile_bone(const Ref<Box3DRagdollProfile> &p_profile, const StringName &p_bone_name) {
	const TypedArray<Dictionary> bones = p_profile->get_bones();
	for (int i = 0; i < bones.size(); i++) {
		const Dictionary bone = bones[i];
		if ((StringName)bone.get(SNAME("bone"), StringName()) == p_bone_name) {
			return bone;
		}
	}
	return Dictionary();
}

TEST_CASE("[Box3D][Ragdoll] Fallback profile generation scales total mass") {
	Skeleton3D *skeleton = memnew(Skeleton3D);
	const int hips = skeleton->add_bone("Hips");
	skeleton->set_bone_rest(hips, Transform3D());
	const int spine = skeleton->add_bone("Spine");
	skeleton->set_bone_parent(spine, hips);
	skeleton->set_bone_rest(spine, Transform3D(Basis(), Vector3(0.0, 0.8, 0.0)));
	const int head = skeleton->add_bone("Head");
	skeleton->set_bone_parent(head, spine);
	skeleton->set_bone_rest(head, Transform3D(Basis(), Vector3(0.0, 0.6, 0.0)));

	Ref<Box3DRagdollProfileGenerator> generator;
	generator.instantiate();
	generator->set_target_total_mass(75.0);
	const Ref<Box3DRagdollProfile> profile = generator->generate_profile(skeleton);

	REQUIRE(profile.is_valid());
	CHECK(profile->get_bones().size() == 3);
	CHECK(profile->estimate_total_mass() == doctest::Approx(75.0));
	const Dictionary analysis = generator->analyze_profile(profile);
	CHECK((int)analysis[SNAME("bone_count")] == 3);
	CHECK((double)analysis[SNAME("total_mass")] == doctest::Approx(75.0));

	const Dictionary spine_entry = _find_profile_bone(profile, SNAME("Spine"));
	REQUIRE_FALSE(spine_entry.is_empty());
	const Box3DRagdollProfile::BoneParams spine_params = Box3DRagdollProfile::parse_bone_entry(spine_entry);
	const Transform3D spine_body_rest = skeleton->get_bone_global_rest(spine) * spine_params.offset;
	CHECK(spine_body_rest.xform(spine_params.joint_frame.origin).is_equal_approx(skeleton->get_bone_global_rest(spine).origin));

	memdelete(skeleton);
}

TEST_CASE("[Box3D][Ragdoll] Limb names select anatomical joint types") {
	Skeleton3D *skeleton = memnew(Skeleton3D);
	const int hips = skeleton->add_bone("Hips");
	skeleton->set_bone_rest(hips, Transform3D());
	const int upper_arm = skeleton->add_bone("LeftUpperArm");
	skeleton->set_bone_parent(upper_arm, hips);
	skeleton->set_bone_rest(upper_arm, Transform3D(Basis(), Vector3(0.2, 0.4, 0.0)));
	const int lower_arm = skeleton->add_bone("LeftLowerArm");
	skeleton->set_bone_parent(lower_arm, upper_arm);
	skeleton->set_bone_rest(lower_arm, Transform3D(Basis(), Vector3(0.4, 0.0, 0.0)));
	const int upper_leg = skeleton->add_bone("LeftUpperLeg");
	skeleton->set_bone_parent(upper_leg, hips);
	skeleton->set_bone_rest(upper_leg, Transform3D(Basis(), Vector3(-0.2, -0.2, 0.0)));
	const int lower_leg = skeleton->add_bone("LeftLowerLeg");
	skeleton->set_bone_parent(lower_leg, upper_leg);
	skeleton->set_bone_rest(lower_leg, Transform3D(Basis(), Vector3(0.0, -0.5, 0.0)));

	Ref<Box3DRagdollProfileGenerator> generator;
	generator.instantiate();
	const Ref<Box3DRagdollProfile> profile = generator->generate_profile(skeleton);
	REQUIRE(profile.is_valid());

	CHECK(Box3DRagdollProfile::parse_bone_entry(_find_profile_bone(profile, SNAME("LeftUpperArm"))).joint_type == Box3DRagdollProfile::JOINT_TYPE_SPHERICAL);
	CHECK(Box3DRagdollProfile::parse_bone_entry(_find_profile_bone(profile, SNAME("LeftLowerArm"))).joint_type == Box3DRagdollProfile::JOINT_TYPE_REVOLUTE);
	CHECK(Box3DRagdollProfile::parse_bone_entry(_find_profile_bone(profile, SNAME("LeftUpperLeg"))).joint_type == Box3DRagdollProfile::JOINT_TYPE_SPHERICAL);
	CHECK(Box3DRagdollProfile::parse_bone_entry(_find_profile_bone(profile, SNAME("LeftLowerLeg"))).joint_type == Box3DRagdollProfile::JOINT_TYPE_REVOLUTE);

	memdelete(skeleton);
}

// Builds a point-cloud mesh where every vertex is fully weighted to one bone.
static MeshInstance3D *_add_skinned_mesh(Skeleton3D *p_skeleton, const PackedVector3Array &p_vertices, const PackedInt32Array &p_vertex_bones) {
	PackedInt32Array bones;
	PackedFloat32Array weights;
	for (int i = 0; i < p_vertices.size(); i++) {
		bones.append(p_vertex_bones[i]);
		bones.append(0);
		bones.append(0);
		bones.append(0);
		weights.append(1.0f);
		weights.append(0.0f);
		weights.append(0.0f);
		weights.append(0.0f);
	}
	Array arrays;
	arrays.resize(Mesh::ARRAY_MAX);
	arrays[Mesh::ARRAY_VERTEX] = p_vertices;
	arrays[Mesh::ARRAY_BONES] = bones;
	arrays[Mesh::ARRAY_WEIGHTS] = weights;
	Ref<ArrayMesh> mesh;
	mesh.instantiate();
	mesh->add_surface_from_arrays(Mesh::PRIMITIVE_POINTS, arrays);
	MeshInstance3D *mesh_instance = memnew(MeshInstance3D);
	mesh_instance->set_mesh(mesh);
	p_skeleton->add_child(mesh_instance);
	return mesh_instance;
}

static void _append_ring(PackedVector3Array &r_vertices, PackedInt32Array &r_vertex_bones, int p_bone, const Vector3 &p_center, real_t p_radius) {
	for (int i = 0; i < 8; i++) {
		const real_t angle = Math::TAU * (real_t)i / (real_t)8.0;
		r_vertices.append(p_center + Vector3(Math::cos(angle) * p_radius, 0.0, Math::sin(angle) * p_radius));
		r_vertex_bones.append(p_bone);
	}
}

TEST_CASE("[SceneTree][Box3D][Ragdoll] Capsule fit spans the bone and ignores vertices past the joints") {
	Skeleton3D *skeleton = memnew(Skeleton3D);
	const int hips = skeleton->add_bone("Hips");
	skeleton->set_bone_rest(hips, Transform3D());
	const int spine = skeleton->add_bone("Spine");
	skeleton->set_bone_parent(spine, hips);
	skeleton->set_bone_rest(spine, Transform3D(Basis(), Vector3(0.0, 0.4, 0.0)));
	SceneTree::get_singleton()->get_root()->add_child(skeleton);

	// Rings of radius 0.1 inside the Hips span, plus outliers hanging well
	// below the bone origin that must not inflate the fitted radius.
	PackedVector3Array vertices;
	PackedInt32Array vertex_bones;
	_append_ring(vertices, vertex_bones, hips, Vector3(0.0, 0.1, 0.0), 0.1);
	_append_ring(vertices, vertex_bones, hips, Vector3(0.0, 0.3, 0.0), 0.1);
	for (int i = 0; i < 6; i++) {
		vertices.append(Vector3(0.05, -0.3 - 0.01 * i, 0.0));
		vertex_bones.append(hips);
	}
	MeshInstance3D *mesh_instance = _add_skinned_mesh(skeleton, vertices, vertex_bones);

	Ref<Box3DRagdollProfileGenerator> generator;
	generator.instantiate();
	const Ref<Box3DRagdollProfile> profile = generator->generate_profile(skeleton, mesh_instance);
	REQUIRE(profile.is_valid());

	const Dictionary hips_entry = _find_profile_bone(profile, SNAME("Hips"));
	REQUIRE_FALSE(hips_entry.is_empty());
	const Box3DRagdollProfile::BoneParams params = Box3DRagdollProfile::parse_bone_entry(hips_entry);
	CHECK(params.radius == doctest::Approx(0.1).epsilon(0.01));
	// Tip-to-tip: the capsule ends at the bone endpoints instead of protruding
	// a full radius past each joint.
	CHECK(params.height == doctest::Approx(0.4).epsilon(0.01));

	SceneTree::get_singleton()->get_root()->remove_child(skeleton);
	memdelete(skeleton);
}

TEST_CASE("[SceneTree][Box3D][Ragdoll] Coincident sibling capsules merge into one body") {
	Skeleton3D *skeleton = memnew(Skeleton3D);
	const int hips = skeleton->add_bone("Hips");
	skeleton->set_bone_rest(hips, Transform3D());
	const int spine = skeleton->add_bone("Spine");
	skeleton->set_bone_parent(spine, hips);
	skeleton->set_bone_rest(spine, Transform3D(Basis(), Vector3(0.0, 0.4, 0.0)));
	const int pad_a = skeleton->add_bone("pad.01");
	skeleton->set_bone_parent(pad_a, hips);
	skeleton->set_bone_rest(pad_a, Transform3D(Basis(), Vector3(0.3, 0.0, 0.0)));
	const int pad_b = skeleton->add_bone("pad.02");
	skeleton->set_bone_parent(pad_b, hips);
	skeleton->set_bone_rest(pad_b, Transform3D(Basis(), Vector3(0.3, 0.0, 0.01)));
	SceneTree::get_singleton()->get_root()->add_child(skeleton);

	PackedVector3Array vertices;
	PackedInt32Array vertex_bones;
	_append_ring(vertices, vertex_bones, hips, Vector3(0.0, 0.1, 0.0), 0.1);
	_append_ring(vertices, vertex_bones, hips, Vector3(0.0, 0.3, 0.0), 0.1);
	// Both sibling bones cover the same geometry.
	_append_ring(vertices, vertex_bones, pad_a, Vector3(0.45, 0.0, 0.0), 0.05);
	_append_ring(vertices, vertex_bones, pad_b, Vector3(0.45, 0.0, 0.01), 0.05);
	MeshInstance3D *mesh_instance = _add_skinned_mesh(skeleton, vertices, vertex_bones);

	Ref<Box3DRagdollProfileGenerator> generator;
	generator.instantiate();
	const Ref<Box3DRagdollProfile> profile = generator->generate_profile(skeleton, mesh_instance);
	REQUIRE(profile.is_valid());

	CHECK(profile->get_bones().size() == 2);
	CHECK_FALSE(_find_profile_bone(profile, SNAME("Hips")).is_empty());
	// One sibling body survives and absorbs the other's vertices.
	const bool pad_a_kept = !_find_profile_bone(profile, SNAME("pad.01")).is_empty();
	const bool pad_b_kept = !_find_profile_bone(profile, SNAME("pad.02")).is_empty();
	CHECK(pad_a_kept != pad_b_kept);
	// Self-collision filters cover the jointed pair.
	CHECK(profile->get_filter_pairs().size() == 1);

	String merged_warning;
	for (const String &warning : generator->get_warnings()) {
		if (warning.contains("sibling")) {
			merged_warning = warning;
		}
	}
	CHECK_FALSE(merged_warning.is_empty());

	SceneTree::get_singleton()->get_root()->remove_child(skeleton);
	memdelete(skeleton);
}

TEST_CASE("[SceneTree][Box3D][Ragdoll] Vertices map through skin binds when the bind pose differs from rest") {
	Skeleton3D *skeleton = memnew(Skeleton3D);
	const int hips = skeleton->add_bone("Hips");
	skeleton->set_bone_rest(hips, Transform3D());
	const int spine = skeleton->add_bone("Spine");
	skeleton->set_bone_parent(spine, hips);
	skeleton->set_bone_rest(spine, Transform3D(Basis(), Vector3(0.0, 0.4, 0.0)));
	SceneTree::get_singleton()->get_root()->add_child(skeleton);

	// The mesh was bound with the bone half a meter to the side of where the
	// (retargeted) rest pose puts it. Rings of radius 0.1 around the bind-pose
	// bone location must still fit a 0.1 capsule, not a 0.5+ one.
	const Vector3 bind_offset(0.5, 0.0, 0.0);
	PackedVector3Array vertices;
	PackedInt32Array vertex_bones;
	_append_ring(vertices, vertex_bones, hips, bind_offset + Vector3(0.0, 0.1, 0.0), 0.1);
	_append_ring(vertices, vertex_bones, hips, bind_offset + Vector3(0.0, 0.3, 0.0), 0.1);
	MeshInstance3D *mesh_instance = _add_skinned_mesh(skeleton, vertices, vertex_bones);
	Ref<Skin> skin;
	skin.instantiate();
	skin->add_bind(hips, Transform3D(Basis(), -bind_offset));
	skin->add_bind(spine, Transform3D(Basis(), Vector3(0.0, -0.4, 0.0)));
	mesh_instance->set_skin(skin);

	Ref<Box3DRagdollProfileGenerator> generator;
	generator.instantiate();
	const Ref<Box3DRagdollProfile> profile = generator->generate_profile(skeleton, mesh_instance);
	REQUIRE(profile.is_valid());

	const Dictionary hips_entry = _find_profile_bone(profile, SNAME("Hips"));
	REQUIRE_FALSE(hips_entry.is_empty());
	const Box3DRagdollProfile::BoneParams params = Box3DRagdollProfile::parse_bone_entry(hips_entry);
	CHECK(params.radius == doctest::Approx(0.1).epsilon(0.01));
	// The capsule sits on the rest-pose bone, not out at the bind location.
	const Vector3 center = skeleton->get_bone_global_rest(hips).xform(params.offset.origin);
	CHECK(center.is_equal_approx(Vector3(0.0, 0.2, 0.0)));

	SceneTree::get_singleton()->get_root()->remove_child(skeleton);
	memdelete(skeleton);
}

TEST_CASE("[SceneTree][Box3D][Ragdoll] Importer artifacts and secondary bones get no bodies") {
	Skeleton3D *skeleton = memnew(Skeleton3D);
	const int hips = skeleton->add_bone("Hips");
	skeleton->set_bone_rest(hips, Transform3D());
	const int spine = skeleton->add_bone("Spine");
	skeleton->set_bone_parent(spine, hips);
	skeleton->set_bone_rest(spine, Transform3D(Basis(), Vector3(0.0, 0.4, 0.0)));
	const int breast = skeleton->add_bone("breast.L");
	skeleton->set_bone_parent(breast, hips);
	skeleton->set_bone_rest(breast, Transform3D(Basis(), Vector3(0.1, 0.2, 0.1)));
	const int neutral = skeleton->add_bone("neutral_bone");
	skeleton->set_bone_parent(neutral, hips);
	skeleton->set_bone_rest(neutral, Transform3D());
	SceneTree::get_singleton()->get_root()->add_child(skeleton);

	PackedVector3Array vertices;
	PackedInt32Array vertex_bones;
	_append_ring(vertices, vertex_bones, hips, Vector3(0.0, 0.1, 0.0), 0.1);
	_append_ring(vertices, vertex_bones, hips, Vector3(0.0, 0.3, 0.0), 0.1);
	// Secondary-motion geometry folds into the parent capsule.
	_append_ring(vertices, vertex_bones, breast, Vector3(0.0, 0.2, 0.0), 0.1);
	// Importer neutral bones collect strays scattered across the whole mesh.
	for (int i = 0; i < 8; i++) {
		vertices.append(Vector3(i % 2 == 0 ? 1.0 : -1.0, 0.2 * i, i % 3 == 0 ? 1.0 : -1.0));
		vertex_bones.append(neutral);
	}
	MeshInstance3D *mesh_instance = _add_skinned_mesh(skeleton, vertices, vertex_bones);

	Ref<Box3DRagdollProfileGenerator> generator;
	generator.instantiate();
	const Ref<Box3DRagdollProfile> profile = generator->generate_profile(skeleton, mesh_instance);
	REQUIRE(profile.is_valid());

	CHECK(profile->get_bones().size() == 1);
	const Dictionary hips_entry = _find_profile_bone(profile, SNAME("Hips"));
	REQUIRE_FALSE(hips_entry.is_empty());
	// The stray-vertex bone must not distort the surviving capsule.
	const Box3DRagdollProfile::BoneParams params = Box3DRagdollProfile::parse_bone_entry(hips_entry);
	CHECK(params.radius == doctest::Approx(0.1).epsilon(0.01));

	bool warned_secondary = false;
	for (const String &warning : generator->get_warnings()) {
		if (warning.contains("secondary and extremity")) {
			warned_secondary = true;
		}
	}
	CHECK(warned_secondary);

	SceneTree::get_singleton()->get_root()->remove_child(skeleton);
	memdelete(skeleton);
}

static Ref<Animation> _make_rotation_clip(const String &p_bone, const Vector3 &p_axis, real_t p_from_degrees, real_t p_to_degrees) {
	Ref<Animation> animation;
	animation.instantiate();
	animation->set_length(1.0);
	const int track = animation->add_track(Animation::TYPE_ROTATION_3D);
	animation->track_set_path(track, NodePath(":" + p_bone));
	animation->rotation_track_insert_key(track, 0.0, Quaternion(p_axis, Math::deg_to_rad(p_from_degrees)));
	animation->rotation_track_insert_key(track, 1.0, Quaternion(p_axis, Math::deg_to_rad(p_to_degrees)));
	return animation;
}

TEST_CASE("[Box3D][Ragdoll] Animation limits re-center the joint neutral and union all clips") {
	Skeleton3D *skeleton = memnew(Skeleton3D);
	const int hips = skeleton->add_bone("Hips");
	skeleton->set_bone_rest(hips, Transform3D());
	const int spine = skeleton->add_bone("Spine");
	skeleton->set_bone_parent(spine, hips);
	skeleton->set_bone_rest(spine, Transform3D(Basis(), Vector3(0.0, 0.4, 0.0)));

	// Two clips twisting Spine about its own (vertical) axis: 40-60 degrees and
	// 20-40 degrees. The union is 20-60, so the neutral should re-center near
	// 40 degrees with symmetric residual limits that contain zero.
	Ref<AnimationLibrary> library;
	library.instantiate();
	library->add_animation("clip_a", _make_rotation_clip("Spine", Vector3(0, 1, 0), 40.0, 60.0));
	library->add_animation("clip_b", _make_rotation_clip("Spine", Vector3(0, 1, 0), 20.0, 40.0));

	Ref<Box3DRagdollProfileGenerator> generator;
	generator.instantiate();
	const Ref<Box3DRagdollProfile> profile = generator->generate_profile(skeleton, nullptr, library);
	REQUIRE(profile.is_valid());

	const Dictionary spine_entry = _find_profile_bone(profile, SNAME("Spine"));
	REQUIRE_FALSE(spine_entry.is_empty());
	const Box3DRagdollProfile::BoneParams params = Box3DRagdollProfile::parse_bone_entry(spine_entry);

	Vector3 delta_axis;
	real_t delta_angle = 0.0;
	params.rest_delta.get_axis_angle(delta_axis, delta_angle);
	CHECK(delta_angle == doctest::Approx(Math::deg_to_rad(40.0)).epsilon(0.05));
	// Residual range is +/-20 degrees plus the default 10 degree padding, and
	// always contains the (re-centered) neutral.
	CHECK(params.twist_lower == doctest::Approx(Math::deg_to_rad(-30.0)).epsilon(0.05));
	CHECK(params.twist_upper == doctest::Approx(Math::deg_to_rad(30.0)).epsilon(0.05));
	CHECK(params.twist_lower < 0.0);
	CHECK(params.twist_upper > 0.0);
	// Pure twist: the swing limit collapses to the padding.
	CHECK(params.swing_limit == doctest::Approx(Math::deg_to_rad(10.0)).epsilon(0.1));

	memdelete(skeleton);
}

TEST_CASE("[SceneTree][Box3D][Ragdoll] Revolute bones recover the hinge axis from animation data") {
	Skeleton3D *skeleton = memnew(Skeleton3D);
	const int hips = skeleton->add_bone("Hips");
	skeleton->set_bone_rest(hips, Transform3D());
	const int upper_arm = skeleton->add_bone("LeftUpperArm");
	skeleton->set_bone_parent(upper_arm, hips);
	skeleton->set_bone_rest(upper_arm, Transform3D(Basis(), Vector3(0.2, 0.0, 0.0)));
	const int lower_arm = skeleton->add_bone("LeftLowerArm");
	skeleton->set_bone_parent(lower_arm, upper_arm);
	skeleton->set_bone_rest(lower_arm, Transform3D(Basis(), Vector3(0.3, 0.0, 0.0)));

	// The elbow bends about world Z, which is not the name-heuristic's guess
	// for this rig; without axis recovery the whole motion registers as swing.
	Ref<AnimationLibrary> library;
	library.instantiate();
	library->add_animation("bend", _make_rotation_clip("LeftLowerArm", Vector3(0, 0, 1), 10.0, 80.0));

	Ref<Box3DRagdollProfileGenerator> generator;
	generator.instantiate();
	const Ref<Box3DRagdollProfile> profile = generator->generate_profile(skeleton, nullptr, library);
	REQUIRE(profile.is_valid());

	const Dictionary lower_arm_entry = _find_profile_bone(profile, SNAME("LeftLowerArm"));
	REQUIRE_FALSE(lower_arm_entry.is_empty());
	const Box3DRagdollProfile::BoneParams params = Box3DRagdollProfile::parse_bone_entry(lower_arm_entry);
	CHECK(params.joint_type == Box3DRagdollProfile::JOINT_TYPE_REVOLUTE);

	// The re-aimed hinge axis (joint-frame Z in world space) matches the
	// sampled bending axis.
	const Vector3 world_hinge_axis = (params.offset.basis * params.joint_frame.basis).xform(Vector3(0, 0, 1));
	CHECK(Math::abs(world_hinge_axis.z) == doctest::Approx(1.0).epsilon(0.01));

	// The bending range lands in twist, re-centered around the mean bend, and
	// nearly nothing remains as swing.
	CHECK(params.twist_lower == doctest::Approx(Math::deg_to_rad(-45.0)).epsilon(0.05));
	CHECK(params.twist_upper == doctest::Approx(Math::deg_to_rad(45.0)).epsilon(0.05));
	CHECK(params.swing_limit < Math::deg_to_rad(15.0));

	bool warned_reaim = false;
	for (const String &warning : generator->get_warnings()) {
		if (warning.contains("re-aimed")) {
			warned_reaim = true;
		}
	}
	CHECK(warned_reaim);

	// Gizmo generation stays valid with re-centered (rest_delta) entries: lines
	// exist for every chain and every point is finite.
	SceneTree::get_singleton()->get_root()->add_child(skeleton);
	const PackedVector3Array gizmo_lines = generator->get_gizmo_lines(profile, skeleton);
	CHECK(gizmo_lines.size() > 0);
	bool gizmo_finite = true;
	for (const Vector3 &point : gizmo_lines) {
		gizmo_finite = gizmo_finite && point.is_finite();
	}
	CHECK(gizmo_finite);

	SceneTree::get_singleton()->get_root()->remove_child(skeleton);
	memdelete(skeleton);
}

#ifdef TOOLS_ENABLED
TEST_CASE("[Box3D][Ragdoll][Editor] Generation context binds the owning scene, not editor focus") {
	Node *scene_a = memnew(Node);
	scene_a->set_name("SceneA");
	Node *scene_b = memnew(Node);
	scene_b->set_name("SceneB");
	Skeleton3D *skeleton = memnew(Skeleton3D);
	Box3DRagdoll *ragdoll = memnew(Box3DRagdoll);
	scene_b->add_child(skeleton);
	skeleton->add_child(ragdoll);
	SceneTree::get_singleton()->get_root()->add_child(scene_a);
	SceneTree::get_singleton()->get_root()->add_child(scene_b);

	Vector<EditorData::EditedScene> scenes;
	EditorData::EditedScene edited_a;
	edited_a.root = scene_a;
	edited_a.path = "res://scene_a.tscn";
	edited_a.history_id = 11;
	scenes.push_back(edited_a);
	EditorData::EditedScene edited_b;
	edited_b.root = scene_b;
	edited_b.path = "res://scene_b.tscn";
	edited_b.history_id = 22;
	scenes.push_back(edited_b);

	Box3DRagdollGenerationContext context;
	String error;
	REQUIRE(context.capture_from_scenes(scenes, ragdoll, error));
	CHECK(context.ragdoll_id == ragdoll->get_instance_id());
	CHECK(context.scene_root_id == scene_b->get_instance_id());
	CHECK(context.skeleton_id == skeleton->get_instance_id());
	CHECK(context.scene_history_id == 22);
	CHECK(context.scene_path == "res://scene_b.tscn");
	CHECK(context.matches_open_scene(scenes));

	SWAP(scenes.write[0], scenes.write[1]);
	CHECK(context.matches_open_scene(scenes));
	scenes.write[0].history_id = 23;
	CHECK_FALSE(context.matches_open_scene(scenes));

	SceneTree::get_singleton()->get_root()->remove_child(scene_a);
	SceneTree::get_singleton()->get_root()->remove_child(scene_b);
	memdelete(scene_a);
	memdelete(scene_b);
}
#endif

} // namespace TestBox3DRagdoll
