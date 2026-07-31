/**************************************************************************/
/*  csg_edit_domain.cpp                                                   */
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

#include "csg_edit_domain.h"

#include "core/config/project_settings.h"
#include "core/input/input.h"
#include "core/input/input_event.h"
#include "core/object/callable_mp.h"
#include "editor/editor_data.h"
#include "editor/editor_document.h"
#include "editor/editor_node.h"
#include "editor/editor_undo_redo_manager.h"
#include "editor/inspector/editor_resource_picker.h"
#include "editor/scene/3d/node_3d_editor_plugin.h"
#include "editor/scene/3d/node_3d_editor_viewport.h"
#include "scene/3d/camera_3d.h"
#include "scene/gui/box_container.h"
#include "scene/gui/button.h"
#include "scene/gui/check_box.h"
#include "scene/gui/label.h"
#include "scene/gui/line_edit.h"
#include "scene/gui/option_button.h"
#include "scene/gui/panel_container.h"
#include "scene/gui/spin_box.h"

static bool _get_box_surface_axis(uint32_t p_surface, int &r_axis, real_t &r_sign, Vector3 &r_outward) {
	r_outward = Vector3();
	switch (p_surface) {
		case CSGBox3D::SURFACE_POSITIVE_X:
			r_axis = Vector3::AXIS_X;
			r_sign = 1.0;
			break;
		case CSGBox3D::SURFACE_NEGATIVE_X:
			r_axis = Vector3::AXIS_X;
			r_sign = -1.0;
			break;
		case CSGBox3D::SURFACE_POSITIVE_Y:
			r_axis = Vector3::AXIS_Y;
			r_sign = 1.0;
			break;
		case CSGBox3D::SURFACE_NEGATIVE_Y:
			r_axis = Vector3::AXIS_Y;
			r_sign = -1.0;
			break;
		case CSGBox3D::SURFACE_POSITIVE_Z:
			r_axis = Vector3::AXIS_Z;
			r_sign = 1.0;
			break;
		case CSGBox3D::SURFACE_NEGATIVE_Z:
			r_axis = Vector3::AXIS_Z;
			r_sign = -1.0;
			break;
		default:
			return false;
	}
	r_outward[r_axis] = r_sign;
	return true;
}

CSGPushPullResult csg_push_pull_apply(const Vector3 &p_start_size, const Transform3D &p_start_transform, uint32_t p_semantic_surface, real_t p_displacement, bool p_symmetric) {
	CSGPushPullResult result;
	result.size = p_start_size;
	result.transform = p_start_transform;

	int axis = 0;
	real_t sign = 1.0;
	Vector3 outward;
	if (!_get_box_surface_axis(p_semantic_surface, axis, sign, outward)) {
		return result;
	}

	const real_t size_multiplier = p_symmetric ? 2.0 : 1.0;
	const real_t unclamped_size = p_start_size[axis] + p_displacement * size_multiplier;
	result.size[axis] = MAX(unclamped_size, (real_t)0.001);
	const real_t effective_displacement = (result.size[axis] - p_start_size[axis]) / size_multiplier;
	if (!p_symmetric) {
		result.transform.origin += p_start_transform.basis.xform(outward * (effective_displacement * 0.5));
	}
	return result;
}

Vector2 csg_texture_lock_compensate_offset(const CSGPrimitive3D *p_primitive, uint32_t p_semantic_surface, const CSGSurfaceSetting &p_setting, const Transform3D &p_operand_to_root, const Vector3 &p_center_shift_root) {
	if (!p_primitive || p_setting.uv_mode != CSGPrimitive3D::SURFACE_UV_MODE_PLANAR || p_setting.uv_space != CSGPrimitive3D::SURFACE_UV_SPACE_LOCAL || !p_setting.texture_lock) {
		return p_setting.offset;
	}

	Vector3 scaled_u;
	Vector3 scaled_v;
	p_primitive->get_surface_planar_uv_axes(p_semantic_surface, p_setting, scaled_u, scaled_v);
	const Vector3 resolved_u = p_operand_to_root.basis.xform(scaled_u);
	const Vector3 resolved_v = p_operand_to_root.basis.xform(scaled_v);
	return p_setting.offset + Vector2(resolved_u.dot(p_center_shift_root), resolved_v.dot(p_center_shift_root));
}

// CSG-5: Keep the inner cap flush and extend an identity-basis child outward.
CSGExtrusionResult csg_extrude_box_face(const Vector3 &p_source_size, uint32_t p_semantic_surface, real_t p_depth) {
	CSGExtrusionResult result;
	result.size = p_source_size;
	result.local_transform = Transform3D();

	int axis = 0;
	real_t sign = 1.0;
	Vector3 outward;
	if (!_get_box_surface_axis(p_semantic_surface, axis, sign, outward)) {
		return result;
	}

	result.size[axis] = MAX(p_depth, (real_t)0.001);
	Vector3 center_local;
	center_local[axis] = sign * (p_source_size[axis] * 0.5 + result.size[axis] * 0.5);
	result.local_transform = Transform3D(Basis(), center_local);
	return result;
}

CSGDrawRect csg_draw_rectangle_bounds(const Vector2 &p_a, const Vector2 &p_b, real_t p_min_extent) {
	// CSG-7: Normalize corner order before testing either workplane extent.
	CSGDrawRect result;
	result.min = Vector2(MIN(p_a.x, p_b.x), MIN(p_a.y, p_b.y));
	result.max = Vector2(MAX(p_a.x, p_b.x), MAX(p_a.y, p_b.y));
	const real_t minimum_extent = MAX(p_min_extent, (real_t)0.0);
	result.degenerate = result.max.x - result.min.x < minimum_extent || result.max.y - result.min.y < minimum_extent;
	return result;
}

CSGDrawBoxResult csg_draw_box_from_rect(const CSGDrawRect &p_rect, real_t p_height, const Vector3 &p_plane_origin, const Vector3 &p_plane_u, const Vector3 &p_plane_normal, const Vector3 &p_plane_v) {
	// CSG-7: The authored box sits on the plane and grows only along its positive normal.
	CSGDrawBoxResult result;
	const real_t height = MAX(p_height, (real_t)0.001);
	result.size = Vector3(p_rect.max.x - p_rect.min.x, height, p_rect.max.y - p_rect.min.y);
	const Vector2 rect_center = (p_rect.min + p_rect.max) * 0.5;
	const Vector3 world_center = p_plane_origin + p_plane_u * rect_center.x + p_plane_v * rect_center.y + p_plane_normal * (height * 0.5);
	result.world_transform = Transform3D(Basis(p_plane_u, p_plane_normal, p_plane_v), world_center);
	return result;
}

void csg_configure_extrusion_surface_settings(CSGBox3D *p_source, uint32_t p_source_surface, const Transform3D &p_source_to_root, CSGBox3D *r_extrusion) {
	ERR_FAIL_NULL(p_source);
	ERR_FAIL_NULL(r_extrusion);
	ERR_FAIL_COND(p_source_surface >= CSGBox3D::SURFACE_COUNT);

	CSGSurfaceSetting source_setting;
	if (p_source->has_surface_setting(p_source_surface)) {
		source_setting = p_source->get_surface_setting(p_source_surface);
	}
	const Ref<Material> source_material = p_source->get_resolved_surface_material(p_source_surface);
	r_extrusion->set_material(source_material);

	CSGSurfaceSetting cap_setting = source_setting;
	cap_setting.material = source_material;
	if (cap_setting.uv_mode == CSGPrimitive3D::SURFACE_UV_MODE_PLANAR && cap_setting.uv_space == CSGPrimitive3D::SURFACE_UV_SPACE_LOCAL) {
		// The cap's new operand-local frame is translated from the source frame.
		// Preserve alignment at creation regardless of whether future edits are locked.
		CSGSurfaceSetting alignment_setting = cap_setting;
		alignment_setting.texture_lock = true;
		const Vector3 center_shift_root = p_source_to_root.basis.xform(r_extrusion->get_transform().origin);
		cap_setting.offset = csg_texture_lock_compensate_offset(p_source, p_source_surface, alignment_setting, p_source_to_root, center_shift_root);
	}
	r_extrusion->set_surface_setting(p_source_surface, cap_setting);

	const uint32_t joining_surface = p_source_surface ^ 1;
	for (uint32_t surface = 0; surface < CSGBox3D::SURFACE_COUNT; surface++) {
		if (surface == p_source_surface || surface == joining_surface) {
			continue;
		}
		CSGSurfaceSetting side_setting;
		side_setting.uv_mode = CSGPrimitive3D::SURFACE_UV_MODE_PLANAR;
		side_setting.uv_space = CSGPrimitive3D::SURFACE_UV_SPACE_ROOT;
		side_setting.meters_per_tile = source_setting.meters_per_tile;
		r_extrusion->set_surface_setting(surface, side_setting);
	}
}

static CSGShape3D *_get_single_selected_csg_shape(const EditorEditDomainContext *p_context = nullptr) {
	EditorSelection *selection = p_context && p_context->document ? p_context->document->get_selection() : nullptr;
	if (!selection) {
		EditorNode *editor_node = EditorNode::get_singleton();
		selection = editor_node ? editor_node->get_editor_selection() : nullptr;
	}
	if (!selection) {
		return nullptr;
	}
	const List<Node *> selected = selection->get_full_selected_node_list();
	if (selected.size() != 1) {
		return nullptr;
	}
	return Object::cast_to<CSGShape3D>(selected.front()->get());
}

static CSGShape3D *_find_csg_root(CSGShape3D *p_shape) {
	CSGShape3D *root = p_shape;
	while (root) {
		CSGShape3D *parent = Object::cast_to<CSGShape3D>(root->get_parent());
		if (!parent) {
			break;
		}
		root = parent;
	}
	return root;
}

struct CSGPaintUndoTarget {
	CSGPrimitive3D *source = nullptr;
	uint32_t surface = 0;
	CSGSurfaceSetting previous_setting;
};

static bool _is_csg_source_editable(const CSGPrimitive3D *p_source, const Node *p_edited_root) {
	return p_source && p_edited_root && (p_source == p_edited_root || p_source->get_owner() == p_edited_root);
}

static void _csg_draw_create_action(EditorUndoRedoManager *p_undo_redo, Node *p_edited_root) {
	// CSG-7: Preserve the native create path's per-document undo history in workspace panes.
	EditorNode *editor_node = EditorNode::get_singleton();
	EditorDocument *active_document = editor_node ? editor_node->get_editor_data().get_active_document() : nullptr;
	if (active_document && active_document->get_root() == p_edited_root && active_document->get_history_id() > 0) {
		p_undo_redo->create_action_for_history(TTR("CSG Draw Box"), active_document->get_history_id());
		return;
	}
	p_undo_redo->create_action(TTR("CSG Draw Box"), UndoRedo::MERGE_DISABLE, p_edited_root);
}

static void _csg_draw_create_action(UndoRedo *p_undo_redo, Node *) {
	p_undo_redo->create_action(TTR("CSG Draw Box"));
}

static void _csg_draw_add_child(EditorUndoRedoManager *p_undo_redo, Node *p_parent, CSGBox3D *p_box) {
	p_undo_redo->add_do_method(p_parent, "add_child", p_box, true);
}

static void _csg_draw_add_child(UndoRedo *p_undo_redo, Node *p_parent, CSGBox3D *p_box) {
	p_undo_redo->add_do_method(Callable(p_parent, SNAME("add_child")).bind(p_box, true));
}

static void _csg_draw_set_owner(EditorUndoRedoManager *p_undo_redo, CSGBox3D *p_box, Node *p_owner) {
	p_undo_redo->add_do_method(p_box, "set_owner", p_owner);
}

static void _csg_draw_set_owner(UndoRedo *p_undo_redo, CSGBox3D *p_box, Node *p_owner) {
	p_undo_redo->add_do_method(Callable(p_box, SNAME("set_owner")).bind(p_owner));
}

static void _csg_draw_remove_child(EditorUndoRedoManager *p_undo_redo, Node *p_parent, CSGBox3D *p_box) {
	p_undo_redo->add_undo_method(p_parent, "remove_child", p_box);
}

static void _csg_draw_remove_child(UndoRedo *p_undo_redo, Node *p_parent, CSGBox3D *p_box) {
	p_undo_redo->add_undo_method(Callable(p_parent, SNAME("remove_child")).bind(p_box));
}

static void _csg_draw_request_evaluation(EditorUndoRedoManager *p_undo_redo, CSGShape3D *p_root, bool p_undo) {
	if (p_undo) {
		p_undo_redo->add_undo_method(p_root, SNAME("_request_final_async_evaluation"));
	} else {
		p_undo_redo->add_do_method(p_root, SNAME("_request_final_async_evaluation"));
	}
}

static void _csg_draw_request_evaluation(UndoRedo *p_undo_redo, CSGShape3D *p_root, bool p_undo) {
	if (p_undo) {
		p_undo_redo->add_undo_method(Callable(p_root, SNAME("_request_final_async_evaluation")));
	} else {
		p_undo_redo->add_do_method(Callable(p_root, SNAME("_request_final_async_evaluation")));
	}
}

static void _csg_draw_set_global_transform(EditorUndoRedoManager *p_undo_redo, CSGBox3D *p_box, const Transform3D &p_transform) {
	p_undo_redo->add_do_method(p_box, "set_global_transform", p_transform);
}

static void _csg_draw_set_global_transform(UndoRedo *p_undo_redo, CSGBox3D *p_box, const Transform3D &p_transform) {
	p_undo_redo->add_do_method(Callable(p_box, SNAME("set_global_transform")).bind(p_transform));
}

static void _csg_draw_set_use_collision(EditorUndoRedoManager *p_undo_redo, CSGBox3D *p_box, bool p_enabled) {
	p_undo_redo->add_do_method(p_box, "set_use_collision", p_enabled);
}

static void _csg_draw_set_use_collision(UndoRedo *p_undo_redo, CSGBox3D *p_box, bool p_enabled) {
	p_undo_redo->add_do_method(Callable(p_box, SNAME("set_use_collision")).bind(p_enabled));
}

