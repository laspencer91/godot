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

#include "core/object/undo_redo.h"
#include "editor/gui/editor_edit_domain.h"
#include "scene/3d/node_3d.h"
#include "scene/gui/button.h"
#include "scene/main/scene_tree.h"
#include "scene/main/window.h"

#ifdef DEV_ENABLED
#include "modules/csg/csg_debug_counters.h"
#endif // DEV_ENABLED
#include "modules/csg/editor/csg_edit_domain.h"

namespace TestCSGEditDomain {

class HeadlessCSGEditDomainProvider : public CSGEditDomainProvider {
public:
	virtual StringName get_domain_id() const override { return SNAME("headless_csg_surface"); }
	virtual bool is_available(const EditorEditDomainContext &p_context) const override { return true; }
};

struct CSGEditDomainSynchronousSchedulerScope {
	CSGEditDomainSynchronousSchedulerScope() {
		CSGShape3D::set_async_evaluation_force_synchronous(true);
	}

	~CSGEditDomainSynchronousSchedulerScope() {
		CSGShape3D::set_async_evaluation_force_synchronous(false);
	}
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

TEST_CASE("[Editor][CSGEditDomain] Tool mode keeps Paint out of the Tab cycle") {
	CSGSurfaceSession session;
	CHECK(session.get_tool_mode() == CSGSurfaceSession::ToolMode::SURFACE);

	Control *rail = session.build_tool_rail();
	REQUIRE(rail != nullptr);
	REQUIRE_EQ(rail->get_child_count(), 4);
	Button *surface_button = Object::cast_to<Button>(rail->get_child(0));
	Button *draw_button = Object::cast_to<Button>(rail->get_child(1));
	Button *paint_button = Object::cast_to<Button>(rail->get_child(2));
	Button *operand_button = Object::cast_to<Button>(rail->get_child(3));
	REQUIRE(surface_button != nullptr);
	REQUIRE(draw_button != nullptr);
	REQUIRE(paint_button != nullptr);
	REQUIRE(operand_button != nullptr);
	CHECK(surface_button->is_pressed());
	CHECK(draw_button->is_disabled());
	CHECK_FALSE(paint_button->is_disabled());
	CHECK_FALSE(operand_button->is_disabled());

	paint_button->emit_signal(SNAME("pressed"));
	CHECK(session.get_tool_mode() == CSGSurfaceSession::ToolMode::PAINT);
	CHECK(paint_button->is_pressed());
	CHECK(session.handle_tool_toggle());
	CHECK(session.get_tool_mode() == CSGSurfaceSession::ToolMode::OPERAND);
	CHECK(operand_button->is_pressed());
	CHECK_FALSE(paint_button->is_pressed());
	CHECK(session.handle_tool_toggle());
	CHECK(session.get_tool_mode() == CSGSurfaceSession::ToolMode::SURFACE);
	CHECK(surface_button->is_pressed());

	memdelete(rail);
}

TEST_CASE("[Editor][CSGEditDomain] Paint applies to the semantic source in one undo action") {
	CSGEditDomainSynchronousSchedulerScope synchronous_scope;
	CSGBox3D *box = memnew(CSGBox3D);
	Ref<StandardMaterial3D> inherited_material;
	inherited_material.instantiate();
	box->set_material(inherited_material);
	SceneTree::get_singleton()->get_root()->add_child(box);
	box->update_shape();
	box->set_material(Ref<Material>());
	box->update_shape();

	CSGSurfaceKey surface;
	REQUIRE(box->get_surface_key(CSGBox3D::SURFACE_POSITIVE_Z, surface));
	CSGSurfaceSession lift_session;
	REQUIRE(lift_session.lift_paint_setting(surface));
	CHECK_EQ(lift_session.get_paint_well().material.ptr(), inherited_material.ptr());
	const uint32_t schema_generation = box->get_surface_schema_generation();
	Ref<StandardMaterial3D> material;
	material.instantiate();
	CSGSurfaceSetting setting;
	setting.material = material;
	setting.uv_mode = CSGPrimitive3D::SURFACE_UV_MODE_PLANAR;
	setting.uv_space = CSGPrimitive3D::SURFACE_UV_SPACE_ROOT;
	setting.meters_per_tile = Vector2(2.5, 3.5);
	setting.offset = Vector2(0.25, -0.75);
	setting.rotation = Math::deg_to_rad(12.0);
	setting.texture_lock = true;
	Vector<CSGSurfaceKey> surfaces;
	surfaces.push_back(surface);
	Node *unrelated_edited_root = memnew(Node);
	UndoRedo *rejected_undo_redo = memnew(UndoRedo);
	CHECK_FALSE(csg_paint_surfaces_with_undo(rejected_undo_redo, box, unrelated_edited_root, surfaces, setting));
	CHECK_EQ(rejected_undo_redo->get_history_count(), 0);
	memdelete(rejected_undo_redo);
	memdelete(unrelated_edited_root);

#ifdef DEV_ENABLED
	CSGDebugCounters::reset();
#endif // DEV_ENABLED
	UndoRedo *undo_redo = memnew(UndoRedo);
	CHECK(csg_paint_surfaces_with_undo(undo_redo, box, box, surfaces, setting));
	CHECK_EQ(undo_redo->get_history_count(), 1);
	REQUIRE(box->has_surface_setting(CSGBox3D::SURFACE_POSITIVE_Z));
	CHECK(box->get_surface_setting(CSGBox3D::SURFACE_POSITIVE_Z) == setting);
	CHECK_EQ(box->get_surface_schema_generation(), schema_generation);

	REQUIRE(undo_redo->undo());
	CHECK_FALSE(box->has_surface_setting(CSGBox3D::SURFACE_POSITIVE_Z));
	CHECK_EQ(box->get_surface_schema_generation(), schema_generation);
	REQUIRE(undo_redo->redo());
	CHECK(box->get_surface_setting(CSGBox3D::SURFACE_POSITIVE_Z) == setting);
	CHECK_EQ(box->get_surface_schema_generation(), schema_generation);
#ifdef DEV_ENABLED
	CHECK_EQ(CSGDebugCounters::get().batch_boolean_calls, 0);
#endif // DEV_ENABLED

	memdelete(undo_redo);
	box->queue_free();
}

TEST_CASE("[Editor][CSGEditDomain] Eyedropper lift applies to selected surfaces atomically") {
	CSGEditDomainSynchronousSchedulerScope synchronous_scope;
	CSGCombiner3D *root = memnew(CSGCombiner3D);
	SceneTree::get_singleton()->get_root()->add_child(root);
	CSGBox3D *source = memnew(CSGBox3D);
	root->add_child(source);
	source->set_owner(root);
	CSGBox3D *target = memnew(CSGBox3D);
	target->set_position(Vector3(2, 0, 0));
	root->add_child(target);
	target->set_owner(root);

	Ref<StandardMaterial3D> lifted_material;
	lifted_material.instantiate();
	CSGSurfaceSetting lifted_setting;
	lifted_setting.material = lifted_material;
	lifted_setting.uv_mode = CSGPrimitive3D::SURFACE_UV_MODE_PLANAR;
	lifted_setting.uv_space = CSGPrimitive3D::SURFACE_UV_SPACE_WORLD;
	lifted_setting.meters_per_tile = Vector2(4, 2);
	lifted_setting.offset = Vector2(-0.5, 0.125);
	lifted_setting.rotation = Math::deg_to_rad(-25.0);
	lifted_setting.texture_lock = true;
	source->set_surface_setting(CSGBox3D::SURFACE_NEGATIVE_X, lifted_setting);
	root->update_shape();

	CSGSurfaceKey source_surface;
	CSGSurfaceKey target_surface;
	REQUIRE(source->get_surface_key(CSGBox3D::SURFACE_NEGATIVE_X, source_surface));
	REQUIRE(target->get_surface_key(CSGBox3D::SURFACE_POSITIVE_Y, target_surface));
	const uint32_t source_schema_generation = source->get_surface_schema_generation();
	const uint32_t target_schema_generation = target->get_surface_schema_generation();

	CSGSurfaceSession session;
	REQUIRE(session.lift_paint_setting(source_surface));
	CHECK(session.get_paint_well() == lifted_setting);
	session.select_paint_surface(source_surface, false);
	session.select_paint_surface(target_surface, true);
	REQUIRE_EQ(session.get_paint_selection().size(), 2);

	UndoRedo *undo_redo = memnew(UndoRedo);
	CHECK(csg_paint_surfaces_with_undo(undo_redo, root, root, session.get_paint_selection(), session.get_paint_well()));
	CHECK_EQ(undo_redo->get_history_count(), 1);
	CHECK_EQ(session.get_paint_selection().size(), 2);
	CHECK(CSGShape3D::is_surface_key_valid(session.get_paint_selection()[0]));
	CHECK(CSGShape3D::is_surface_key_valid(session.get_paint_selection()[1]));
	CHECK(target->get_surface_setting(CSGBox3D::SURFACE_POSITIVE_Y) == lifted_setting);
	CHECK_EQ(source->get_surface_schema_generation(), source_schema_generation);
	CHECK_EQ(target->get_surface_schema_generation(), target_schema_generation);

	REQUIRE(undo_redo->undo());
	CHECK(source->get_surface_setting(CSGBox3D::SURFACE_NEGATIVE_X) == lifted_setting);
	CHECK_FALSE(target->has_surface_setting(CSGBox3D::SURFACE_POSITIVE_Y));
	CHECK_EQ(source->get_surface_schema_generation(), source_schema_generation);
	CHECK_EQ(target->get_surface_schema_generation(), target_schema_generation);
	REQUIRE(undo_redo->redo());
	CHECK(target->get_surface_setting(CSGBox3D::SURFACE_POSITIVE_Y) == lifted_setting);

	memdelete(undo_redo);
	root->queue_free();
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

TEST_CASE("[Editor][CSGEditDomain] Local planar texture lock compensates a one-sided push pull") {
	CSGBox3D *box = memnew(CSGBox3D);
	CSGSurfaceSetting setting;
	setting.uv_mode = CSGPrimitive3D::SURFACE_UV_MODE_PLANAR;
	setting.uv_space = CSGPrimitive3D::SURFACE_UV_SPACE_LOCAL;
	setting.meters_per_tile = Vector2(2, 4);
	setting.offset = Vector2(0.25, -0.5);
	setting.texture_lock = true;

	const Vector3 start_size(2, 2, 2);
	const Transform3D start_transform;
	const CSGPushPullResult result = csg_push_pull_apply(start_size, start_transform, CSGBox3D::SURFACE_POSITIVE_X, 2.0, false);
	const Vector3 center_shift_local = start_transform.basis.inverse().xform(result.transform.origin - start_transform.origin);
	const Vector2 compensated = csg_texture_lock_compensate_offset(box, CSGBox3D::SURFACE_POSITIVE_Z, setting, Transform3D(), center_shift_local);
	CHECK(compensated.is_equal_approx(Vector2(0.75, -0.5)));

	const real_t fixed_face_u_before = -start_size.x * 0.5 / setting.meters_per_tile.x + setting.offset.x;
	const real_t fixed_face_u_after = -result.size.x * 0.5 / setting.meters_per_tile.x + compensated.x;
	CHECK(fixed_face_u_after == doctest::Approx(fixed_face_u_before));

	setting.uv_space = CSGPrimitive3D::SURFACE_UV_SPACE_ROOT;
	CHECK_EQ(csg_texture_lock_compensate_offset(box, CSGBox3D::SURFACE_POSITIVE_Z, setting, Transform3D(), center_shift_local), setting.offset);
	setting.uv_space = CSGPrimitive3D::SURFACE_UV_SPACE_WORLD;
	CHECK_EQ(csg_texture_lock_compensate_offset(box, CSGBox3D::SURFACE_POSITIVE_Z, setting, Transform3D(), center_shift_local), setting.offset);
	setting.uv_space = CSGPrimitive3D::SURFACE_UV_SPACE_LOCAL;
	setting.texture_lock = false;
	CHECK_EQ(csg_texture_lock_compensate_offset(box, CSGBox3D::SURFACE_POSITIVE_Z, setting, Transform3D(), center_shift_local), setting.offset);
	memdelete(box);
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

TEST_CASE("[Editor][CSGEditDomain] Extrusion inherits cap and side settings in one structural action") {
	Ref<StandardMaterial3D> node_material;
	node_material.instantiate();
	Ref<StandardMaterial3D> face_material;
	face_material.instantiate();

	CSGBox3D *source = memnew(CSGBox3D);
	source->set_size(Vector3(2, 4, 6));
	source->set_material(node_material);
	CSGSurfaceSetting source_setting;
	source_setting.material = face_material;
	source_setting.uv_mode = CSGPrimitive3D::SURFACE_UV_MODE_PLANAR;
	source_setting.uv_space = CSGPrimitive3D::SURFACE_UV_SPACE_LOCAL;
	source_setting.meters_per_tile = Vector2(2.5, 3.5);
	source_setting.offset = Vector2(0.125, -0.75);
	source_setting.rotation = Math::deg_to_rad(17.0);
	source_setting.texture_lock = true;
	source->set_surface_setting(CSGBox3D::SURFACE_POSITIVE_X, source_setting);

	const CSGExtrusionResult result = csg_extrude_box_face(source->get_size(), CSGBox3D::SURFACE_POSITIVE_X, 1.5);
	CSGBox3D *extrusion = memnew(CSGBox3D);
	extrusion->set_size(result.size);
	extrusion->set_transform(result.local_transform);
	csg_configure_extrusion_surface_settings(source, CSGBox3D::SURFACE_POSITIVE_X, Transform3D(), extrusion);

	CHECK_EQ(extrusion->get_material().ptr(), face_material.ptr());
	REQUIRE(extrusion->has_surface_setting(CSGBox3D::SURFACE_POSITIVE_X));
	const CSGSurfaceSetting cap_setting = extrusion->get_surface_setting(CSGBox3D::SURFACE_POSITIVE_X);
	CHECK_EQ(cap_setting.material.ptr(), face_material.ptr());
	CHECK_EQ(cap_setting.uv_mode, source_setting.uv_mode);
	CHECK_EQ(cap_setting.uv_space, source_setting.uv_space);
	CHECK_EQ(cap_setting.meters_per_tile, source_setting.meters_per_tile);
	CHECK(cap_setting.offset.is_equal_approx(source_setting.offset));
	CHECK(cap_setting.rotation == doctest::Approx(source_setting.rotation));
	CHECK_EQ(cap_setting.texture_lock, source_setting.texture_lock);
	CHECK_FALSE(extrusion->has_surface_setting(CSGBox3D::SURFACE_NEGATIVE_X));

	int side_setting_count = 0;
	for (uint32_t surface = 0; surface < CSGBox3D::SURFACE_COUNT; surface++) {
		if (surface == CSGBox3D::SURFACE_POSITIVE_X || surface == CSGBox3D::SURFACE_NEGATIVE_X) {
			continue;
		}
		REQUIRE(extrusion->has_surface_setting(surface));
		const CSGSurfaceSetting side_setting = extrusion->get_surface_setting(surface);
		CHECK(side_setting.material.is_null());
		CHECK_EQ(side_setting.uv_mode, CSGPrimitive3D::SURFACE_UV_MODE_PLANAR);
		CHECK_EQ(side_setting.uv_space, CSGPrimitive3D::SURFACE_UV_SPACE_ROOT);
		CHECK_EQ(side_setting.meters_per_tile, source_setting.meters_per_tile);
		side_setting_count++;
	}
	CHECK_EQ(side_setting_count, 4);

	UndoRedo *undo_redo = memnew(UndoRedo);
	undo_redo->create_action("CSG Extrude Face");
	undo_redo->add_do_method(Callable(source, SNAME("add_child")).bind(extrusion));
	undo_redo->add_undo_method(Callable(source, SNAME("remove_child")).bind(extrusion));
	undo_redo->add_do_reference(extrusion);
	undo_redo->commit_action();
	CHECK_EQ(source->get_child_count(), 1);
	CHECK_EQ(source->get_child(0), extrusion);
	REQUIRE(undo_redo->undo());
	CHECK_EQ(source->get_child_count(), 0);
	CHECK(extrusion->has_surface_setting(CSGBox3D::SURFACE_POSITIVE_X));
	REQUIRE(undo_redo->redo());
	CHECK_EQ(source->get_child_count(), 1);
	CHECK_EQ(source->get_child(0), extrusion);
	CHECK_EQ(extrusion->get_surface_setting(CSGBox3D::SURFACE_POSITIVE_X).material.ptr(), face_material.ptr());

	REQUIRE(undo_redo->undo());
	memdelete(undo_redo);
	memdelete(source);
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
