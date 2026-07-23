/**************************************************************************/
/*  test_csg_edit_domain.cpp                                              */
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

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_csg_edit_domain)

#ifdef TOOLS_ENABLED

#include "editor/gui/editor_edit_domain.h"
#include "scene/3d/node_3d.h"
#include "scene/main/scene_tree.h"
#include "scene/main/window.h"

#include "modules/csg/editor/csg_edit_domain.h"

namespace TestCSGEditDomain {

class HeadlessCSGEditDomainProvider : public CSGEditDomainProvider {
public:
	virtual StringName get_domain_id() const override { return SNAME("headless_csg_surface"); }
	virtual bool is_available(const EditorEditDomainContext &p_context) const override { return true; }
};

TEST_CASE("[Editor][CSGEditDomain] Provider gates CSG selection and double-click hits") {
	CSGEditDomainProvider provider;
	EditorEditDomainRegistry registry;
	REQUIRE(registry.register_provider(&provider));
	CHECK(registry.get_provider(SNAME("csg_surface")) == &provider);

	EditorEditDomainContext context;
	CHECK_FALSE(provider.is_available(context));

	Node3D *plain_node = memnew(Node3D);
	CSGBox3D *box = memnew(CSGBox3D);
	CHECK_FALSE(provider.can_activate_from_double_click(context, plain_node->get_instance_id()));
	CHECK(provider.can_activate_from_double_click(context, box->get_instance_id()));

	memdelete(plain_node);
	memdelete(box);
	CHECK(registry.unregister_provider(provider.get_domain_id(), &provider));
}

TEST_CASE("[Editor][CSGEditDomain] Provider creates independent surface sessions") {
	CSGEditDomainProvider provider;
	EditorEditDomainContext context;
	EditorEditDomainSession *first = provider.create_session(context);
	EditorEditDomainSession *second = provider.create_session(context);
	REQUIRE(first != nullptr);
	REQUIRE(second != nullptr);
	CHECK(first != second);
	CHECK(static_cast<CSGSurfaceSession *>(first)->get_active_root_id().is_null());
	CHECK(static_cast<CSGSurfaceSession *>(second)->get_active_root_id().is_null());
	memdelete(first);
	memdelete(second);
}

TEST_CASE("[Editor][CSGEditDomain] Box push pull preserves the fixed face and transform basis") {
	const Vector3 start_size(2, 4, 6);
	const Transform3D identity_start(Basis(), Vector3(1, 2, 3));

	SUBCASE("Positive face moves one-sided") {
		const CSGPushPullResult result = csg_push_pull_apply(start_size, identity_start, CSGBox3D::SURFACE_POSITIVE_X, 2.0, false);
		CHECK(result.size.is_equal_approx(Vector3(4, 4, 6)));
		CHECK(result.transform.origin.is_equal_approx(Vector3(2, 2, 3)));
		CHECK(result.transform.basis.is_equal_approx(identity_start.basis));
	}

	SUBCASE("Negative face moves one-sided") {
		const CSGPushPullResult result = csg_push_pull_apply(start_size, identity_start, CSGBox3D::SURFACE_NEGATIVE_X, 2.0, false);
		CHECK(result.size.is_equal_approx(Vector3(4, 4, 6)));
		CHECK(result.transform.origin.is_equal_approx(Vector3(0, 2, 3)));
		CHECK(result.transform.basis.is_equal_approx(identity_start.basis));
	}

	SUBCASE("Alt displacement is symmetric") {
		const CSGPushPullResult result = csg_push_pull_apply(start_size, identity_start, CSGBox3D::SURFACE_POSITIVE_Y, 1.0, true);
		CHECK(result.size.is_equal_approx(Vector3(2, 6, 6)));
		CHECK(result.transform.is_equal_approx(identity_start));
	}

	SUBCASE("Rotated nonuniform basis is preserved") {
		const Basis basis = Basis(Vector3(0, 1, 0), Math::deg_to_rad(35.0)).scaled_local(Vector3(2, 3, 4));
		const Transform3D transformed_start(basis, Vector3(-3, 5, 7));
		const CSGPushPullResult result = csg_push_pull_apply(start_size, transformed_start, CSGBox3D::SURFACE_POSITIVE_Z, 2.0, false);
		CHECK(result.size.is_equal_approx(Vector3(2, 4, 8)));
		CHECK(result.transform.basis.is_equal_approx(basis));
		CHECK(result.transform.origin.is_equal_approx(transformed_start.origin + basis.xform(Vector3(0, 0, 1))));
	}

	SUBCASE("Clamp keeps the opposite face fixed") {
		const CSGPushPullResult result = csg_push_pull_apply(start_size, identity_start, CSGBox3D::SURFACE_NEGATIVE_X, -10.0, false);
		CHECK(result.size.x == doctest::Approx(0.001));
		CHECK(result.transform.origin.x == doctest::Approx(1.9995));
		CHECK(result.transform.basis.is_equal_approx(identity_start.basis));
	}
}

// CSG-5: Pin the child footprint, cap center, and identity local basis on every face.
TEST_CASE("[Editor][CSGEditDomain] Box face extrusion produces an outward identity-basis child") {
	const Vector3 source_size(2, 4, 6);

	SUBCASE("Positive X") {
		const CSGExtrusionResult result = csg_extrude_box_face(source_size, CSGBox3D::SURFACE_POSITIVE_X, 1.5);
		CHECK(result.size.is_equal_approx(Vector3(1.5, 4, 6)));
		CHECK(result.local_transform.origin.is_equal_approx(Vector3(1.75, 0, 0)));
		CHECK(result.local_transform.basis.is_equal_approx(Basis()));
	}

	SUBCASE("Negative X") {
		const CSGExtrusionResult result = csg_extrude_box_face(source_size, CSGBox3D::SURFACE_NEGATIVE_X, 2.5);
		CHECK(result.size.is_equal_approx(Vector3(2.5, 4, 6)));
		CHECK(result.local_transform.origin.is_equal_approx(Vector3(-2.25, 0, 0)));
		CHECK(result.local_transform.basis.is_equal_approx(Basis()));
	}

	SUBCASE("Positive Y") {
		const CSGExtrusionResult result = csg_extrude_box_face(source_size, CSGBox3D::SURFACE_POSITIVE_Y, 0.75);
		CHECK(result.size.is_equal_approx(Vector3(2, 0.75, 6)));
		CHECK(result.local_transform.origin.is_equal_approx(Vector3(0, 2.375, 0)));
		CHECK(result.local_transform.basis.is_equal_approx(Basis()));
	}

	SUBCASE("Negative Y") {
		const CSGExtrusionResult result = csg_extrude_box_face(source_size, CSGBox3D::SURFACE_NEGATIVE_Y, 3.0);
		CHECK(result.size.is_equal_approx(Vector3(2, 3, 6)));
		CHECK(result.local_transform.origin.is_equal_approx(Vector3(0, -3.5, 0)));
		CHECK(result.local_transform.basis.is_equal_approx(Basis()));
	}

	SUBCASE("Positive Z") {
		const CSGExtrusionResult result = csg_extrude_box_face(source_size, CSGBox3D::SURFACE_POSITIVE_Z, 4.0);
		CHECK(result.size.is_equal_approx(Vector3(2, 4, 4)));
		CHECK(result.local_transform.origin.is_equal_approx(Vector3(0, 0, 5)));
		CHECK(result.local_transform.basis.is_equal_approx(Basis()));
	}

	SUBCASE("Negative Z") {
		const CSGExtrusionResult result = csg_extrude_box_face(source_size, CSGBox3D::SURFACE_NEGATIVE_Z, 1.25);
		CHECK(result.size.is_equal_approx(Vector3(2, 4, 1.25)));
		CHECK(result.local_transform.origin.is_equal_approx(Vector3(0, 0, -3.625)));
		CHECK(result.local_transform.basis.is_equal_approx(Basis()));
	}

	SUBCASE("Depth clamps to the box minimum") {
		const CSGExtrusionResult result = csg_extrude_box_face(source_size, CSGBox3D::SURFACE_NEGATIVE_Y, 0.0);
		CHECK(result.size.is_equal_approx(Vector3(2, 0.001, 6)));
		CHECK(result.local_transform.origin.is_equal_approx(Vector3(0, -2.0005, 0)));
		CHECK(result.local_transform.basis.is_equal_approx(Basis()));
	}
}

TEST_CASE("[Editor][CSGEditDomain] Provider removal notification exits a CSG session") {
	EditorEditDomainRegistry *registry = EditorEditDomainRegistry::get_singleton();
	REQUIRE(registry != nullptr);
	HeadlessCSGEditDomainProvider provider;
	REQUIRE(registry->register_provider(&provider));

	EditorEditDomainHost host;
	REQUIRE(host.enter_domain(provider.get_domain_id(), nullptr));
	CHECK(host.is_active());
	CHECK(static_cast<CSGSurfaceSession *>(host.get_active_session())->get_active_root_id().is_null());
	CHECK(registry->unregister_provider(provider.get_domain_id(), &provider));
	host.notify_provider_unregistered(&provider);
	CHECK_FALSE(host.is_active());
	CHECK(host.get_active_session() == nullptr);
}

} // namespace TestCSGEditDomain

#endif // TOOLS_ENABLED