template <typename TUndoRedo>
static CSGBox3D *_csg_draw_commit_box_impl(TUndoRedo *p_undo_redo, CSGShape3D *p_root, Node *p_edited_root, CSGPrimitive3D *p_parent_operand, const CSGDrawBoxResult &p_box, bool p_cut, bool p_use_collision_for_new_root) {
	// CSG-7: Reject every invalid configuration before allocating or opening history.
	if (!p_undo_redo || !p_root || !p_edited_root || p_box.size.x <= CMP_EPSILON || p_box.size.y <= CMP_EPSILON || p_box.size.z <= CMP_EPSILON) {
		return nullptr;
	}
	if (p_parent_operand && (!_is_csg_source_editable(p_parent_operand, p_edited_root) || _find_csg_root(p_parent_operand) != p_root)) {
		return nullptr;
	}

	CSGBox3D *new_box = memnew(CSGBox3D);
	if (!p_parent_operand) {
		// Match the native Create CSG Box path before applying Draw-authored values.
		if (EditorNode *editor_node = EditorNode::get_singleton()) {
			editor_node->get_editor_data().instantiate_object_properties(new_box);
		}
	}
	String node_name = (p_parent_operand ? static_cast<Node *>(p_parent_operand) : p_edited_root)->validate_child_name(new_box);
	if (GLOBAL_GET("editor/naming/node_name_casing").operator int() != Node::NAME_CASING_PASCAL_CASE) {
		node_name = Node::adjust_name_casing(node_name);
	}
	new_box->set_name(node_name);
	new_box->set_size(p_box.size);

	if (p_parent_operand) {
		// A face draw authors one boolean child in the hit operand's local frame.
		new_box->set_operation(p_cut ? CSGShape3D::OPERATION_SUBTRACTION : CSGShape3D::OPERATION_UNION);
		new_box->set_transform(p_parent_operand->get_global_transform().affine_inverse() * p_box.world_transform);
		_csg_draw_create_action(p_undo_redo, p_edited_root);
		_csg_draw_add_child(p_undo_redo, p_parent_operand, new_box);
		_csg_draw_set_owner(p_undo_redo, new_box, p_edited_root);
		_csg_draw_remove_child(p_undo_redo, p_parent_operand, new_box);
		p_undo_redo->add_do_reference(new_box);
		_csg_draw_request_evaluation(p_undo_redo, p_root, false);
		_csg_draw_request_evaluation(p_undo_redo, p_root, true);
		p_undo_redo->commit_action();
		return new_box;
	}

	// A ground-plane draw is a new standalone root, so its own operation is always Union.
	new_box->set_operation(CSGShape3D::OPERATION_UNION);
	_csg_draw_create_action(p_undo_redo, p_edited_root);
	_csg_draw_add_child(p_undo_redo, p_edited_root, new_box);
	_csg_draw_set_owner(p_undo_redo, new_box, p_edited_root);
	_csg_draw_set_global_transform(p_undo_redo, new_box, p_box.world_transform);
	_csg_draw_set_use_collision(p_undo_redo, new_box, p_use_collision_for_new_root);
	p_undo_redo->add_do_reference(new_box);
	_csg_draw_remove_child(p_undo_redo, p_edited_root, new_box);
	_csg_draw_request_evaluation(p_undo_redo, new_box, false);
	p_undo_redo->commit_action();
	return new_box;
}

CSGBox3D *csg_draw_commit_box(EditorUndoRedoManager *p_undo_redo, CSGShape3D *p_root, Node *p_edited_root, CSGPrimitive3D *p_parent_operand, const CSGDrawBoxResult &p_box, bool p_cut, bool p_use_collision_for_new_root) {
	return _csg_draw_commit_box_impl(p_undo_redo, p_root, p_edited_root, p_parent_operand, p_box, p_cut, p_use_collision_for_new_root);
}

CSGBox3D *csg_draw_commit_box(UndoRedo *p_undo_redo, CSGShape3D *p_root, Node *p_edited_root, CSGPrimitive3D *p_parent_operand, const CSGDrawBoxResult &p_box, bool p_cut, bool p_use_collision_for_new_root) {
	return _csg_draw_commit_box_impl(p_undo_redo, p_root, p_edited_root, p_parent_operand, p_box, p_cut, p_use_collision_for_new_root);
}

static Vector<CSGPaintUndoTarget> _collect_csg_paint_targets(CSGShape3D *p_root, Node *p_edited_root, const Vector<CSGSurfaceKey> &p_surfaces, const CSGSurfaceSetting &p_setting) {
	Vector<CSGPaintUndoTarget> targets;
	if (!p_root || !p_edited_root || p_setting.uv_mode < CSGPrimitive3D::SURFACE_UV_MODE_LEGACY || p_setting.uv_mode > CSGPrimitive3D::SURFACE_UV_MODE_PLANAR || p_setting.uv_space < CSGPrimitive3D::SURFACE_UV_SPACE_LOCAL || p_setting.uv_space > CSGPrimitive3D::SURFACE_UV_SPACE_WORLD) {
		return targets;
	}

	for (const CSGSurfaceKey &surface : p_surfaces) {
		if (!CSGShape3D::is_surface_key_valid(surface)) {
			continue;
		}
		CSGPrimitive3D *source = ObjectDB::get_instance<CSGPrimitive3D>(surface.source_shape);
		if (!_is_csg_source_editable(source, p_edited_root) || _find_csg_root(source) != p_root) {
			continue;
		}

		bool duplicate = false;
		for (const CSGPaintUndoTarget &target : targets) {
			if (target.source == source && target.surface == surface.semantic_surface) {
				duplicate = true;
				break;
			}
		}
		if (duplicate) {
			continue;
		}

		const CSGSurfaceSetting previous_setting = source->get_surface_setting(surface.semantic_surface);
		if (previous_setting == p_setting) {
			continue;
		}
		CSGPaintUndoTarget target;
		target.source = source;
		target.surface = surface.semantic_surface;
		target.previous_setting = previous_setting;
		targets.push_back(target);
	}
	return targets;
}

template <typename TUndoRedo>
static void _add_csg_paint_properties(TUndoRedo *p_undo_redo, const CSGPaintUndoTarget &p_target, const CSGSurfaceSetting &p_setting) {
	const String prefix = vformat("surface_settings/%d/", p_target.surface);
	p_undo_redo->add_do_property(p_target.source, prefix + "material", p_setting.material);
	p_undo_redo->add_undo_property(p_target.source, prefix + "material", p_target.previous_setting.material);
	p_undo_redo->add_do_property(p_target.source, prefix + "uv_mode", p_setting.uv_mode);
	p_undo_redo->add_undo_property(p_target.source, prefix + "uv_mode", p_target.previous_setting.uv_mode);
	p_undo_redo->add_do_property(p_target.source, prefix + "uv_space", p_setting.uv_space);
	p_undo_redo->add_undo_property(p_target.source, prefix + "uv_space", p_target.previous_setting.uv_space);
	p_undo_redo->add_do_property(p_target.source, prefix + "meters_per_tile", p_setting.meters_per_tile);
	p_undo_redo->add_undo_property(p_target.source, prefix + "meters_per_tile", p_target.previous_setting.meters_per_tile);
	p_undo_redo->add_do_property(p_target.source, prefix + "offset", p_setting.offset);
	p_undo_redo->add_undo_property(p_target.source, prefix + "offset", p_target.previous_setting.offset);
	p_undo_redo->add_do_property(p_target.source, prefix + "rotation", p_setting.rotation);
	p_undo_redo->add_undo_property(p_target.source, prefix + "rotation", p_target.previous_setting.rotation);
	p_undo_redo->add_do_property(p_target.source, prefix + "texture_lock", p_setting.texture_lock);
	p_undo_redo->add_undo_property(p_target.source, prefix + "texture_lock", p_target.previous_setting.texture_lock);
}

bool csg_paint_surfaces_with_undo(UndoRedo *p_undo_redo, CSGShape3D *p_root, Node *p_edited_root, const Vector<CSGSurfaceKey> &p_surfaces, const CSGSurfaceSetting &p_setting, UndoRedo::MergeMode p_merge_mode, const String &p_action_name) {
	ERR_FAIL_NULL_V(p_undo_redo, false);
	const Vector<CSGPaintUndoTarget> targets = _collect_csg_paint_targets(p_root, p_edited_root, p_surfaces, p_setting);
	if (targets.is_empty()) {
		return false;
	}

	p_undo_redo->create_action(p_action_name, p_merge_mode);
	for (const CSGPaintUndoTarget &target : targets) {
		_add_csg_paint_properties(p_undo_redo, target, p_setting);
	}
	p_undo_redo->add_do_method(Callable(p_root, SNAME("_request_final_async_evaluation")));
	p_undo_redo->add_undo_method(Callable(p_root, SNAME("_request_final_async_evaluation")));
	p_undo_redo->commit_action();
	return true;
}

void CSGSurfaceSession::_resolve_active_root(const EditorEditDomainContext &p_context) {
	CSGShape3D *selected_shape = _get_single_selected_csg_shape(&p_context);
	CSGShape3D *root = _find_csg_root(selected_shape);
	active_root_id = root ? root->get_instance_id() : ObjectID();
}

void CSGSurfaceSession::_capture_edited_scene_root(const EditorEditDomainContext &p_context) {
	Node *edited_root = p_context.document ? p_context.document->get_root() : nullptr;
	if (!edited_root) {
		EditorNode *editor_node = EditorNode::get_singleton();
		edited_root = editor_node ? editor_node->get_edited_scene() : nullptr;
	}
	edited_scene_root_id = edited_root ? edited_root->get_instance_id() : ObjectID();
}

CSGShape3D *CSGSurfaceSession::_get_active_root() const {
	return ObjectDB::get_instance<CSGShape3D>(active_root_id);
}

CSGBox3D *CSGSurfaceSession::_get_active_box() const {
	return ObjectDB::get_instance<CSGBox3D>(active_box_id);
}

Node3DEditorViewport *CSGSurfaceSession::_get_active_viewport() const {
	return ObjectDB::get_instance<Node3DEditorViewport>(active_viewport_id);
}

Node *CSGSurfaceSession::_get_edited_scene_root() const {
	return ObjectDB::get_instance<Node>(edited_scene_root_id);
}

bool CSGSurfaceSession::_is_source_editable(const CSGPrimitive3D *p_source) const {
	return _is_csg_source_editable(p_source, _get_edited_scene_root());
}

void CSGSurfaceSession::_clear_pick_state() {
	pick_mesh.unref();
	pick_faces.clear();
	pick_mesh_generation = UINT64_MAX;
	hover_hit = CSGSurfaceHit();
	has_hover = false;
}

void CSGSurfaceSession::_clear_selection() {
	active_box_id = ObjectID();
	selected_hit = CSGSurfaceHit();
	has_selection = false;
	has_ghost = false;
	extrude_gesture = false; // CSG-5: Do not leak a canceled mode into numeric entry.
	gesture_state = has_hover ? GestureState::HOVER : GestureState::IDLE;
	_update_context_panel();
}

void CSGSurfaceSession::_set_tool_mode(ToolMode p_mode) {
	if (gesture_state == GestureState::PRESSED || gesture_state == GestureState::DRAGGING) {
		_cancel_gesture();
	}
	if (p_mode != tool_mode && (tool_mode == ToolMode::DRAW || p_mode == ToolMode::DRAW)) {
		// CSG-7: A tool switch abandons only the view-local Draw transaction.
		_reset_draw_state(false);
	}
	tool_mode = p_mode;
	if (tool_mode == ToolMode::OPERAND) {
		has_hover = false;
	}
	_update_tool_buttons();
	_update_context_panel();
	_queue_redraw(_get_active_viewport());
}

void CSGSurfaceSession::_update_tool_buttons() {
	if (Button *button = ObjectDB::get_instance<Button>(surface_tool_button_id)) {
		button->set_pressed_no_signal(tool_mode == ToolMode::SURFACE);
	}
	if (Button *button = ObjectDB::get_instance<Button>(draw_tool_button_id)) {
		button->set_pressed_no_signal(tool_mode == ToolMode::DRAW);
	}
	if (Button *button = ObjectDB::get_instance<Button>(paint_tool_button_id)) {
		button->set_pressed_no_signal(tool_mode == ToolMode::PAINT);
	}
	if (Button *button = ObjectDB::get_instance<Button>(operand_tool_button_id)) {
		button->set_pressed_no_signal(tool_mode == ToolMode::OPERAND);
	}
}

void CSGSurfaceSession::_prune_paint_selection() {
	for (int i = paint_selection.size() - 1; i >= 0; i--) {
		if (!CSGShape3D::is_surface_key_valid(paint_selection[i])) {
			paint_selection.remove_at(i);
		}
	}
}

bool CSGSurfaceSession::_paint_selection_has(const CSGSurfaceKey &p_surface) const {
	for (const CSGSurfaceKey &surface : paint_selection) {
		if (surface == p_surface) {
			return true;
		}
	}
	return false;
}

void CSGSurfaceSession::_select_paint_surface(const CSGSurfaceKey &p_surface, bool p_add) {
	_prune_paint_selection();
	if (!p_add) {
		paint_selection.clear();
	}
	if (CSGShape3D::is_surface_key_valid(p_surface) && !_paint_selection_has(p_surface)) {
		paint_selection.push_back(p_surface);
	}
	_update_context_panel();
}

bool CSGSurfaceSession::_apply_paint_to_surfaces(const Vector<CSGSurfaceKey> &p_surfaces, UndoRedo::MergeMode p_merge_mode, const String &p_action_name) {
	CSGShape3D *root = _get_active_root();
	Node *edited_root = _get_edited_scene_root();
	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	if (!root || !edited_root || !undo_redo) {
		return false;
	}

	const Vector<CSGPaintUndoTarget> targets = _collect_csg_paint_targets(root, edited_root, p_surfaces, paint_well);
	if (targets.is_empty()) {
		return false;
	}

	undo_redo->create_action(p_action_name, p_merge_mode, edited_root);
	for (const CSGPaintUndoTarget &target : targets) {
		_add_csg_paint_properties(undo_redo, target, paint_well);
	}
	undo_redo->add_do_method(root, SNAME("_request_final_async_evaluation"));
	undo_redo->add_undo_method(root, SNAME("_request_final_async_evaluation"));
	undo_redo->commit_action();
	return true;
}

