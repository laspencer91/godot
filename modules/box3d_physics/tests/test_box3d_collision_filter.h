/**************************************************************************/
/*  test_box3d_collision_filter.h                                         */
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
/* included in all copies or substantial portions of the Software.       */
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

#include "../box3d_physics_server_3d.h"

#include "tests/test_macros.h"

// Godot decides whether a dynamic body contacts a static/kinematic body solely by the DYNAMIC
// side's mask against the other's layer; the non-dynamic body's own mask never vetoes the pair.
// Box3D's b3Filter is a symmetric AND, so the module gives non-dynamic bodies an all-bits mask
// (Box3DBody3D::_effective_mask_bits). Map geometry authored with collision_mask = 0 (idiomatic
// upstream — a static body's mask is meaningless there) regressed to "everything falls through"
// before that translation existed; these tests pin it, including across runtime mode changes
// (RigidBody3D.freeze IS a mode change and must swap the effective mask both ways).

namespace TestBox3DCollisionFilter {

class FilterTestSpace {
	Box3DPhysicsServer3D server;
	RID space;
	LocalVector<RID> objects;
	LocalVector<RID> shapes;

public:
	FilterTestSpace() {
		server.init();
		space = server.space_create();
		server.space_set_active(space, true);
	}

	~FilterTestSpace() {
		for (uint32_t i = objects.size(); i > 0; i--) {
			server.free_rid(objects[i - 1]);
		}
		for (uint32_t i = shapes.size(); i > 0; i--) {
			server.free_rid(shapes[i - 1]);
		}
		server.space_set_active(space, false);
		server.free_rid(space);
		server.finish();
	}

	Box3DPhysicsServer3D &get_server() { return server; }

	RID create_box(const Vector3 &p_half_extents) {
		RID shape = server.box_shape_create();
		shapes.push_back(shape);
		server.shape_set_data(shape, p_half_extents);
		return shape;
	}

	RID add_body(PS3DE::BodyMode p_mode, RID p_shape, const Transform3D &p_transform,
			uint32_t p_layer, uint32_t p_mask) {
		RID body = server.body_create();
		objects.push_back(body);
		server.body_set_mode(body, p_mode);
		server.body_add_shape(body, p_shape);
		server.body_set_state(body, PS3DE::BODY_STATE_TRANSFORM, p_transform);
		server.body_set_collision_layer(body, p_layer);
		server.body_set_collision_mask(body, p_mask);
		server.body_set_space(body, space);
		return body;
	}

	// Launches the body straight down; gravity in a bare test space is not guaranteed, and the
	// question under test is only "does a contact stop it".
	void drop(RID p_body, real_t p_speed) {
		get_server().body_set_state(p_body, PS3DE::BODY_STATE_LINEAR_VELOCITY, Vector3(0.0, -p_speed, 0.0));
	}

	real_t height_of(RID p_body) {
		Transform3D t = get_server().body_get_state(p_body, PS3DE::BODY_STATE_TRANSFORM);
		return t.origin.y;
	}

	void step(int p_frames) {
		for (int i = 0; i < p_frames; i++) {
			server.step(1.0 / 60.0);
			server.flush_queries();
		}
	}
};

// Layers mirror the game's project.godot: 1 world, and a "debris" layer that world does not mask.
constexpr uint32_t LAYER_WORLD = 1u << 0;
constexpr uint32_t LAYER_DEBRIS = 1u << 3;
constexpr uint32_t LAYER_OTHER = 1u << 4;

TEST_CASE("[Box3D][CollisionFilter] Rigid body lands on a zero-mask static floor") {
	FilterTestSpace test_space;
	test_space.add_body(PS3DE::BODY_MODE_STATIC, test_space.create_box(Vector3(10.0, 0.5, 10.0)),
			Transform3D(), LAYER_WORLD, 0);
	const RID chunk = test_space.add_body(PS3DE::BODY_MODE_RIGID,
			test_space.create_box(Vector3(0.25, 0.25, 0.25)),
			Transform3D(Basis(), Vector3(0.0, 3.0, 0.0)), LAYER_DEBRIS, LAYER_WORLD);
	test_space.drop(chunk, 5.0);
	test_space.step(90);
	// Without the contact it would be several metres below the slab by now.
	CHECK(test_space.height_of(chunk) > 0.5);
}

TEST_CASE("[Box3D][CollisionFilter] Dynamic side's mask still filters against statics") {
	FilterTestSpace test_space;
	test_space.add_body(PS3DE::BODY_MODE_STATIC, test_space.create_box(Vector3(10.0, 0.5, 10.0)),
			Transform3D(), LAYER_OTHER, 0);
	const RID chunk = test_space.add_body(PS3DE::BODY_MODE_RIGID,
			test_space.create_box(Vector3(0.25, 0.25, 0.25)),
			Transform3D(Basis(), Vector3(0.0, 3.0, 0.0)), LAYER_DEBRIS, LAYER_WORLD);
	test_space.drop(chunk, 5.0);
	test_space.step(90);
	// The floor's layer is not in the chunk's mask: no contact, regardless of the floor's
	// all-bits effective mask.
	CHECK(test_space.height_of(chunk) < -1.0);
}

TEST_CASE("[Box3D][CollisionFilter] Thawing a frozen body restores its real mask") {
	FilterTestSpace test_space;
	// Floor on a layer the chunk does NOT mask. While the chunk is frozen (static), its
	// effective mask is all bits — but a thawed chunk must go back to falling through.
	test_space.add_body(PS3DE::BODY_MODE_STATIC, test_space.create_box(Vector3(10.0, 0.5, 10.0)),
			Transform3D(), LAYER_OTHER, 0);
	const RID chunk = test_space.add_body(PS3DE::BODY_MODE_RIGID,
			test_space.create_box(Vector3(0.25, 0.25, 0.25)),
			Transform3D(Basis(), Vector3(0.0, 3.0, 0.0)), LAYER_DEBRIS, LAYER_WORLD);
	test_space.get_server().body_set_mode(chunk, PS3DE::BODY_MODE_STATIC);
	test_space.step(5);
	test_space.get_server().body_set_mode(chunk, PS3DE::BODY_MODE_RIGID);
	test_space.drop(chunk, 5.0);
	test_space.step(90);
	CHECK(test_space.height_of(chunk) < -1.0);
}

TEST_CASE("[Box3D][CollisionFilter] Freezing a body makes it catch bodies that mask it") {
	FilterTestSpace test_space;
	// A platform created DYNAMIC (mask does not see the faller) then frozen static. The faller
	// masks the platform's layer; Godot semantics say that alone decides the contact.
	const RID platform = test_space.add_body(PS3DE::BODY_MODE_RIGID,
			test_space.create_box(Vector3(2.0, 0.5, 2.0)),
			Transform3D(), LAYER_DEBRIS, LAYER_WORLD);
	test_space.get_server().body_set_mode(platform, PS3DE::BODY_MODE_STATIC);
	const RID faller = test_space.add_body(PS3DE::BODY_MODE_RIGID,
			test_space.create_box(Vector3(0.25, 0.25, 0.25)),
			Transform3D(Basis(), Vector3(0.0, 3.0, 0.0)), LAYER_OTHER, LAYER_DEBRIS);
	test_space.drop(faller, 5.0);
	test_space.step(90);
	CHECK(test_space.height_of(faller) > 0.5);
}

} // namespace TestBox3DCollisionFilter