bool CSGSurfaceSession::_lift_paint_setting(const CSGSurfaceKey &p_surface) {
	if (!CSGShape3D::is_surface_key_valid(p_surface)) {
		return false;
	}
	CSGPrimitive3D *source = ObjectDB::get_instance<CSGPrimitive3D>(p_surface.source_shape);
	if (!source) {
		return false;
	}
	paint_well = source->get_surface_setting(p_surface.semantic_surface);
	if (paint_well.material.is_null()) {
		paint_well.material = source->get_resolved_surface_material(p_surface.semantic_surface);
	}
	_update_paint_controls();
	return true;
}

void CSGSurfaceSession::_apply_well_to_selection(UndoRedo::MergeMode p_merge_mode, const String &p_action_name) {
	_prune_paint_selection();
	_apply_paint_to_surfaces(paint_selection, p_merge_mode, p_action_name);
}

void CSGSurfaceSession::_surface_tool_pressed() {
	_set_tool_mode(ToolMode::SURFACE);
}

void CSGSurfaceSession::_draw_tool_pressed() {
	_set_tool_mode(ToolMode::DRAW);
}

void CSGSurfaceSession::_paint_tool_pressed() {
	_set_tool_mode(ToolMode::PAINT);
}

void CSGSurfaceSession::_operand_tool_pressed() {
	_set_tool_mode(ToolMode::OPERAND);
}

void CSGSurfaceSession::_draw_add_pressed() {
	// CSG-7: Explicit operation state remains stable when Ctrl temporarily inverts it.
	draw_cut_mode = false;
	_update_context_panel();
	_queue_redraw(_get_active_viewport());
}

void CSGSurfaceSession::_draw_cut_pressed() {
	// CSG-7: Explicit operation state remains stable when Ctrl temporarily inverts it.
	draw_cut_mode = true;
	_update_context_panel();
	_queue_redraw(_get_active_viewport());
}

void CSGSurfaceSession::_paint_material_changed(Ref<Resource> p_resource) {
	if (updating_paint_controls) {
		return;
	}
	paint_well.material = p_resource;
	_apply_well_to_selection(UndoRedo::MERGE_DISABLE, TTR("CSG Paint Material"));
}

void CSGSurfaceSession::_paint_uv_mode_selected(int p_index) {
	if (updating_paint_controls) {
		return;
	}
	paint_well.uv_mode = p_index;
	_update_paint_controls();
	_apply_well_to_selection(UndoRedo::MERGE_DISABLE, TTR("CSG Change Projection"));
}

void CSGSurfaceSession::_paint_uv_space_selected(int p_index) {
	if (updating_paint_controls) {
		return;
	}
	paint_well.uv_space = p_index;
	_apply_well_to_selection(UndoRedo::MERGE_DISABLE, TTR("CSG Change Projection Space"));
}

void CSGSurfaceSession::_paint_numeric_changed(double) {
	if (updating_paint_controls) {
		return;
	}
	SpinBox *meters_u = ObjectDB::get_instance<SpinBox>(paint_meters_u_id);
	SpinBox *meters_v = ObjectDB::get_instance<SpinBox>(paint_meters_v_id);
	SpinBox *offset_u = ObjectDB::get_instance<SpinBox>(paint_offset_u_id);
	SpinBox *offset_v = ObjectDB::get_instance<SpinBox>(paint_offset_v_id);
	SpinBox *rotation = ObjectDB::get_instance<SpinBox>(paint_rotation_id);
	if (!meters_u || !meters_v || !offset_u || !offset_v || !rotation) {
		return;
	}
	paint_well.meters_per_tile = Vector2(meters_u->get_value(), meters_v->get_value());
	paint_well.offset = Vector2(offset_u->get_value(), offset_v->get_value());
	paint_well.rotation = Math::deg_to_rad(rotation->get_value());
	_apply_well_to_selection(UndoRedo::MERGE_ENDS, TTR("CSG Adjust Surface UV"));
}

void CSGSurfaceSession::_paint_texture_lock_toggled(bool p_pressed) {
	if (updating_paint_controls) {
		return;
	}
	paint_well.texture_lock = p_pressed;
	_apply_well_to_selection(UndoRedo::MERGE_DISABLE, TTR("CSG Toggle Texture Lock"));
}

void CSGSurfaceSession::_paint_assign_pressed() {
	_prune_paint_selection();
	if (!paint_selection.is_empty()) {
		_apply_paint_to_surfaces(paint_selection, UndoRedo::MERGE_DISABLE, TTR("CSG Assign Surface Settings"));
	} else if (has_hover) {
		Vector<CSGSurfaceKey> hovered_surface;
		hovered_surface.push_back(hover_hit.surface);
		_apply_paint_to_surfaces(hovered_surface, UndoRedo::MERGE_DISABLE, TTR("CSG Assign Surface Settings"));
	}
}

void CSGSurfaceSession::_paint_eyedropper_toggled(bool p_pressed) {
	paint_eyedropper_active = p_pressed;
}

void CSGSurfaceSession::_paint_align_face_pressed() {
	paint_well.uv_mode = CSGPrimitive3D::SURFACE_UV_MODE_PLANAR;
	paint_well.uv_space = CSGPrimitive3D::SURFACE_UV_SPACE_LOCAL;
	paint_well.offset = Vector2();
	paint_well.rotation = 0.0;
	_update_paint_controls();
	_apply_well_to_selection(UndoRedo::MERGE_DISABLE, TTR("CSG Align Surface UV"));
}

void CSGSurfaceSession::_paint_align_root_pressed() {
	paint_well.uv_mode = CSGPrimitive3D::SURFACE_UV_MODE_PLANAR;
	paint_well.uv_space = CSGPrimitive3D::SURFACE_UV_SPACE_ROOT;
	paint_well.offset = Vector2();
	paint_well.rotation = 0.0;
	_update_paint_controls();
	_apply_well_to_selection(UndoRedo::MERGE_DISABLE, TTR("CSG Align UV to Root Grid"));
}

void CSGSurfaceSession::_paint_fit_pressed() {
	_prune_paint_selection();
	CSGSurfaceKey surface;
	if (!paint_selection.is_empty()) {
		surface = paint_selection[0];
	} else if (has_hover) {
		surface = hover_hit.surface;
	} else {
		return;
	}
	if (!CSGShape3D::is_surface_key_valid(surface)) {
		return;
	}
	CSGPrimitive3D *primitive = ObjectDB::get_instance<CSGPrimitive3D>(surface.source_shape);
	if (!primitive) {
		return;
	}

	Vector3 axis_u;
	Vector3 axis_v;
	primitive->get_surface_uv_basis(surface.semantic_surface, axis_u, axis_v);
	const AABB bounds = primitive->get_aabb();
	real_t min_u = 0.0;
	real_t max_u = 0.0;
	real_t min_v = 0.0;
	real_t max_v = 0.0;
	for (int corner_i = 0; corner_i < 8; corner_i++) {
		const Vector3 corner = bounds.position + Vector3(
				(corner_i & 1) ? bounds.size.x : 0.0,
				(corner_i & 2) ? bounds.size.y : 0.0,
				(corner_i & 4) ? bounds.size.z : 0.0);
		const real_t u = axis_u.dot(corner);
		const real_t v = axis_v.dot(corner);
		if (corner_i == 0) {
			min_u = max_u = u;
			min_v = max_v = v;
		} else {
			min_u = MIN(min_u, u);
			max_u = MAX(max_u, u);
			min_v = MIN(min_v, v);
			max_v = MAX(max_v, v);
		}
	}
	paint_well.meters_per_tile = Vector2(MAX(max_u - min_u, (real_t)0.001), MAX(max_v - min_v, (real_t)0.001));
	paint_well.uv_mode = CSGPrimitive3D::SURFACE_UV_MODE_PLANAR;
	paint_well.uv_space = CSGPrimitive3D::SURFACE_UV_SPACE_LOCAL;
	paint_well.offset = Vector2();
	paint_well.rotation = 0.0;
	_update_paint_controls();
	_apply_well_to_selection(UndoRedo::MERGE_DISABLE, TTR("CSG Fit Surface UV"));
}

void CSGSurfaceSession::_paint_reset_pressed() {
	paint_well = CSGSurfaceSetting();
	_update_paint_controls();
	_apply_well_to_selection(UndoRedo::MERGE_DISABLE, TTR("CSG Reset Surface Settings"));
}

void CSGSurfaceSession::_paint_apply_selected_pressed() {
	_apply_well_to_selection(UndoRedo::MERGE_DISABLE, TTR("CSG Apply Surface Settings"));
}

void CSGSurfaceSession::_queue_redraw(Node3DEditorViewport *p_viewport) const {
	if (p_viewport) {
		p_viewport->update_surface();
	}
}

bool CSGSurfaceSession::_pick(Node3DEditorViewport *p_viewport, const Vector2 &p_position) {
	CSGShape3D *root = _get_active_root();
	if (!root || !p_viewport) {
		has_hover = false;
		return false;
	}

	uint64_t result_generation = root->get_result_generation();
	if (result_generation != pick_mesh_generation) {
		// Brush-face order is the result-triangle order. The render ArrayMesh is
		// deliberately not used because material grouping changes that order.
		pick_faces = root->get_brush_faces();
		result_generation = root->get_result_generation();
		pick_mesh.unref();
		if (!pick_faces.is_empty()) {
			pick_mesh.instantiate();
			pick_mesh->create(pick_faces);
		}
		pick_mesh_generation = result_generation;
	}

	if (pick_mesh.is_null() || !pick_mesh->is_valid()) {
		has_hover = false;
		return false;
	}

	const Transform3D root_inverse = root->get_global_transform().affine_inverse();
	const Vector3 ray_position = root_inverse.xform(p_viewport->get_ray_pos(p_position));
	const Vector3 ray_direction = root_inverse.basis.xform(p_viewport->get_ray(p_position)).normalized();
	Vector3 hit_position;
	Vector3 hit_normal;
	int32_t face_index = -1;
	if (!pick_mesh->intersect_ray(ray_position, ray_direction, hit_position, hit_normal, nullptr, &face_index) || face_index < 0) {
		has_hover = false;
		return false;
	}

	CSGSurfaceKey surface;
	uint32_t face_id = 0;
	if (!root->resolve_result_triangle((uint32_t)face_index, result_generation, surface, face_id)) {
		has_hover = false;
		return false;
	}

	hover_hit.surface = surface;
	hover_hit.result_generation = result_generation;
	hover_hit.face_id = face_id;
	hover_hit.triangle = (uint32_t)face_index;
	has_hover = true;
	return true;
}

bool CSGSurfaceSession::_pick_for_draw(Node3DEditorViewport *p_viewport, const Vector2 &p_position, Vector3 &r_hit_position_root, Vector3 &r_hit_normal_root) {
	// CSG-7: Preserve the established semantic pick path while retaining the geometric hit for a workplane.
	if (!_pick(p_viewport, p_position)) {
		return false;
	}
	CSGShape3D *root = _get_active_root();
	if (!root || pick_mesh.is_null() || !pick_mesh->is_valid()) {
		return false;
	}

	const Transform3D root_inverse = root->get_global_transform().affine_inverse();
	const Vector3 ray_position = root_inverse.xform(p_viewport->get_ray_pos(p_position));
	const Vector3 ray_direction = root_inverse.basis.xform(p_viewport->get_ray(p_position)).normalized();
	int32_t face_index = -1;
	if (!pick_mesh->intersect_ray(ray_position, ray_direction, r_hit_position_root, r_hit_normal_root, nullptr, &face_index) || face_index < 0 || (uint32_t)face_index != hover_hit.triangle) {
		return false;
	}
	return true;
}

void CSGSurfaceSession::_reset_draw_state(bool p_update) {
	// CSG-7: A fresh Draw transaction always starts in explicit Add mode.
	draw_phase = DrawPhase::IDLE;
	draw_cut_mode = false;
	draw_ctrl_pressed = false;
	draw_plane_origin_world = Vector3();
	draw_plane_normal_world = Vector3();
	draw_plane_u_world = Vector3();
	draw_plane_v_world = Vector3();
	draw_parent_operand_id = ObjectID();
	draw_rect_min = Vector2();
	draw_rect_max = Vector2();
	draw_first_corner_uv = Vector2();
	draw_height = 0.0;
	draw_height_line_origin_world = Vector3();
	draw_height_line_direction_world = Vector3();
	draw_height_start_parameter = 0.0;
	if (p_update) {
		_update_context_panel();
		_queue_redraw(_get_active_viewport());
	}
}

bool CSGSurfaceSession::_is_draw_cut_effective() const {
	const Node3DEditor *node_3d_editor = Node3DEditor::get_singleton();
	const bool ctrl_pressed = node_3d_editor ? node_3d_editor->is_raw_ctrl_pressed() : draw_ctrl_pressed;
	return draw_cut_mode ^ ctrl_pressed;
}

real_t CSGSurfaceSession::_active_translate_snap_step() const {
	Node3DEditor *node_3d_editor = Node3DEditor::get_singleton();
	if (node_3d_editor && node_3d_editor->is_snap_enabled(EditorSnapModifierEffect::NONE)) {
		return node_3d_editor->get_translate_snap(EditorSnapModifierEffect::NONE);
	}
	return 0.0;
}

bool CSGSurfaceSession::_project_draw_point(Node3DEditorViewport *p_viewport, const Vector2 &p_position, Vector2 &r_plane_position) const {
	if (!p_viewport || draw_plane_normal_world.is_zero_approx()) {
		return false;
	}
	Vector3 intersection;
	const Vector3 ray_direction = p_viewport->get_ray(p_position).normalized();
	if (!Plane(draw_plane_normal_world, draw_plane_origin_world).intersects_ray(p_viewport->get_ray_pos(p_position), ray_direction, &intersection)) {
		return false;
	}

	const Vector3 offset = intersection - draw_plane_origin_world;
	r_plane_position = Vector2(offset.dot(draw_plane_u_world), offset.dot(draw_plane_v_world));
	const real_t snap_step = _active_translate_snap_step();
	if (snap_step > 0.0) {
		r_plane_position.x = Math::snapped(r_plane_position.x, snap_step);
		r_plane_position.y = Math::snapped(r_plane_position.y, snap_step);
	}
	return true;
}

bool CSGSurfaceSession::_resolve_draw_plane(Node3DEditorViewport *p_viewport, const Vector2 &p_position) {
	CSGShape3D *root = _get_active_root();
	if (!root || !p_viewport) {
		return false;
	}

	Vector3 hit_position_root;
	Vector3 hit_normal_root;
	if (_pick_for_draw(p_viewport, p_position, hit_position_root, hit_normal_root)) {
		CSGPrimitive3D *source = ObjectDB::get_instance<CSGPrimitive3D>(hover_hit.surface.source_shape);
		if (!source || _find_csg_root(source) != root) {
			return false;
		}
		draw_parent_operand_id = source->get_instance_id();
		draw_plane_origin_world = root->get_global_transform().xform(hit_position_root);
		draw_plane_normal_world = root->get_global_transform().basis.inverse().transposed().xform(hit_normal_root).normalized();

		// An authored box face supplies an exact axis even when the visible fragment is triangulated.
		if (CSGBox3D *box = Object::cast_to<CSGBox3D>(source)) {
			int axis = 0;
			real_t sign = 1.0;
			Vector3 outward;
			if (_get_box_surface_axis(hover_hit.surface.semantic_surface, axis, sign, outward)) {
				draw_plane_normal_world = box->get_global_transform().basis.inverse().transposed().xform(outward).normalized();
			}
		}
		if (draw_plane_normal_world.is_zero_approx()) {
			return false;
		}

		Vector3 seed;
		const Vector3 abs_normal = draw_plane_normal_world.abs();
		if (abs_normal.x <= abs_normal.y && abs_normal.x <= abs_normal.z) {
			seed = Vector3(1, 0, 0);
		} else if (abs_normal.y <= abs_normal.z) {
			seed = Vector3(0, 1, 0);
		} else {
			seed = Vector3(0, 0, 1);
		}
		draw_plane_u_world = seed.cross(draw_plane_normal_world).normalized();
		draw_plane_v_world = draw_plane_normal_world.cross(draw_plane_u_world).normalized();
	} else {
		// No visible CSG hit means the world XZ ground plane and a future standalone root.
		draw_parent_operand_id = ObjectID();
		draw_plane_origin_world = Vector3();
		draw_plane_normal_world = Vector3(0, 1, 0);
		draw_plane_u_world = Vector3(1, 0, 0);
		draw_plane_v_world = Vector3(0, 0, 1);
	}

	Vector2 first_corner;
	if (!_project_draw_point(p_viewport, p_position, first_corner)) {
		return false;
	}
	draw_first_corner_uv = first_corner;
	draw_rect_min = first_corner;
	draw_rect_max = first_corner;
	draw_height = 0.0;
	draw_phase = DrawPhase::RECTANGLE;
	_update_context_panel();
	_queue_redraw(p_viewport);
	return true;
}

void CSGSurfaceSession::_update_draw_rectangle(Node3DEditorViewport *p_viewport, const Vector2 &p_position) {
	if (draw_phase != DrawPhase::RECTANGLE) {
		return;
	}
	Vector2 current_corner;
	if (!_project_draw_point(p_viewport, p_position, current_corner)) {
		return;
	}
	const CSGDrawRect rect = csg_draw_rectangle_bounds(draw_first_corner_uv, current_corner, 0.0);
	draw_rect_min = rect.min;
	draw_rect_max = rect.max;
	_update_context_panel();
	_queue_redraw(p_viewport);
}

static real_t _closest_parameter_on_line_to_ray(const Vector3 &p_line_origin, const Vector3 &p_line_direction, const Vector3 &p_ray_origin, const Vector3 &p_ray_direction);

void CSGSurfaceSession::_begin_draw_height(Node3DEditorViewport *p_viewport, const Vector2 &p_position) {
	if (!p_viewport || draw_phase != DrawPhase::RECTANGLE) {
		return;
	}
	const Vector2 rect_center = (draw_rect_min + draw_rect_max) * 0.5;
	draw_height_line_origin_world = draw_plane_origin_world + draw_plane_u_world * rect_center.x + draw_plane_v_world * rect_center.y;
	draw_height_line_direction_world = draw_plane_normal_world;
	draw_height_start_parameter = _closest_parameter_on_line_to_ray(
			draw_height_line_origin_world,
			draw_height_line_direction_world,
			p_viewport->get_ray_pos(p_position),
			p_viewport->get_ray(p_position).normalized());
	draw_height = 0.0;
	draw_phase = DrawPhase::HEIGHT;
	_update_context_panel();
	_queue_redraw(p_viewport);
}

void CSGSurfaceSession::_update_draw_height(Node3DEditorViewport *p_viewport, const Vector2 &p_position, bool p_ctrl_pressed) {
	if (!p_viewport || draw_phase != DrawPhase::HEIGHT) {
		return;
	}
	draw_ctrl_pressed = p_ctrl_pressed;
	const real_t current_parameter = _closest_parameter_on_line_to_ray(
			draw_height_line_origin_world,
			draw_height_line_direction_world,
			p_viewport->get_ray_pos(p_position),
			p_viewport->get_ray(p_position).normalized());
	draw_height = MAX(current_parameter - draw_height_start_parameter, (real_t)0.0);
	const real_t snap_step = _active_translate_snap_step();
	if (snap_step > 0.0) {
		// CSG-7: Snap the absolute height, never an accumulated mouse delta.
		draw_height = Math::snapped(draw_height, snap_step);
	}
	_update_context_panel();
	_queue_redraw(p_viewport);
}

void CSGSurfaceSession::_commit_draw() {
	if (draw_phase != DrawPhase::HEIGHT) {
		return;
	}
	// CSG-7: Zero height and a stale/degenerate footprint are pure cancellation, with no history.
	const CSGDrawRect rect = csg_draw_rectangle_bounds(draw_rect_min, draw_rect_max, 0.001);
	if (draw_height <= CMP_EPSILON || rect.degenerate) {
		_reset_draw_state();
		return;
	}

	CSGShape3D *root = _get_active_root();
	Node *edited_root = _get_edited_scene_root();
	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	if (!root || !edited_root || !undo_redo) {
		_reset_draw_state();
		return;
	}
	CSGPrimitive3D *parent_operand = nullptr;
	if (!draw_parent_operand_id.is_null()) {
		parent_operand = ObjectDB::get_instance<CSGPrimitive3D>(draw_parent_operand_id);
		if (!parent_operand) {
			_reset_draw_state();
			return;
		}
	}
	const CSGDrawBoxResult box = csg_draw_box_from_rect(rect, draw_height, draw_plane_origin_world, draw_plane_u_world, draw_plane_normal_world, draw_plane_v_world);
	CSGBox3D *new_box = csg_draw_commit_box(undo_redo, root, edited_root, parent_operand, box, _is_draw_cut_effective());
	if (!new_box) {
		_reset_draw_state();
		return;
	}

	// Retarget immediately so selection notifications cannot observe the old root with the new node selected.
	CSGShape3D *new_active_root = _find_csg_root(new_box);
	active_root_id = new_active_root ? new_active_root->get_instance_id() : ObjectID();
	if (new_active_root != root) {
		paint_selection.clear();
	}
	_clear_pick_state();
	_clear_selection();

	EditorNode *editor_node = EditorNode::get_singleton();
	EditorDocument *active_document = editor_node ? editor_node->get_editor_data().get_active_document() : nullptr;
	if (editor_node && (!active_document || active_document->get_root() == edited_root)) {
		EditorSelection *selection = active_document && active_document->get_selection() ? active_document->get_selection() : editor_node->get_editor_selection();
		if (selection) {
			selection->clear();
			selection->add_node(new_box);
		}
	}
	// CSG-7: A successful commit keeps the explicit Add/Cut choice (§23) so repeated cuts
	// don't require re-arming; cancel/exit/tool-switch paths still reset it (§3).
	const bool keep_cut_mode = draw_cut_mode;
	_reset_draw_state();
	draw_cut_mode = keep_cut_mode;
}

real_t CSGSurfaceSession::_get_draw_min_extent() const {
	real_t minimum_extent = 0.001;
	const real_t snap_step = _active_translate_snap_step();
	if (snap_step > 0.0) {
		minimum_extent = MAX(snap_step, minimum_extent);
	}
	return minimum_extent;
}

static real_t _closest_parameter_on_line_to_ray(const Vector3 &p_line_origin, const Vector3 &p_line_direction, const Vector3 &p_ray_origin, const Vector3 &p_ray_direction) {
	const Vector3 offset = p_line_origin - p_ray_origin;
	const real_t line_dot_ray = p_line_direction.dot(p_ray_direction);
	const real_t line_dot_offset = p_line_direction.dot(offset);
	const real_t ray_dot_offset = p_ray_direction.dot(offset);
	const real_t denominator = 1.0 - line_dot_ray * line_dot_ray;
	if (Math::is_zero_approx(denominator)) {
		return -line_dot_offset;
	}

	real_t line_parameter = (line_dot_ray * ray_dot_offset - line_dot_offset) / denominator;
	const real_t ray_parameter = (ray_dot_offset - line_dot_ray * line_dot_offset) / denominator;
	if (ray_parameter < 0.0) {
		line_parameter = -line_dot_offset;
	}
	return line_parameter;
}

bool CSGSurfaceSession::_begin_gesture(Node3DEditorViewport *p_viewport, const Ref<InputEventMouseButton> &p_event) {
	CSGShape3D *root = _get_active_root();
	if (!root || !p_viewport || p_event.is_null()) {
		return false;
	}
	if (!has_hover || hover_hit.result_generation != root->get_result_generation()) {
		if (!_pick(p_viewport, p_event->get_position())) {
			return false;
		}
	}

	CSGBox3D *box = ObjectDB::get_instance<CSGBox3D>(hover_hit.surface.source_shape);
	Vector3 outward;
	if (!box || !_get_box_surface_axis(hover_hit.surface.semantic_surface, drag_axis, drag_axis_sign, outward)) {
		return false;
	}

	active_box_id = box->get_instance_id();
	active_viewport_id = p_viewport->get_instance_id();
	selected_hit = hover_hit;
	has_selection = true;
	gesture_state = GestureState::PRESSED;
	press_position = p_event->get_position();
	// CSG-5: Gesture mode is fixed at press; extrusion wins over Alt symmetry.
	extrude_gesture = p_event->is_shift_pressed();
	symmetric_drag = p_event->is_alt_pressed();
	if (extrude_gesture) {
		symmetric_drag = false;
	}
	start_size = box->get_size();
	start_transform = box->get_transform();
	start_global_transform = box->get_global_transform();
	start_plane_coordinate = drag_axis_sign * start_size[drag_axis] * 0.5;
	target_plane_coordinate = start_plane_coordinate;
	drag_displacement = 0.0;
	ghost_result.size = start_size;
	ghost_result.transform = start_transform;
	has_ghost = false;

	drag_line_origin_world = start_global_transform.xform(outward * (start_size[drag_axis] * 0.5));
	const Vector3 world_axis = start_global_transform.basis.xform(outward);
	drag_axis_world_scale = world_axis.length();
	if (Math::is_zero_approx(drag_axis_world_scale)) {
		_cancel_gesture();
		return false;
	}
	drag_line_direction_world = world_axis / drag_axis_world_scale;
	drag_start_parameter = _closest_parameter_on_line_to_ray(
			drag_line_origin_world,
			drag_line_direction_world,
			p_viewport->get_ray_pos(press_position),
			p_viewport->get_ray(press_position).normalized());
	_update_context_panel();
	_queue_redraw(p_viewport);
	return true;
}

void CSGSurfaceSession::_update_drag(Node3DEditorViewport *p_viewport, const Vector2 &p_position) {
	if (!p_viewport || (gesture_state != GestureState::PRESSED && gesture_state != GestureState::DRAGGING)) {
		return;
	}
	if (gesture_state == GestureState::PRESSED && press_position.distance_to(p_position) < 4.0 * EDSCALE) {
		return;
	}
	gesture_state = GestureState::DRAGGING;

	const real_t current_parameter = _closest_parameter_on_line_to_ray(
			drag_line_origin_world,
			drag_line_direction_world,
			p_viewport->get_ray_pos(p_position),
			p_viewport->get_ray(p_position).normalized());
	const real_t outward_displacement = (current_parameter - drag_start_parameter) / drag_axis_world_scale;
	target_plane_coordinate = start_plane_coordinate + drag_axis_sign * outward_displacement;
	const real_t snap_step = _active_translate_snap_step();
	if (snap_space == SnapSpace::LOCAL && snap_step > 0.0) {
		target_plane_coordinate = Math::snapped(target_plane_coordinate, snap_step);
	}

	// CSG-4: Pointer and numeric input share the same post-clamp recompute.
	_apply_displacement();
	has_ghost = true;
	_update_context_panel();
	_queue_redraw(p_viewport);
}

void CSGSurfaceSession::_apply_displacement() {
	drag_displacement = (target_plane_coordinate - start_plane_coordinate) * drag_axis_sign;
	if (extrude_gesture) {
		// CSG-5: Inward depth stays non-committable while the ghost remains outward.
		extrude_ghost = csg_extrude_box_face(start_size, selected_hit.surface.semantic_surface, MAX(drag_displacement, (real_t)0.0));
		return;
	}

	// CSG-4: Re-derive the effective plane after the minimum-size clamp.
	ghost_result = csg_push_pull_apply(start_size, start_transform, selected_hit.surface.semantic_surface, drag_displacement, symmetric_drag);
	const real_t multiplier = symmetric_drag ? 2.0 : 1.0;
	drag_displacement = (ghost_result.size[drag_axis] - start_size[drag_axis]) / multiplier;
	target_plane_coordinate = start_plane_coordinate + drag_axis_sign * drag_displacement;
}

void CSGSurfaceSession::_cancel_gesture() {
	// CSG-4: Cancellation clears only session-local transient state.
	has_ghost = false;
	extrude_gesture = false; // CSG-5: Escape leaves no captured extrusion mode.
	drag_displacement = 0.0;
	target_plane_coordinate = start_plane_coordinate;
	gesture_state = has_hover ? GestureState::HOVER : GestureState::IDLE;
	_update_context_panel();
	_queue_redraw(_get_active_viewport());
}

void CSGSurfaceSession::_finish_without_commit() {
	// CSG-4: A threshold miss or no-op creates no undo history.
	has_ghost = false;
	extrude_gesture = false; // CSG-5: Ghost-only release has no persistent state.
	gesture_state = has_hover ? GestureState::HOVER : GestureState::IDLE;
	_update_context_panel();
	_queue_redraw(_get_active_viewport());
}

void CSGSurfaceSession::_commit_gesture() {
	CSGBox3D *box = _get_active_box();
	CSGShape3D *root = _get_active_root();
	if (!has_ghost || !box || !root) {
		_finish_without_commit();
		return;
	}
	if (extrude_gesture) {
		// CSG-5: Positive-only MVP; inward and zero drags create no node/history.
		if (drag_displacement <= CMP_EPSILON) {
			_finish_without_commit();
			return;
		}

		// Only nodes owned directly by this pane's document are writable here.
		Node *edited_root = _get_edited_scene_root();
		if (!_is_source_editable(box)) {
			_finish_without_commit();
			return;
		}

		EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
		if (!undo_redo) {
			_finish_without_commit();
			return;
		}

		// CSG-5: Add one union child beneath the source; never reparent the source.
		const CSGExtrusionResult extrusion = csg_extrude_box_face(start_size, selected_hit.surface.semantic_surface, drag_displacement);
		CSGBox3D *new_box = memnew(CSGBox3D);
		new_box->set_name("Extrusion");
		new_box->set_operation(CSGShape3D::OPERATION_UNION);
		new_box->set_size(extrusion.size);
		new_box->set_transform(extrusion.local_transform);
		const Transform3D source_to_root = root->get_global_transform().affine_inverse() * box->get_global_transform();
		csg_configure_extrusion_surface_settings(box, selected_hit.surface.semantic_surface, source_to_root, new_box);

		// CSG-5: Cap identity is transient - captured here, consumed below.
		const ObjectID cap_box_id = new_box->get_instance_id();
		const uint32_t cap_surface = selected_hit.surface.semantic_surface;
		gesture_state = GestureState::COMMIT;
		undo_redo->create_action(TTR("CSG Extrude Face"));
		undo_redo->add_do_method(box, "add_child", new_box, true);
		undo_redo->add_do_method(new_box, "set_owner", edited_root);
		undo_redo->add_undo_method(box, "remove_child", new_box);
		undo_redo->add_do_reference(new_box);
		// add_child queued a sync update. This last do-method snapshots the
		// post-add tree and supersedes that queued update (plan Sections 18/28).
		undo_redo->add_do_method(root, SNAME("_request_final_async_evaluation"));
		undo_redo->add_undo_method(root, SNAME("_request_final_async_evaluation"));
		undo_redo->commit_action();

		// CSG-5: Axes align, so the outward cap keeps the dragged surface index.
		active_box_id = cap_box_id;
		selected_hit = CSGSurfaceHit();
		selected_hit.surface = { cap_box_id, cap_surface, new_box->get_surface_schema_generation() };
		selected_hit.result_generation = root->get_result_generation();
		has_selection = true;
		has_ghost = false;
		has_hover = false;
		extrude_gesture = false;
		gesture_state = GestureState::IDLE;
		drag_displacement = 0.0;
		start_size = new_box->get_size();
		start_transform = new_box->get_transform();
		start_global_transform = new_box->get_global_transform();
		start_plane_coordinate = drag_axis_sign * start_size[drag_axis] * 0.5;
		target_plane_coordinate = start_plane_coordinate;
		_update_context_panel();
		_queue_redraw(_get_active_viewport());
		return;
	}
	if (ghost_result.size.is_equal_approx(start_size) && ghost_result.transform.is_equal_approx(start_transform)) {
		_finish_without_commit();
		return;
	}

	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	if (!undo_redo) {
		_finish_without_commit();
		return;
	}

	gesture_state = GestureState::COMMIT;
	undo_redo->create_action(TTR("CSG Push/Pull Face"));
	undo_redo->add_do_property(box, SNAME("size"), ghost_result.size);
	undo_redo->add_undo_property(box, SNAME("size"), start_size);
	undo_redo->add_do_property(box, SNAME("transform"), ghost_result.transform);
	undo_redo->add_undo_property(box, SNAME("transform"), start_transform);

	// A one-sided edit shifts the authored box center in its pre-edit local
	// frame. Compensate every locked Local projection in this same action;
	// Root and World frames are already anchored and need no stored change.
	const Vector3 center_shift_local = start_transform.basis.inverse().xform(ghost_result.transform.origin - start_transform.origin);
	const Transform3D operand_to_root = root->get_global_transform().affine_inverse() * start_global_transform;
	const Vector3 center_shift_root = operand_to_root.basis.xform(center_shift_local);
	for (uint32_t surface = 0; surface < box->get_surface_schema_size(); surface++) {
		if (!box->has_surface_setting(surface)) {
			continue;
		}
		const CSGSurfaceSetting setting = box->get_surface_setting(surface);
		const Vector2 compensated_offset = csg_texture_lock_compensate_offset(box, surface, setting, operand_to_root, center_shift_root);
		if (compensated_offset.is_equal_approx(setting.offset)) {
			continue;
		}
		const StringName offset_property = vformat("surface_settings/%d/offset", surface);
		undo_redo->add_do_property(box, offset_property, compensated_offset);
		undo_redo->add_undo_property(box, offset_property, setting.offset);
	}
	// Property setters queue the ordinary deferred update first. This final
	// request snapshots both properties and supersedes that queued sync update.
	undo_redo->add_do_method(root, SNAME("_request_final_async_evaluation"));
	undo_redo->add_undo_method(root, SNAME("_request_final_async_evaluation"));
	undo_redo->commit_action();

	has_ghost = false;
	has_hover = false;
	gesture_state = GestureState::IDLE;
	drag_displacement = 0.0;
	start_size = box->get_size();
	start_transform = box->get_transform();
	start_plane_coordinate = drag_axis_sign * start_size[drag_axis] * 0.5;
	target_plane_coordinate = start_plane_coordinate;
	_update_context_panel();
	_queue_redraw(_get_active_viewport());
}

void CSGSurfaceSession::_update_context_panel() {
	Control *surface_context = ObjectDB::get_instance<Control>(surface_context_id);
	Control *draw_context = ObjectDB::get_instance<Control>(draw_context_id);
	Control *paint_context = ObjectDB::get_instance<Control>(paint_context_id);
	if (surface_context) {
		surface_context->set_visible(tool_mode == ToolMode::SURFACE);
	}
	if (draw_context) {
		draw_context->set_visible(tool_mode == ToolMode::DRAW);
	}
	if (paint_context) {
		paint_context->set_visible(tool_mode == ToolMode::PAINT);
	}
	if (tool_mode == ToolMode::DRAW) {
		if (Button *button = ObjectDB::get_instance<Button>(draw_add_button_id)) {
			button->set_pressed_no_signal(!draw_cut_mode);
		}
		if (Button *button = ObjectDB::get_instance<Button>(draw_cut_button_id)) {
			button->set_pressed_no_signal(draw_cut_mode);
		}
		if (Label *hint = ObjectDB::get_instance<Label>(draw_hint_label_id)) {
			switch (draw_phase) {
				case DrawPhase::IDLE:
					hint->set_text(TTR("Drag a rectangle on a face or the ground plane"));
					break;
				case DrawPhase::RECTANGLE:
					hint->set_text(TTR("Drag to size the rectangle"));
					break;
				case DrawPhase::HEIGHT:
					hint->set_text(TTR("Move to set height, then click or type a value"));
					break;
			}
		}
		if (LineEdit *height_edit = ObjectDB::get_instance<LineEdit>(draw_height_edit_id)) {
			height_edit->set_editable(draw_phase == DrawPhase::HEIGHT);
			if (draw_phase != DrawPhase::HEIGHT) {
				height_edit->clear();
			} else if (!height_edit->has_focus()) {
				height_edit->set_text(String::num(draw_height, 4));
			}
		}
		return;
	}
	if (tool_mode == ToolMode::PAINT) {
		_prune_paint_selection();
		_update_paint_controls();
		return;
	}
	if (tool_mode == ToolMode::OPERAND) {
		return;
	}

	Label *distance_label = ObjectDB::get_instance<Label>(distance_label_id);
	LineEdit *coordinate_edit = ObjectDB::get_instance<LineEdit>(coordinate_edit_id);
	if (!has_selection) {
		if (distance_label) {
			distance_label->set_text(TTR("Select a box face"));
		}
		if (coordinate_edit && !coordinate_edit->has_focus()) {
			coordinate_edit->clear();
		}
		return;
	}

	if (distance_label) {
		// CSG-5: An extrusion reports positive-only depth, not push/pull distance.
		if (extrude_gesture) {
			distance_label->set_text(vformat(TTR("Extrude depth: %s m"), String::num(MAX(drag_displacement, (real_t)0.0), 4)));
		} else {
			distance_label->set_text(vformat(TTR("Distance: %s m"), String::num(drag_displacement, 4)));
		}
	}
	if (coordinate_edit && !coordinate_edit->has_focus()) {
		coordinate_edit->set_text(String::num(target_plane_coordinate, 4));
	}
}

void CSGSurfaceSession::_update_paint_controls() {
	EditorResourcePicker *material_picker = ObjectDB::get_instance<EditorResourcePicker>(paint_material_picker_id);
	OptionButton *uv_mode = ObjectDB::get_instance<OptionButton>(paint_uv_mode_id);
	OptionButton *uv_space = ObjectDB::get_instance<OptionButton>(paint_uv_space_id);
	SpinBox *meters_u = ObjectDB::get_instance<SpinBox>(paint_meters_u_id);
	SpinBox *meters_v = ObjectDB::get_instance<SpinBox>(paint_meters_v_id);
	SpinBox *offset_u = ObjectDB::get_instance<SpinBox>(paint_offset_u_id);
	SpinBox *offset_v = ObjectDB::get_instance<SpinBox>(paint_offset_v_id);
	SpinBox *rotation = ObjectDB::get_instance<SpinBox>(paint_rotation_id);
	CheckBox *texture_lock = ObjectDB::get_instance<CheckBox>(paint_texture_lock_id);
	Label *selection_label = ObjectDB::get_instance<Label>(paint_selection_label_id);
	Button *eyedropper = ObjectDB::get_instance<Button>(paint_eyedropper_button_id);

	updating_paint_controls = true;
	if (material_picker) {
		material_picker->set_edited_resource(paint_well.material);
	}
	if (uv_mode) {
		uv_mode->select(paint_well.uv_mode);
	}
	if (uv_space) {
		uv_space->select(paint_well.uv_space);
	}
	if (meters_u) {
		meters_u->set_value(paint_well.meters_per_tile.x);
	}
	if (meters_v) {
		meters_v->set_value(paint_well.meters_per_tile.y);
	}
	if (offset_u) {
		offset_u->set_value(paint_well.offset.x);
	}
	if (offset_v) {
		offset_v->set_value(paint_well.offset.y);
	}
	if (rotation) {
		rotation->set_value(Math::rad_to_deg(paint_well.rotation));
	}
	if (texture_lock) {
		texture_lock->set_pressed_no_signal(paint_well.texture_lock);
	}
	if (eyedropper) {
		eyedropper->set_pressed_no_signal(paint_eyedropper_active);
	}
	updating_paint_controls = false;

	const bool planar = paint_well.uv_mode == CSGPrimitive3D::SURFACE_UV_MODE_PLANAR;
	if (uv_space) {
		uv_space->set_disabled(!planar);
	}
	for (SpinBox *spin : { meters_u, meters_v, offset_u, offset_v, rotation }) {
		if (spin) {
			spin->set_editable(planar);
		}
	}
	if (texture_lock) {
		texture_lock->set_disabled(!planar);
	}
	if (selection_label) {
		selection_label->set_text(vformat(TTRN("%d surface selected", "%d surfaces selected", paint_selection.size()), paint_selection.size()));
	}
}

void CSGSurfaceSession::_numeric_coordinate_submitted(const String &p_text) {
	if (!has_selection || !p_text.is_valid_float()) {
		return;
	}
	CSGBox3D *box = _get_active_box();
	if (!box) {
		_clear_selection();
		return;
	}

	Vector3 outward;
	if (gesture_state != GestureState::PRESSED && gesture_state != GestureState::DRAGGING) {
		if (!_get_box_surface_axis(selected_hit.surface.semantic_surface, drag_axis, drag_axis_sign, outward)) {
			return;
		}
		start_size = box->get_size();
		start_transform = box->get_transform();
		start_global_transform = box->get_global_transform();
		start_plane_coordinate = drag_axis_sign * start_size[drag_axis] * 0.5;
		extrude_gesture = false; // CSG-5: Fresh numeric entry remains push/pull.
		symmetric_drag = false;
	}

	target_plane_coordinate = p_text.to_float();
	// CSG-4: Numeric entry uses the pointer path's post-clamp recompute.
	_apply_displacement();
	gesture_state = GestureState::DRAGGING;
	has_ghost = true;
	_update_context_panel();
	_queue_redraw(_get_active_viewport());
	_commit_gesture();
}

void CSGSurfaceSession::_numeric_draw_height_submitted(const String &p_text) {
	if (draw_phase != DrawPhase::HEIGHT || !p_text.is_valid_float()) {
		return;
	}
	// CSG-7: Draw boxes only grow along the positive plane normal.
	draw_height = MAX(p_text.to_float(), (real_t)0.0);
	_update_context_panel();
	_queue_redraw(_get_active_viewport());
	_commit_draw();
}

static void _get_box_face_corners(const Vector3 &p_size, uint32_t p_surface, Vector3 r_corners[4]) {
	const Vector3 half_size = p_size * 0.5;
	switch (p_surface) {
		case CSGBox3D::SURFACE_POSITIVE_X:
			r_corners[0] = Vector3(half_size.x, -half_size.y, -half_size.z);
			r_corners[1] = Vector3(half_size.x, half_size.y, -half_size.z);
			r_corners[2] = Vector3(half_size.x, half_size.y, half_size.z);
			r_corners[3] = Vector3(half_size.x, -half_size.y, half_size.z);
			break;
		case CSGBox3D::SURFACE_NEGATIVE_X:
			r_corners[0] = Vector3(-half_size.x, -half_size.y, half_size.z);
			r_corners[1] = Vector3(-half_size.x, half_size.y, half_size.z);
			r_corners[2] = Vector3(-half_size.x, half_size.y, -half_size.z);
			r_corners[3] = Vector3(-half_size.x, -half_size.y, -half_size.z);
			break;
		case CSGBox3D::SURFACE_POSITIVE_Y:
			r_corners[0] = Vector3(-half_size.x, half_size.y, half_size.z);
			r_corners[1] = Vector3(half_size.x, half_size.y, half_size.z);
			r_corners[2] = Vector3(half_size.x, half_size.y, -half_size.z);
			r_corners[3] = Vector3(-half_size.x, half_size.y, -half_size.z);
			break;
		case CSGBox3D::SURFACE_NEGATIVE_Y:
			r_corners[0] = Vector3(-half_size.x, -half_size.y, -half_size.z);
			r_corners[1] = Vector3(half_size.x, -half_size.y, -half_size.z);
			r_corners[2] = Vector3(half_size.x, -half_size.y, half_size.z);
			r_corners[3] = Vector3(-half_size.x, -half_size.y, half_size.z);
			break;
		case CSGBox3D::SURFACE_POSITIVE_Z:
			r_corners[0] = Vector3(-half_size.x, -half_size.y, half_size.z);
			r_corners[1] = Vector3(half_size.x, -half_size.y, half_size.z);
			r_corners[2] = Vector3(half_size.x, half_size.y, half_size.z);
			r_corners[3] = Vector3(-half_size.x, half_size.y, half_size.z);
			break;
		case CSGBox3D::SURFACE_NEGATIVE_Z:
			r_corners[0] = Vector3(-half_size.x, half_size.y, -half_size.z);
			r_corners[1] = Vector3(half_size.x, half_size.y, -half_size.z);
			r_corners[2] = Vector3(half_size.x, -half_size.y, -half_size.z);
			r_corners[3] = Vector3(-half_size.x, -half_size.y, -half_size.z);
			break;
		default:
			for (int i = 0; i < 4; i++) {
				r_corners[i] = Vector3();
			}
			break;
	}
}

static Transform3D _local_to_world_transform(Node3D *p_node, const Transform3D &p_local_transform) {
	Node3D *parent_3d = Object::cast_to<Node3D>(p_node->get_parent());
	return parent_3d ? parent_3d->get_global_transform() * p_local_transform : p_local_transform;
}

void CSGSurfaceSession::_draw_ghost(Node3DEditorViewport *p_viewport) const {
	if (!has_selection || !p_viewport) {
		return;
	}
	CSGBox3D *box = _get_active_box();
	if (!box) {
		return;
	}
	Camera3D *camera = p_viewport->get_previewing_camera();
	if (!camera) {
		camera = p_viewport->get_camera_3d();
	}
	Control *surface_control = p_viewport->get_surface();
	if (!camera || !surface_control) {
		return;
	}

	// CSG-5: The source stays rendered in place; only the prospective child prism moves.
	const bool draw_extrusion = has_ghost && extrude_gesture;
	const Vector3 target_size = draw_extrusion ? extrude_ghost.size : (has_ghost ? ghost_result.size : box->get_size());
	const Transform3D target_global = draw_extrusion ? box->get_global_transform() * extrude_ghost.local_transform : (has_ghost ? _local_to_world_transform(box, ghost_result.transform) : box->get_global_transform());
	Vector3 face_corners[4];
	_get_box_face_corners(target_size, selected_hit.surface.semantic_surface, face_corners);
	Vector<Point2> face_polygon;
	face_polygon.resize(4);
	Vector3 face_center;
	for (int i = 0; i < 4; i++) {
		const Vector3 world_corner = target_global.xform(face_corners[i]);
		if (camera->is_position_behind(world_corner)) {
			return;
		}
		face_center += world_corner;
		face_polygon.write[i] = camera->unproject_position(world_corner);
	}
	face_center /= 4.0;
	surface_control->draw_colored_polygon(face_polygon, draw_extrusion ? Color(0.25, 0.95, 0.5, 0.24) : Color(1.0, 0.65, 0.15, has_ghost ? 0.22 : 0.12));
	face_polygon.push_back(face_polygon[0]);
	surface_control->draw_polyline(face_polygon, draw_extrusion ? Color(0.35, 1.0, 0.6) : Color(1.0, 0.72, 0.2), 2.0 * EDSCALE, true);

	int axis = 0;
	real_t sign = 1.0;
	Vector3 outward;
	if (_get_box_surface_axis(selected_hit.surface.semantic_surface, axis, sign, outward)) {
		const Vector3 world_normal = target_global.basis.xform(outward).normalized();
		const real_t handle_length = MAX(target_global.basis.xform(outward * target_size[axis] * 0.25).length(), (real_t)0.25);
		const Vector3 handle_end = face_center + world_normal * handle_length;
		if (!camera->is_position_behind(handle_end)) {
			surface_control->draw_line(camera->unproject_position(face_center), camera->unproject_position(handle_end), draw_extrusion ? Color(0.45, 1.0, 0.65) : Color(1.0, 0.78, 0.25), 2.0 * EDSCALE, true);
		}
	}

	if (has_ghost) {
		Vector3 corners[8];
		for (int corner_i = 0; corner_i < 8; corner_i++) {
			const Vector3 local_corner(
					(corner_i & 1) ? target_size.x * 0.5 : -target_size.x * 0.5,
					(corner_i & 2) ? target_size.y * 0.5 : -target_size.y * 0.5,
					(corner_i & 4) ? target_size.z * 0.5 : -target_size.z * 0.5);
			corners[corner_i] = target_global.xform(local_corner);
		}
		static constexpr int edge_indices[12][2] = {
			{ 0, 1 },
			{ 2, 3 },
			{ 4, 5 },
			{ 6, 7 },
			{ 0, 2 },
			{ 1, 3 },
			{ 4, 6 },
			{ 5, 7 },
			{ 0, 4 },
			{ 1, 5 },
			{ 2, 6 },
			{ 3, 7 },
		};
		for (const int *edge : edge_indices) {
			if (camera->is_position_behind(corners[edge[0]]) || camera->is_position_behind(corners[edge[1]])) {
				continue;
			}
			surface_control->draw_line(camera->unproject_position(corners[edge[0]]), camera->unproject_position(corners[edge[1]]), draw_extrusion ? Color(0.35, 1.0, 0.6, 0.95) : Color(0.4, 0.9, 1.0, 0.95), 1.5 * EDSCALE, true);
		}

		const Point2 label_position = camera->unproject_position(face_center) + Point2(10, -10) * EDSCALE;
		surface_control->draw_string(
				surface_control->get_theme_default_font(),
				label_position,
				vformat(TTR("Plane %s m"), String::num(target_plane_coordinate, 4)),
				HORIZONTAL_ALIGNMENT_LEFT,
				-1,
				surface_control->get_theme_default_font_size(),
				Color(0.9, 0.98, 1.0));
	}
}

void CSGSurfaceSession::_draw_draw_ghost(Node3DEditorViewport *p_viewport) const {
	if ((draw_phase != DrawPhase::RECTANGLE && draw_phase != DrawPhase::HEIGHT) || !p_viewport) {
		return;
	}
	Camera3D *camera = p_viewport->get_previewing_camera();
	if (!camera) {
		camera = p_viewport->get_camera_3d();
	}
	Control *surface_control = p_viewport->get_surface();
	if (!camera || !surface_control) {
		return;
	}

	const bool effective_cut = _is_draw_cut_effective();
	const Color color = effective_cut ? Color(1.0, 0.25, 0.2) : Color(0.25, 1.0, 0.45);
	if (draw_phase == DrawPhase::RECTANGLE) {
		// CSG-7: Rectangle feedback is view-only; no CSG node exists until the Height commit.
		const Vector2 plane_corners[4] = {
			Vector2(draw_rect_min.x, draw_rect_min.y),
			Vector2(draw_rect_max.x, draw_rect_min.y),
			Vector2(draw_rect_max.x, draw_rect_max.y),
			Vector2(draw_rect_min.x, draw_rect_max.y),
		};
		Vector<Point2> outline;
		outline.resize(5);
		for (int i = 0; i < 4; i++) {
			const Vector3 world_corner = draw_plane_origin_world + draw_plane_u_world * plane_corners[i].x + draw_plane_v_world * plane_corners[i].y;
			if (camera->is_position_behind(world_corner)) {
				return;
			}
			outline.write[i] = camera->unproject_position(world_corner);
		}
		outline.write[4] = outline[0];
		surface_control->draw_polyline(outline, color, 2.0 * EDSCALE, true);
		return;
	}

	const CSGDrawRect rect = { draw_rect_min, draw_rect_max, false };
	const CSGDrawBoxResult box = csg_draw_box_from_rect(rect, draw_height, draw_plane_origin_world, draw_plane_u_world, draw_plane_normal_world, draw_plane_v_world);
	Vector3 corners[8];
	for (int corner_i = 0; corner_i < 8; corner_i++) {
		const Vector3 local_corner(
				(corner_i & 1) ? box.size.x * 0.5 : -box.size.x * 0.5,
				(corner_i & 2) ? box.size.y * 0.5 : -box.size.y * 0.5,
				(corner_i & 4) ? box.size.z * 0.5 : -box.size.z * 0.5);
		corners[corner_i] = box.world_transform.xform(local_corner);
	}
	static constexpr int edge_indices[12][2] = {
		{ 0, 1 },
		{ 2, 3 },
		{ 4, 5 },
		{ 6, 7 },
		{ 0, 2 },
		{ 1, 3 },
		{ 4, 6 },
		{ 5, 7 },
		{ 0, 4 },
		{ 1, 5 },
		{ 2, 6 },
		{ 3, 7 },
	};
	for (const int *edge : edge_indices) {
		if (camera->is_position_behind(corners[edge[0]]) || camera->is_position_behind(corners[edge[1]])) {
			continue;
		}
		surface_control->draw_line(camera->unproject_position(corners[edge[0]]), camera->unproject_position(corners[edge[1]]), color, 1.5 * EDSCALE, true);
	}

	const int top_indices[4] = { 2, 3, 7, 6 };
	Vector<Point2> top_cap;
	top_cap.resize(4);
	Vector3 top_center;
	for (int i = 0; i < 4; i++) {
		const Vector3 world_corner = corners[top_indices[i]];
		if (camera->is_position_behind(world_corner)) {
			return;
		}
		top_center += world_corner;
		top_cap.write[i] = camera->unproject_position(world_corner);
	}
	top_center /= 4.0;
	Color fill_color = color;
	fill_color.a = 0.22;
	surface_control->draw_colored_polygon(top_cap, fill_color);
	top_cap.push_back(top_cap[0]);
	surface_control->draw_polyline(top_cap, color, 2.0 * EDSCALE, true);
	surface_control->draw_string(
			surface_control->get_theme_default_font(),
			camera->unproject_position(top_center) + Point2(10, -10) * EDSCALE,
			vformat(TTR("Height %s m"), String::num(draw_height, 4)),
			HORIZONTAL_ALIGNMENT_LEFT,
			-1,
			surface_control->get_theme_default_font_size(),
			Color(0.95, 1.0, 0.96));
}

void CSGSurfaceSession::_draw_hover(Node3DEditorViewport *p_viewport) const {
	if (!has_hover || !p_viewport) {
		return;
	}
	CSGShape3D *root = _get_active_root();
	if (!root) {
		return;
	}

	Camera3D *camera = p_viewport->get_previewing_camera();
	if (!camera) {
		camera = p_viewport->get_camera_3d();
	}
	Control *surface_control = p_viewport->get_surface();
	if (!camera || !surface_control) {
		return;
	}

	Vector<Vector3> world_corners;
	if (CSGBox3D *box = ObjectDB::get_instance<CSGBox3D>(hover_hit.surface.source_shape)) {
		Vector3 local_corners[4];
		_get_box_face_corners(box->get_size(), hover_hit.surface.semantic_surface, local_corners);
		world_corners.resize(4);
		for (int i = 0; i < 4; i++) {
			world_corners.write[i] = box->get_global_transform().xform(local_corners[i]);
		}
	} else {
		const uint32_t vertex_begin = hover_hit.triangle * 3;
		if (vertex_begin + 2 >= (uint32_t)pick_faces.size()) {
			return;
		}
		world_corners.resize(3);
		for (int i = 0; i < 3; i++) {
			world_corners.write[i] = root->get_global_transform().xform(pick_faces[vertex_begin + i]);
		}
	}

	Vector<Point2> polygon;
	polygon.resize(world_corners.size());
	for (int i = 0; i < world_corners.size(); i++) {
		if (camera->is_position_behind(world_corners[i])) {
			return;
		}
		polygon.write[i] = camera->unproject_position(world_corners[i]);
	}
	surface_control->draw_colored_polygon(polygon, Color(0.2, 0.7, 1.0, 0.24));
	polygon.push_back(polygon[0]);
	surface_control->draw_polyline(polygon, Color(0.35, 0.85, 1.0), 2.0 * EDSCALE, true);
}

void CSGSurfaceSession::_draw_paint_selection(Node3DEditorViewport *p_viewport) const {
	if (paint_selection.is_empty() || !p_viewport) {
		return;
	}
	CSGShape3D *root = _get_active_root();
	if (!root || pick_mesh_generation != root->get_result_generation()) {
		return;
	}

	Camera3D *camera = p_viewport->get_previewing_camera();
	if (!camera) {
		camera = p_viewport->get_camera_3d();
	}
	Control *surface_control = p_viewport->get_surface();
	if (!camera || !surface_control) {
		return;
	}

	const uint64_t result_generation = root->get_result_generation();
	const Transform3D root_to_world = root->get_global_transform();
	for (uint32_t triangle_i = 0; triangle_i < (uint32_t)pick_faces.size() / 3; triangle_i++) {
		CSGSurfaceKey surface;
		uint32_t face_id = 0;
		if (!root->resolve_result_triangle(triangle_i, result_generation, surface, face_id) || !_paint_selection_has(surface)) {
			continue;
		}

		Vector<Point2> polygon;
		polygon.resize(3);
		bool behind_camera = false;
		for (int corner_i = 0; corner_i < 3; corner_i++) {
			const Vector3 world_corner = root_to_world.xform(pick_faces[triangle_i * 3 + corner_i]);
			if (camera->is_position_behind(world_corner)) {
				behind_camera = true;
				break;
			}
			polygon.write[corner_i] = camera->unproject_position(world_corner);
		}
		if (behind_camera) {
			continue;
		}
		surface_control->draw_colored_polygon(polygon, Color(1.0, 0.58, 0.18, 0.14));
		polygon.push_back(polygon[0]);
		surface_control->draw_polyline(polygon, Color(1.0, 0.68, 0.28, 0.75), EDSCALE, true);
	}
}

void CSGSurfaceSession::enter(const EditorEditDomainContext &p_context) {
	entered = true;
	active_viewport_id = p_context.active_viewport ? p_context.active_viewport->get_instance_id() : ObjectID();
	_capture_edited_scene_root(p_context);
	_resolve_active_root(p_context);
	if (p_context.active_viewport) {
		p_context.active_viewport->update_surface();
	}
}

void CSGSurfaceSession::exit() {
	_cancel_gesture();
	_reset_draw_state(false);
	entered = false;
	active_root_id = ObjectID();
	active_viewport_id = ObjectID();
	edited_scene_root_id = ObjectID();
	paint_selection.clear();
	paint_eyedropper_active = false;
	_clear_pick_state();
	_clear_selection();
}

void CSGSurfaceSession::retarget(const EditorEditDomainContext &p_context) {
	_cancel_gesture();
	_reset_draw_state(false);
	_clear_pick_state();
	paint_selection.clear();
	_clear_selection();
	active_viewport_id = p_context.active_viewport ? p_context.active_viewport->get_instance_id() : ObjectID();
	_capture_edited_scene_root(p_context);
	_resolve_active_root(p_context);
	if (p_context.active_viewport) {
		p_context.active_viewport->update_surface();
	}
}

EditorEditDomainInput CSGSurfaceSession::handle_input(const EditorEditDomainContext &p_context, Camera3D *p_camera, const Ref<InputEvent> &p_event) {
	if (!entered || !p_context.active_viewport) {
		return EditorEditDomainInput::PASS_TO_VIEWPORT;
	}
	_capture_edited_scene_root(p_context);
	CSGShape3D *selected_root = _find_csg_root(_get_single_selected_csg_shape(&p_context));
	const ObjectID selected_root_id = selected_root ? selected_root->get_instance_id() : ObjectID();
	if (selected_root_id != active_root_id) {
		retarget(p_context);
	}
	if (!_get_active_root()) {
		if (tool_mode == ToolMode::DRAW) {
			// CSG-7: Draw never becomes an activation path after its scoped root disappears.
			_reset_draw_state();
		}
		return EditorEditDomainInput::PASS_TO_VIEWPORT;
	}
	active_viewport_id = p_context.active_viewport->get_instance_id();
	if (tool_mode == ToolMode::OPERAND) {
		return EditorEditDomainInput::PASS_TO_VIEWPORT;
	}

	Ref<InputEventKey> key_event = p_event;
	if (tool_mode == ToolMode::SURFACE && key_event.is_valid() && key_event->is_pressed() && !key_event->is_echo() && (key_event->get_keycode() == Key::ENTER || key_event->get_keycode() == Key::KP_ENTER)) {
		if (gesture_state == GestureState::DRAGGING) {
			_commit_gesture();
			return EditorEditDomainInput::CONSUMED;
		}
		if (gesture_state == GestureState::PRESSED) {
			_finish_without_commit();
			return EditorEditDomainInput::CONSUMED;
		}
	}

	Ref<View3DController> controller = p_context.active_viewport->get_controller();
	if (controller.is_valid() && (controller->is_navigating() || controller->cursor.region_select)) {
		return EditorEditDomainInput::PASS_TO_VIEWPORT;
	}
	if (tool_mode == ToolMode::DRAW && key_event.is_valid()) {
		const bool previous_ctrl = draw_ctrl_pressed;
		draw_ctrl_pressed = key_event->is_ctrl_pressed();
		if (previous_ctrl != draw_ctrl_pressed) {
			_queue_redraw(p_context.active_viewport);
		}
		if (key_event->get_keycode() == Key::CTRL) {
			return EditorEditDomainInput::BLOCK_NATIVE_EDIT;
		}
		if (key_event->is_pressed() && !key_event->is_echo() && draw_phase == DrawPhase::HEIGHT && (key_event->get_keycode() == Key::ENTER || key_event->get_keycode() == Key::KP_ENTER)) {
			_commit_draw();
			return EditorEditDomainInput::CONSUMED;
		}
	}

	Ref<InputEventMouseButton> mouse_button = p_event;
	if (mouse_button.is_valid()) {
		const MouseButton button = mouse_button->get_button_index();
		if (button == MouseButton::MIDDLE || button == MouseButton::RIGHT || button == MouseButton::WHEEL_UP || button == MouseButton::WHEEL_DOWN || button == MouseButton::WHEEL_LEFT || button == MouseButton::WHEEL_RIGHT) {
			return EditorEditDomainInput::PASS_TO_VIEWPORT;
		}
		if (button == MouseButton::LEFT) {
			if (tool_mode == ToolMode::DRAW) {
				draw_ctrl_pressed = mouse_button->is_ctrl_pressed();
				if (mouse_button->is_pressed()) {
					if (draw_phase == DrawPhase::HEIGHT) {
						_commit_draw();
						return EditorEditDomainInput::CONSUMED;
					}
					if (draw_phase == DrawPhase::IDLE) {
						return _resolve_draw_plane(p_context.active_viewport, mouse_button->get_position()) ? EditorEditDomainInput::CONSUMED : EditorEditDomainInput::PASS_TO_VIEWPORT;
					}
					return EditorEditDomainInput::CONSUMED;
				}
				if (draw_phase == DrawPhase::RECTANGLE) {
					_update_draw_rectangle(p_context.active_viewport, mouse_button->get_position());
					const CSGDrawRect rect = csg_draw_rectangle_bounds(draw_rect_min, draw_rect_max, _get_draw_min_extent());
					if (rect.degenerate) {
						_reset_draw_state();
					} else {
						draw_rect_min = rect.min;
						draw_rect_max = rect.max;
						_begin_draw_height(p_context.active_viewport, mouse_button->get_position());
					}
					return EditorEditDomainInput::CONSUMED;
				}
				return EditorEditDomainInput::PASS_TO_VIEWPORT;
			}
			if (tool_mode == ToolMode::PAINT) {
				if (mouse_button->is_pressed()) {
					if (!_pick(p_context.active_viewport, mouse_button->get_position())) {
						return EditorEditDomainInput::PASS_TO_VIEWPORT;
					}
					if (mouse_button->is_alt_pressed() || paint_eyedropper_active) {
						_lift_paint_setting(hover_hit.surface);
						paint_eyedropper_active = false;
						_update_paint_controls();
						_queue_redraw(p_context.active_viewport);
						return EditorEditDomainInput::CONSUMED;
					}

					_select_paint_surface(hover_hit.surface, mouse_button->is_shift_pressed());
					Vector<CSGSurfaceKey> clicked_surface;
					clicked_surface.push_back(hover_hit.surface);
					_apply_paint_to_surfaces(clicked_surface, UndoRedo::MERGE_DISABLE, TTR("CSG Paint Surface"));
					_queue_redraw(p_context.active_viewport);
					return EditorEditDomainInput::CONSUMED;
				}
				return has_hover ? EditorEditDomainInput::CONSUMED : EditorEditDomainInput::PASS_TO_VIEWPORT;
			}
			if (mouse_button->is_pressed()) {
				return _begin_gesture(p_context.active_viewport, mouse_button) ? EditorEditDomainInput::CONSUMED : EditorEditDomainInput::PASS_TO_VIEWPORT;
			}
			if (gesture_state == GestureState::PRESSED) {
				_finish_without_commit();
				return EditorEditDomainInput::CONSUMED;
			}
			if (gesture_state == GestureState::DRAGGING) {
				_commit_gesture();
				return EditorEditDomainInput::CONSUMED;
			}
		}
	}

	Ref<InputEventMouseMotion> mouse_motion = p_event;
	if (tool_mode == ToolMode::DRAW) {
		if (mouse_motion.is_valid() && mouse_motion->get_button_mask().has_flag(MouseButtonMask::LEFT) && draw_phase == DrawPhase::RECTANGLE) {
			draw_ctrl_pressed = mouse_motion->is_ctrl_pressed();
			_update_draw_rectangle(p_context.active_viewport, mouse_motion->get_position());
			return EditorEditDomainInput::CONSUMED;
		}
		if (mouse_motion.is_valid() && draw_phase == DrawPhase::HEIGHT && !mouse_motion->get_button_mask().has_flag(MouseButtonMask::LEFT) && !mouse_motion->get_button_mask().has_flag(MouseButtonMask::MIDDLE) && !mouse_motion->get_button_mask().has_flag(MouseButtonMask::RIGHT)) {
			_update_draw_height(p_context.active_viewport, mouse_motion->get_position(), mouse_motion->is_ctrl_pressed());
			return EditorEditDomainInput::BLOCK_NATIVE_EDIT;
		}
		return EditorEditDomainInput::PASS_TO_VIEWPORT;
	}
	if (tool_mode == ToolMode::PAINT) {
		if (mouse_motion.is_valid() && mouse_motion->get_button_mask().has_flag(MouseButtonMask::LEFT)) {
			return has_hover ? EditorEditDomainInput::CONSUMED : EditorEditDomainInput::PASS_TO_VIEWPORT;
		}
		if (mouse_motion.is_valid() && !mouse_motion->get_button_mask().has_flag(MouseButtonMask::MIDDLE) && !mouse_motion->get_button_mask().has_flag(MouseButtonMask::RIGHT)) {
			const bool had_hover = has_hover;
			const CSGSurfaceHit previous_hit = hover_hit;
			const bool picked = _pick(p_context.active_viewport, mouse_motion->get_position());
			if (had_hover != has_hover || (has_hover && (!(previous_hit.surface == hover_hit.surface) || previous_hit.triangle != hover_hit.triangle || previous_hit.result_generation != hover_hit.result_generation))) {
				_queue_redraw(p_context.active_viewport);
			}
			return picked ? EditorEditDomainInput::BLOCK_NATIVE_EDIT : EditorEditDomainInput::PASS_TO_VIEWPORT;
		}
		return EditorEditDomainInput::PASS_TO_VIEWPORT;
	}
	if (mouse_motion.is_valid() && mouse_motion->get_button_mask().has_flag(MouseButtonMask::LEFT) && (gesture_state == GestureState::PRESSED || gesture_state == GestureState::DRAGGING)) {
		_update_drag(p_context.active_viewport, mouse_motion->get_position());
		return EditorEditDomainInput::CONSUMED;
	}
	if (mouse_motion.is_valid() && !mouse_motion->get_button_mask().has_flag(MouseButtonMask::LEFT) && !mouse_motion->get_button_mask().has_flag(MouseButtonMask::MIDDLE) && !mouse_motion->get_button_mask().has_flag(MouseButtonMask::RIGHT)) {
		const bool had_hover = has_hover;
		const CSGSurfaceHit previous_hit = hover_hit;
		const bool picked = _pick(p_context.active_viewport, mouse_motion->get_position());
		if (had_hover != has_hover || (has_hover && (previous_hit.triangle != hover_hit.triangle || previous_hit.result_generation != hover_hit.result_generation))) {
			_queue_redraw(p_context.active_viewport);
		}
		gesture_state = has_hover ? GestureState::HOVER : GestureState::IDLE;
		return picked ? EditorEditDomainInput::BLOCK_NATIVE_EDIT : EditorEditDomainInput::PASS_TO_VIEWPORT;
	}
	return EditorEditDomainInput::PASS_TO_VIEWPORT;
}

bool CSGSurfaceSession::handle_escape() {
	if (tool_mode == ToolMode::DRAW) {
		if (draw_phase == DrawPhase::HEIGHT) {
			// CSG-7: Cancel only height selection and return to the authored footprint.
			draw_phase = DrawPhase::RECTANGLE;
			draw_height = 0.0;
			draw_height_line_origin_world = Vector3();
			draw_height_line_direction_world = Vector3();
			draw_height_start_parameter = 0.0;
			_update_context_panel();
			_queue_redraw(_get_active_viewport());
			return true;
		}
		if (draw_phase == DrawPhase::RECTANGLE) {
			_reset_draw_state();
			return true;
		}
	}
	if (tool_mode == ToolMode::PAINT && paint_eyedropper_active) {
		paint_eyedropper_active = false;
		_update_paint_controls();
		return true;
	}
	if (gesture_state != GestureState::PRESSED && gesture_state != GestureState::DRAGGING) {
		return false;
	}
	_cancel_gesture();
	return true;
}

bool CSGSurfaceSession::handle_tool_toggle() {
	_set_tool_mode(tool_mode == ToolMode::OPERAND ? ToolMode::SURFACE : ToolMode::OPERAND);
	return true;
}

void CSGSurfaceSession::draw_overlay(Node3DEditorViewport *p_viewport) {
	if (!entered || tool_mode == ToolMode::OPERAND) {
		return;
	}
	if (tool_mode == ToolMode::PAINT) {
		_draw_paint_selection(p_viewport);
		_draw_hover(p_viewport);
	} else if (tool_mode == ToolMode::DRAW) {
		_draw_draw_ghost(p_viewport);
	} else {
		_draw_hover(p_viewport);
		_draw_ghost(p_viewport);
	}
}

Control *CSGSurfaceSession::build_tool_rail() {
	PanelContainer *rail_panel = memnew(PanelContainer);
	rail_panel->set_name("CSGSurfaceToolRailPanel");
	rail_panel->set_theme_type_variation(SNAME("ViewportPanel"));
	VBoxContainer *rail = memnew(VBoxContainer);
	rail->set_name("CSGSurfaceToolRail");
	rail_panel->add_child(rail);
	Button *surface_button = memnew(Button);
	surface_button->set_text(TTR("Surface"));
	surface_button->set_toggle_mode(true);
	surface_button->connect(SceneStringName(pressed), callable_mp(this, &CSGSurfaceSession::_surface_tool_pressed));
	surface_tool_button_id = surface_button->get_instance_id();
	rail->add_child(surface_button);

	Button *draw_button = memnew(Button);
	draw_button->set_text(TTR("Draw"));
	draw_button->set_toggle_mode(true);
	draw_button->connect(SceneStringName(pressed), callable_mp(this, &CSGSurfaceSession::_draw_tool_pressed));
	draw_tool_button_id = draw_button->get_instance_id();
	rail->add_child(draw_button);

	Button *paint_button = memnew(Button);
	paint_button->set_text(TTR("Paint"));
	paint_button->set_toggle_mode(true);
	paint_button->connect(SceneStringName(pressed), callable_mp(this, &CSGSurfaceSession::_paint_tool_pressed));
	paint_tool_button_id = paint_button->get_instance_id();
	rail->add_child(paint_button);

	Button *operand_button = memnew(Button);
	operand_button->set_text(TTR("Operand"));
	operand_button->set_toggle_mode(true);
	operand_button->connect(SceneStringName(pressed), callable_mp(this, &CSGSurfaceSession::_operand_tool_pressed));
	operand_tool_button_id = operand_button->get_instance_id();
	rail->add_child(operand_button);
	_update_tool_buttons();
	return rail_panel;
}

static HBoxContainer *_add_csg_paint_row(VBoxContainer *p_parent, const String &p_label) {
	HBoxContainer *row = memnew(HBoxContainer);
	p_parent->add_child(row);
	Label *label = memnew(Label);
	label->set_text(p_label);
	label->set_custom_minimum_size(Size2(72, 0) * EDSCALE);
	row->add_child(label);
	return row;
}

static SpinBox *_create_csg_paint_spin(double p_min, double p_max, double p_step, const String &p_suffix = String()) {
	SpinBox *spin = memnew(SpinBox);
	spin->set_min(p_min);
	spin->set_max(p_max);
	spin->set_step(p_step);
	spin->set_allow_lesser(true);
	spin->set_allow_greater(true);
	spin->set_suffix(p_suffix);
	spin->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	return spin;
}

static void _add_csg_paint_button(VBoxContainer *p_parent, const String &p_text, const Callable &p_callable) {
	Button *button = memnew(Button);
	button->set_text(p_text);
	button->connect(SceneStringName(pressed), p_callable);
	p_parent->add_child(button);
}

Control *CSGSurfaceSession::build_contextual_panel() {
	PanelContainer *panel = memnew(PanelContainer);
	panel->set_name("CSGSurfaceContextPanel");
	panel->set_theme_type_variation(SNAME("ViewportPanel"));
	VBoxContainer *contents = memnew(VBoxContainer);
	panel->add_child(contents);

	VBoxContainer *surface_contents = memnew(VBoxContainer);
	surface_context_id = surface_contents->get_instance_id();
	contents->add_child(surface_contents);
	Label *distance_label = memnew(Label);
	distance_label->set_text(TTR("Select a box face"));
	distance_label_id = distance_label->get_instance_id();
	surface_contents->add_child(distance_label);
	HBoxContainer *coordinate_row = memnew(HBoxContainer);
	surface_contents->add_child(coordinate_row);
	Label *coordinate_label = memnew(Label);
	coordinate_label->set_text(TTR("Plane"));
	coordinate_row->add_child(coordinate_label);
	LineEdit *coordinate_edit = memnew(LineEdit);
	coordinate_edit->set_placeholder(TTR("Coordinate"));
	coordinate_edit->set_custom_minimum_size(Size2(96, 0) * EDSCALE);
	coordinate_edit->connect(SceneStringName(text_submitted), callable_mp(this, &CSGSurfaceSession::_numeric_coordinate_submitted));
	coordinate_edit_id = coordinate_edit->get_instance_id();
	coordinate_row->add_child(coordinate_edit);

	// CSG-7: Draw-specific state is view-local and lives only in this contextual panel.
	VBoxContainer *draw_contents = memnew(VBoxContainer);
	draw_context_id = draw_contents->get_instance_id();
	contents->add_child(draw_contents);
	Label *draw_title = memnew(Label);
	draw_title->set_text(TTR("Draw Box"));
	draw_contents->add_child(draw_title);

	HBoxContainer *operation_row = memnew(HBoxContainer);
	draw_contents->add_child(operation_row);
	Ref<ButtonGroup> operation_group;
	operation_group.instantiate();
	operation_group->set_allow_unpress(false);
	Button *add_button = memnew(Button);
	add_button->set_name("CSGDrawAdd");
	add_button->set_text(TTR("Add"));
	add_button->set_toggle_mode(true);
	add_button->set_button_group(operation_group);
	add_button->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	add_button->connect(SceneStringName(pressed), callable_mp(this, &CSGSurfaceSession::_draw_add_pressed));
	draw_add_button_id = add_button->get_instance_id();
	operation_row->add_child(add_button);
	Button *cut_button = memnew(Button);
	cut_button->set_name("CSGDrawCut");
	cut_button->set_text(TTR("Cut"));
	cut_button->set_toggle_mode(true);
	cut_button->set_button_group(operation_group);
	cut_button->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	cut_button->connect(SceneStringName(pressed), callable_mp(this, &CSGSurfaceSession::_draw_cut_pressed));
	draw_cut_button_id = cut_button->get_instance_id();
	operation_row->add_child(cut_button);

	HBoxContainer *height_row = memnew(HBoxContainer);
	draw_contents->add_child(height_row);
	Label *height_label = memnew(Label);
	height_label->set_text(TTR("Height"));
	height_row->add_child(height_label);
	LineEdit *height_edit = memnew(LineEdit);
	height_edit->set_name("CSGDrawHeight");
	height_edit->set_placeholder(TTR("Height"));
	height_edit->set_custom_minimum_size(Size2(96, 0) * EDSCALE);
	height_edit->connect(SceneStringName(text_submitted), callable_mp(this, &CSGSurfaceSession::_numeric_draw_height_submitted));
	draw_height_edit_id = height_edit->get_instance_id();
	height_row->add_child(height_edit);
	Label *draw_hint = memnew(Label);
	draw_hint->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	draw_hint_label_id = draw_hint->get_instance_id();
	draw_contents->add_child(draw_hint);

	VBoxContainer *paint_contents = memnew(VBoxContainer);
	paint_context_id = paint_contents->get_instance_id();
	contents->add_child(paint_contents);
	Label *paint_title = memnew(Label);
	paint_title->set_text(TTR("Surface Material and UV"));
	paint_contents->add_child(paint_title);

	Label *selection_label = memnew(Label);
	paint_selection_label_id = selection_label->get_instance_id();
	paint_contents->add_child(selection_label);

	EditorResourcePicker *material_picker = memnew(EditorResourcePicker);
	material_picker->set_base_type("Material");
	material_picker->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	material_picker->connect(SNAME("resource_changed"), callable_mp(this, &CSGSurfaceSession::_paint_material_changed));
	paint_material_picker_id = material_picker->get_instance_id();
	_add_csg_paint_row(paint_contents, TTR("Material"))->add_child(material_picker);

	OptionButton *uv_mode = memnew(OptionButton);
	uv_mode->add_item(TTR("Legacy"), CSGPrimitive3D::SURFACE_UV_MODE_LEGACY);
	uv_mode->add_item(TTR("Planar"), CSGPrimitive3D::SURFACE_UV_MODE_PLANAR);
	uv_mode->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	uv_mode->connect(SceneStringName(item_selected), callable_mp(this, &CSGSurfaceSession::_paint_uv_mode_selected));
	paint_uv_mode_id = uv_mode->get_instance_id();
	_add_csg_paint_row(paint_contents, TTR("Projection"))->add_child(uv_mode);

	OptionButton *uv_space = memnew(OptionButton);
	uv_space->add_item(TTR("Local"), CSGPrimitive3D::SURFACE_UV_SPACE_LOCAL);
	uv_space->add_item(TTR("Root"), CSGPrimitive3D::SURFACE_UV_SPACE_ROOT);
	uv_space->add_item(TTR("World"), CSGPrimitive3D::SURFACE_UV_SPACE_WORLD);
	uv_space->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	uv_space->connect(SceneStringName(item_selected), callable_mp(this, &CSGSurfaceSession::_paint_uv_space_selected));
	paint_uv_space_id = uv_space->get_instance_id();
	_add_csg_paint_row(paint_contents, TTR("Space"))->add_child(uv_space);

	HBoxContainer *meters_row = _add_csg_paint_row(paint_contents, TTR("Tile Size"));
	SpinBox *meters_u = _create_csg_paint_spin(0.001, 10000.0, 0.01, TTR(" m U"));
	meters_u->connect(SceneStringName(value_changed), callable_mp(this, &CSGSurfaceSession::_paint_numeric_changed));
	paint_meters_u_id = meters_u->get_instance_id();
	meters_row->add_child(meters_u);
	SpinBox *meters_v = _create_csg_paint_spin(0.001, 10000.0, 0.01, TTR(" m V"));
	meters_v->connect(SceneStringName(value_changed), callable_mp(this, &CSGSurfaceSession::_paint_numeric_changed));
	paint_meters_v_id = meters_v->get_instance_id();
	meters_row->add_child(meters_v);

	HBoxContainer *offset_row = _add_csg_paint_row(paint_contents, TTR("Offset"));
	SpinBox *offset_u = _create_csg_paint_spin(-10000.0, 10000.0, 0.01, TTR(" U"));
	offset_u->connect(SceneStringName(value_changed), callable_mp(this, &CSGSurfaceSession::_paint_numeric_changed));
	paint_offset_u_id = offset_u->get_instance_id();
	offset_row->add_child(offset_u);
	SpinBox *offset_v = _create_csg_paint_spin(-10000.0, 10000.0, 0.01, TTR(" V"));
	offset_v->connect(SceneStringName(value_changed), callable_mp(this, &CSGSurfaceSession::_paint_numeric_changed));
	paint_offset_v_id = offset_v->get_instance_id();
	offset_row->add_child(offset_v);

	SpinBox *rotation = _create_csg_paint_spin(-360.0, 360.0, 0.1, TTR(" deg"));
	rotation->connect(SceneStringName(value_changed), callable_mp(this, &CSGSurfaceSession::_paint_numeric_changed));
	paint_rotation_id = rotation->get_instance_id();
	_add_csg_paint_row(paint_contents, TTR("Rotation"))->add_child(rotation);

	CheckBox *texture_lock = memnew(CheckBox);
	texture_lock->set_text(TTR("Texture Lock"));
	texture_lock->connect(SceneStringName(toggled), callable_mp(this, &CSGSurfaceSession::_paint_texture_lock_toggled));
	paint_texture_lock_id = texture_lock->get_instance_id();
	paint_contents->add_child(texture_lock);

	HBoxContainer *primary_actions = memnew(HBoxContainer);
	paint_contents->add_child(primary_actions);
	Button *assign_button = memnew(Button);
	assign_button->set_text(TTR("Assign"));
	assign_button->connect(SceneStringName(pressed), callable_mp(this, &CSGSurfaceSession::_paint_assign_pressed));
	primary_actions->add_child(assign_button);
	Button *eyedropper_button = memnew(Button);
	eyedropper_button->set_text(TTR("Eyedropper"));
	eyedropper_button->set_toggle_mode(true);
	eyedropper_button->connect(SceneStringName(toggled), callable_mp(this, &CSGSurfaceSession::_paint_eyedropper_toggled));
	paint_eyedropper_button_id = eyedropper_button->get_instance_id();
	primary_actions->add_child(eyedropper_button);

	_add_csg_paint_button(paint_contents, TTR("Align to Face"), callable_mp(this, &CSGSurfaceSession::_paint_align_face_pressed));
	_add_csg_paint_button(paint_contents, TTR("Align to Root Grid"), callable_mp(this, &CSGSurfaceSession::_paint_align_root_pressed));
	_add_csg_paint_button(paint_contents, TTR("Fit"), callable_mp(this, &CSGSurfaceSession::_paint_fit_pressed));
	_add_csg_paint_button(paint_contents, TTR("Reset"), callable_mp(this, &CSGSurfaceSession::_paint_reset_pressed));
	_add_csg_paint_button(paint_contents, TTR("Apply to Selected"), callable_mp(this, &CSGSurfaceSession::_paint_apply_selected_pressed));
	_update_context_panel();
	return panel;
}

StringName CSGEditDomainProvider::get_domain_id() const {
	return SNAME("csg_surface");
}

bool CSGEditDomainProvider::is_available(const EditorEditDomainContext &p_context) const {
	return _get_single_selected_csg_shape(&p_context) != nullptr;
}

bool CSGEditDomainProvider::can_activate_from_double_click(const EditorEditDomainContext &p_context, ObjectID p_hit) const {
	return ObjectDB::get_instance<CSGShape3D>(p_hit) != nullptr;
}

EditorEditDomainSession *CSGEditDomainProvider::create_session(const EditorEditDomainContext &p_context) const {
	return memnew(CSGSurfaceSession);
}
