/**************************************************************************/
/*  csg_shape.cpp                                                         */
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

#include "csg_shape.h"

#include "core/config/engine.h"
#include "core/math/geometry_2d.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "scene/main/scene_tree.h"
#include "scene/resources/3d/navigation_mesh_source_geometry_data_3d.h"
#include "scene/resources/navigation_mesh.h"
#include "scene/resources/surface_tool.h"
#include "servers/rendering/rendering_server.h"

#ifdef DEV_ENABLED
#include "csg_debug_counters.h"

#include "core/io/json.h"
#endif // DEV_ENABLED

#ifndef NAVIGATION_3D_DISABLED
#include "servers/navigation_3d/navigation_server_3d.h"
#endif // NAVIGATION_3D_DISABLED

#include <manifold/manifold.h>

#include <cfloat> // FLT_EPSILON

struct CSGManifoldSurfaceRecord {
	CSGOriginToken origin_token = 0;
	CSGSurfaceKey surface;
	Ref<Material> source_material;
};

struct CSGManifoldResultTriangle {
	CSGOriginToken origin_token = 0;
	uint32_t face_id = 0;
};

struct CSGShape3D::ManifoldCache {
	CSGBrush *local_brush = nullptr;
	// Clean Manifold values keep their exact CSG-node handles. In particular,
	// subtree_manifold must only be evaluated through a copy.
	manifold::Manifold local_manifold;
	manifold::Manifold transformed_manifold;
	manifold::Manifold subtree_manifold;
	// One contiguous origin-token range is retained for the whole schema
	// generation, including across geometry-only leaf rebuilds.
	CSGOriginToken origin_base = 0;
	uint32_t origin_count = 0;
	uint32_t origin_schema_generation = 0;
	Vector<CSGManifoldSurfaceRecord> surface_records;

	// Root materialization snapshot. One compact pair is retained per output
	// triangle; keys are stored once per origin token.
	HashMap<CSGOriginToken, CSGSurfaceKey> result_surface_keys;
	Vector<CSGManifoldResultTriangle> result_triangles;

	bool local_manifold_dirty = true;
	bool transformed_manifold_dirty = true;
	bool subtree_manifold_dirty = true;
	bool materialization_dirty = true;
	bool subtree_empty = true;
	bool subtree_empty_valid = false;

	~ManifoldCache() {
		if (local_brush) {
			memdelete(local_brush);
		}
	}
};

#ifndef NAVIGATION_3D_DISABLED
Callable CSGShape3D::_navmesh_source_geometry_parsing_callback;
RID CSGShape3D::_navmesh_source_geometry_parser;

void CSGShape3D::navmesh_parse_init() {
	ERR_FAIL_NULL(NavigationServer3D::get_singleton());
	if (!_navmesh_source_geometry_parser.is_valid()) {
		_navmesh_source_geometry_parsing_callback = callable_mp_static(&CSGShape3D::navmesh_parse_source_geometry);
		_navmesh_source_geometry_parser = NavigationServer3D::get_singleton()->source_geometry_parser_create();
		NavigationServer3D::get_singleton()->source_geometry_parser_set_callback(_navmesh_source_geometry_parser, _navmesh_source_geometry_parsing_callback);
	}
}

void CSGShape3D::navmesh_parse_source_geometry(const Ref<NavigationMesh> &p_navigation_mesh, Ref<NavigationMeshSourceGeometryData3D> p_source_geometry_data, Node *p_node) {
	CSGShape3D *csgshape3d = Object::cast_to<CSGShape3D>(p_node);

	if (csgshape3d == nullptr) {
		return;
	}

	NavigationMesh::ParsedGeometryType parsed_geometry_type = p_navigation_mesh->get_parsed_geometry_type();

#ifndef PHYSICS_3D_DISABLED
	bool nav_collision = (parsed_geometry_type == NavigationMesh::PARSED_GEOMETRY_STATIC_COLLIDERS && csgshape3d->is_using_collision() && (csgshape3d->get_collision_layer() & p_navigation_mesh->get_collision_mask()));
#else
	bool nav_collision = false;
#endif // PHYSICS_3D_DISABLED
	if (parsed_geometry_type == NavigationMesh::PARSED_GEOMETRY_MESH_INSTANCES || nav_collision || parsed_geometry_type == NavigationMesh::PARSED_GEOMETRY_BOTH) {
		Array meshes = csgshape3d->get_meshes();
		if (!meshes.is_empty()) {
			Ref<Mesh> mesh = meshes[1];
			if (mesh.is_valid()) {
				p_source_geometry_data->add_mesh(mesh, csgshape3d->get_global_transform());
			}
		}
	}
}
#endif // NAVIGATION_3D_DISABLED

#ifndef PHYSICS_3D_DISABLED
void CSGShape3D::set_use_collision(bool p_enable) {
	if (use_collision == p_enable) {
		return;
	}

	use_collision = p_enable;

	if (!is_inside_tree() || !is_root_shape()) {
		return;
	}

	if (use_collision) {
		root_collision_shape.instantiate();
		root_collision_body = PhysicsServer3D::get_singleton()->body_create();
		PhysicsServer3D::get_singleton()->body_set_mode(root_collision_body, PS3DE::BODY_MODE_STATIC);
		PhysicsServer3D::get_singleton()->body_set_state(root_collision_body, PS3DE::BODY_STATE_TRANSFORM, get_global_transform());
		PhysicsServer3D::get_singleton()->body_add_shape(root_collision_body, root_collision_shape->get_rid());
		PhysicsServer3D::get_singleton()->body_set_space(root_collision_body, get_world_3d()->get_space());
		PhysicsServer3D::get_singleton()->body_attach_object_instance_id(root_collision_body, get_instance_id());
		set_collision_layer(collision_layer);
		set_collision_mask(collision_mask);
		set_collision_priority(collision_priority);
		_make_output_dirty(); // Force collision output to update.
	} else {
		PhysicsServer3D::get_singleton()->free_rid(root_collision_body);
		root_collision_body = RID();
		root_collision_shape.unref();
	}
	notify_property_list_changed();
	update_gizmos();
}

bool CSGShape3D::is_using_collision() const {
	return use_collision;
}

void CSGShape3D::set_collision_layer(uint32_t p_layer) {
	collision_layer = p_layer;
	if (root_collision_body.is_valid()) {
		PhysicsServer3D::get_singleton()->body_set_collision_layer(root_collision_body, p_layer);
	}
}

uint32_t CSGShape3D::get_collision_layer() const {
	return collision_layer;
}

void CSGShape3D::set_collision_mask(uint32_t p_mask) {
	collision_mask = p_mask;
	if (root_collision_body.is_valid()) {
		PhysicsServer3D::get_singleton()->body_set_collision_mask(root_collision_body, p_mask);
	}
}

uint32_t CSGShape3D::get_collision_mask() const {
	return collision_mask;
}

void CSGShape3D::set_collision_layer_value(int p_layer_number, bool p_value) {
	ERR_FAIL_COND_MSG(p_layer_number < 1, "Collision layer number must be between 1 and 32 inclusive.");
	ERR_FAIL_COND_MSG(p_layer_number > 32, "Collision layer number must be between 1 and 32 inclusive.");
	uint32_t layer = get_collision_layer();
	if (p_value) {
		layer |= 1 << (p_layer_number - 1);
	} else {
		layer &= ~(1 << (p_layer_number - 1));
	}
	set_collision_layer(layer);
}

bool CSGShape3D::get_collision_layer_value(int p_layer_number) const {
	ERR_FAIL_COND_V_MSG(p_layer_number < 1, false, "Collision layer number must be between 1 and 32 inclusive.");
	ERR_FAIL_COND_V_MSG(p_layer_number > 32, false, "Collision layer number must be between 1 and 32 inclusive.");
	return get_collision_layer() & (1 << (p_layer_number - 1));
}

void CSGShape3D::set_collision_mask_value(int p_layer_number, bool p_value) {
	ERR_FAIL_COND_MSG(p_layer_number < 1, "Collision layer number must be between 1 and 32 inclusive.");
	ERR_FAIL_COND_MSG(p_layer_number > 32, "Collision layer number must be between 1 and 32 inclusive.");
	uint32_t mask = get_collision_mask();
	if (p_value) {
		mask |= 1 << (p_layer_number - 1);
	} else {
		mask &= ~(1 << (p_layer_number - 1));
	}
	set_collision_mask(mask);
}

bool CSGShape3D::get_collision_mask_value(int p_layer_number) const {
	ERR_FAIL_COND_V_MSG(p_layer_number < 1, false, "Collision layer number must be between 1 and 32 inclusive.");
	ERR_FAIL_COND_V_MSG(p_layer_number > 32, false, "Collision layer number must be between 1 and 32 inclusive.");
	return get_collision_mask() & (1 << (p_layer_number - 1));
}

RID CSGShape3D::_get_root_collision_instance() const {
	if (root_collision_body.is_valid()) {
		return root_collision_body;
	} else if (parent_shape) {
		return parent_shape->_get_root_collision_instance();
	}

	return RID();
}

void CSGShape3D::set_collision_priority(real_t p_priority) {
	collision_priority = p_priority;
	if (root_collision_body.is_valid()) {
		PhysicsServer3D::get_singleton()->body_set_collision_priority(root_collision_body, p_priority);
	}
}

real_t CSGShape3D::get_collision_priority() const {
	return collision_priority;
}

void CSGShape3D::set_autosmooth(bool p_smooth) {
	autosmooth = p_smooth;
	_make_output_dirty();
	notify_property_list_changed();
}

bool CSGShape3D::is_autosmooth() const {
	return autosmooth;
}

void CSGShape3D::set_smoothing_angle(const float p_angle) {
	smoothing_angle = p_angle;
	_make_output_dirty();
}

float CSGShape3D::get_smoothing_angle() const {
	return smoothing_angle;
}

#endif // PHYSICS_3D_DISABLED

bool CSGShape3D::is_root_shape() const {
	return !parent_shape;
}

uint32_t CSGShape3D::get_surface_schema_size() const {
	return _get_surface_schema_size();
}

uint32_t CSGShape3D::get_surface_schema_generation() const {
	return surface_schema_generation;
}

bool CSGShape3D::get_surface_key(uint32_t p_semantic_surface, CSGSurfaceKey &r_surface) const {
	r_surface = CSGSurfaceKey();
	if (p_semantic_surface >= _get_surface_schema_size()) {
		return false;
	}

	r_surface.source_shape = get_instance_id();
	r_surface.semantic_surface = p_semantic_surface;
	r_surface.schema_generation = surface_schema_generation;
	return true;
}

bool CSGShape3D::get_surface_origin_token(uint32_t p_semantic_surface, CSGOriginToken &r_token) {
	r_token = 0;
	if (p_semantic_surface >= _get_surface_schema_size()) {
		return false;
	}

	_ensure_local_manifold();
	if (manifold_cache->origin_schema_generation != surface_schema_generation || p_semantic_surface >= manifold_cache->origin_count) {
		return false;
	}
	r_token = manifold_cache->origin_base + p_semantic_surface;
	return true;
}

bool CSGShape3D::is_surface_key_valid(const CSGSurfaceKey &p_surface) {
	CSGShape3D *source = ObjectDB::get_instance<CSGShape3D>(p_surface.source_shape);
	return source && p_surface.schema_generation == source->surface_schema_generation && p_surface.semantic_surface < source->_get_surface_schema_size();
}

uint64_t CSGShape3D::get_result_generation() const {
	return result_generation;
}

uint32_t CSGShape3D::get_result_triangle_count() const {
	return manifold_cache->result_triangles.size();
}

bool CSGShape3D::resolve_result_triangle(uint32_t p_triangle, uint64_t p_result_generation, CSGSurfaceKey &r_surface, uint32_t &r_face_id, CSGOriginToken *r_origin_token) const {
	r_surface = CSGSurfaceKey();
	r_face_id = 0;
	if (r_origin_token) {
		*r_origin_token = 0;
	}
	if (!is_root_shape() || p_result_generation != result_generation || p_triangle >= (uint32_t)manifold_cache->result_triangles.size()) {
		return false;
	}

	const CSGManifoldResultTriangle &triangle = manifold_cache->result_triangles[p_triangle];
	const CSGSurfaceKey *surface = manifold_cache->result_surface_keys.getptr(triangle.origin_token);
	if (!surface || !is_surface_key_valid(*surface)) {
		return false;
	}

	r_surface = *surface;
	r_face_id = triangle.face_id;
	if (r_origin_token) {
		*r_origin_token = triangle.origin_token;
	}
	return true;
}

#ifndef DISABLE_DEPRECATED
void CSGShape3D::set_snap(float p_snap) {
	if (snap == p_snap) {
		return;
	}

	snap = p_snap;
	_make_output_dirty();
}

float CSGShape3D::get_snap() const {
	return snap;
}
#endif // DISABLE_DEPRECATED

void CSGShape3D::_queue_root_update(bool p_force) {
	CSGShape3D *root = this;
	while (root->parent_shape) {
		root = root->parent_shape;
	}

	if (p_force || !root->dirty) {
		callable_mp(root, &CSGShape3D::update_shape).call_deferred();
	}
	root->dirty = true;
}

void CSGShape3D::_invalidate_subtree_and_ancestors() {
	CSGShape3D *shape = this;
	while (shape) {
		shape->manifold_cache->subtree_manifold_dirty = true;
		shape->manifold_cache->transformed_manifold_dirty = true;
		shape->manifold_cache->materialization_dirty = true;
		shape->manifold_cache->subtree_empty_valid = false;
		shape = shape->parent_shape;
	}
	_queue_root_update();
}

void CSGShape3D::_invalidate_materialization_and_ancestors() {
	CSGShape3D *shape = this;
	while (shape) {
		shape->manifold_cache->materialization_dirty = true;
		shape = shape->parent_shape;
	}
	_queue_root_update();
}

void CSGShape3D::_make_dirty() {
	manifold_cache->local_manifold_dirty = true;
	_invalidate_subtree_and_ancestors();
}

void CSGShape3D::_make_material_dirty() {
	_invalidate_materialization_and_ancestors();
}

void CSGShape3D::_make_output_dirty() {
	_queue_root_update();
}

void CSGShape3D::_make_transform_dirty() {
	manifold_cache->transformed_manifold_dirty = true;
	if (parent_shape) {
		parent_shape->_invalidate_subtree_and_ancestors();
	}
}

void CSGShape3D::_make_operation_dirty() {
	if (parent_shape) {
		parent_shape->_invalidate_subtree_and_ancestors();
	}
}

void CSGShape3D::_synchronize_surface_schema() {
	const uint32_t schema_size = _get_surface_schema_size();
	if (cached_surface_schema_size == UINT32_MAX) {
		cached_surface_schema_size = schema_size;
		return;
	}
	if (cached_surface_schema_size == schema_size) {
		return;
	}

	cached_surface_schema_size = schema_size;
	surface_schema_generation++;
	if (surface_schema_generation == 0) {
		surface_schema_generation = 1;
	}
}

Ref<Material> CSGShape3D::_resolve_manifold_material(const Ref<Material> &p_source_material) const {
	if (const CSGMesh3D *mesh = Object::cast_to<CSGMesh3D>(this)) {
		Ref<Material> csg_material_override = mesh->get_material();
		return csg_material_override.is_valid() ? csg_material_override : p_source_material;
	}
	if (const CSGSphere3D *sphere = Object::cast_to<CSGSphere3D>(this)) {
		return sphere->get_material();
	}
	if (const CSGBox3D *box = Object::cast_to<CSGBox3D>(this)) {
		return box->get_material();
	}
	if (const CSGCylinder3D *cylinder = Object::cast_to<CSGCylinder3D>(this)) {
		return cylinder->get_material();
	}
	if (const CSGTorus3D *torus = Object::cast_to<CSGTorus3D>(this)) {
		return torus->get_material();
	}
	if (const CSGPolygon3D *polygon = Object::cast_to<CSGPolygon3D>(this)) {
		return polygon->get_material();
	}
	return p_source_material;
}

enum ManifoldProperty {
	MANIFOLD_PROPERTY_POSITION_X = 0,
	MANIFOLD_PROPERTY_POSITION_Y,
	MANIFOLD_PROPERTY_POSITION_Z,
	MANIFOLD_PROPERTY_INVERT,
	MANIFOLD_PROPERTY_SMOOTH_GROUP,
	MANIFOLD_PROPERTY_UV_X_0,
	MANIFOLD_PROPERTY_UV_Y_0,
	MANIFOLD_PROPERTY_MAX
};

static void _unpack_manifold(
		const manifold::Manifold &p_manifold,
		const HashMap<CSGOriginToken, Ref<Material>> &p_mesh_materials,
		CSGBrush *r_mesh_merge,
		Vector<CSGManifoldResultTriangle> *r_result_triangles) {
	manifold::MeshGL64 mesh = p_manifold.GetMeshGL64();

	constexpr int32_t order[3] = { 0, 2, 1 };

	for (size_t run_i = 0; run_i < mesh.runIndex.size() - 1; run_i++) {
		CSGOriginToken original_id = UINT32_MAX;
		if (run_i < mesh.runOriginalID.size()) {
			original_id = mesh.runOriginalID[run_i];
		}

		Ref<Material> material;
		if (p_mesh_materials.has(original_id)) {
			material = p_mesh_materials[original_id];
		}
		// Find or reserve a material ID in the brush.
		int32_t material_id = r_mesh_merge->materials.find(material);
		if (material_id == -1) {
			material_id = r_mesh_merge->materials.size();
			r_mesh_merge->materials.push_back(material);
		}

		size_t begin = mesh.runIndex[run_i];
		size_t end = mesh.runIndex[run_i + 1];
		for (size_t vert_i = begin; vert_i < end; vert_i += 3) {
			CSGBrush::Face face;
			face.material = material_id;
			int32_t first_property_index = mesh.triVerts[vert_i + order[0]];
			face.smooth = mesh.vertProperties[first_property_index * mesh.numProp + MANIFOLD_PROPERTY_SMOOTH_GROUP] > 0.5f;
			face.invert = mesh.vertProperties[first_property_index * mesh.numProp + MANIFOLD_PROPERTY_INVERT] > 0.5f;

			for (int32_t tri_order_i = 0; tri_order_i < 3; tri_order_i++) {
				int32_t property_i = mesh.triVerts[vert_i + order[tri_order_i]];
				ERR_FAIL_COND_MSG(property_i * mesh.numProp >= mesh.vertProperties.size(), "Invalid index into vertex properties");
				face.vertices[tri_order_i] = Vector3(
						mesh.vertProperties[property_i * mesh.numProp + MANIFOLD_PROPERTY_POSITION_X],
						mesh.vertProperties[property_i * mesh.numProp + MANIFOLD_PROPERTY_POSITION_Y],
						mesh.vertProperties[property_i * mesh.numProp + MANIFOLD_PROPERTY_POSITION_Z]);
				face.uvs[tri_order_i] = Vector2(
						mesh.vertProperties[property_i * mesh.numProp + MANIFOLD_PROPERTY_UV_X_0],
						mesh.vertProperties[property_i * mesh.numProp + MANIFOLD_PROPERTY_UV_Y_0]);
			}
			r_mesh_merge->faces.push_back(face);
			if (r_result_triangles) {
				const size_t triangle_i = vert_i / 3;
				CSGManifoldResultTriangle triangle;
				triangle.origin_token = original_id;
				if (triangle_i < mesh.faceID.size()) {
					triangle.face_id = (uint32_t)mesh.faceID[triangle_i];
				}
				r_result_triangles->push_back(triangle);
			}
		}
	}

	r_mesh_merge->_regen_face_aabbs();
}

#ifdef DEV_ENABLED
static String _export_meshgl_as_json(const manifold::MeshGL64 &p_mesh) {
	Dictionary mesh_dict;
	mesh_dict["numProp"] = p_mesh.numProp;

	Array vert_properties;
	for (const double &val : p_mesh.vertProperties) {
		vert_properties.append(val);
	}
	mesh_dict["vertProperties"] = vert_properties;

	Array tri_verts;
	for (const uint64_t &val : p_mesh.triVerts) {
		tri_verts.append(val);
	}
	mesh_dict["triVerts"] = tri_verts;

	Array merge_from_vert;
	for (const uint64_t &val : p_mesh.mergeFromVert) {
		merge_from_vert.append(val);
	}
	mesh_dict["mergeFromVert"] = merge_from_vert;

	Array merge_to_vert;
	for (const uint64_t &val : p_mesh.mergeToVert) {
		merge_to_vert.append(val);
	}
	mesh_dict["mergeToVert"] = merge_to_vert;

	Array run_index;
	for (const uint64_t &val : p_mesh.runIndex) {
		run_index.append(val);
	}
	mesh_dict["runIndex"] = run_index;

	Array run_original_id;
	for (const uint32_t &val : p_mesh.runOriginalID) {
		run_original_id.append(val);
	}
	mesh_dict["runOriginalID"] = run_original_id;

	Array run_transform;
	for (const double &val : p_mesh.runTransform) {
		run_transform.append(val);
	}
	mesh_dict["runTransform"] = run_transform;

	Array face_id;
	for (const uint64_t &val : p_mesh.faceID) {
		face_id.append(val);
	}
	mesh_dict["faceID"] = face_id;

	Array halfedge_tangent;
	for (const double &val : p_mesh.halfedgeTangent) {
		halfedge_tangent.append(val);
	}
	mesh_dict["halfedgeTangent"] = halfedge_tangent;

	mesh_dict["tolerance"] = p_mesh.tolerance;

	String json_string = JSON::stringify(mesh_dict);
	return json_string;
}
#endif // DEV_ENABLED

static void _pack_manifold(
		const CSGBrush *const p_mesh_merge,
		manifold::Manifold &r_manifold,
		CSGOriginToken p_origin_base,
		uint32_t p_schema_size,
		ObjectID p_source_shape,
		uint32_t p_schema_generation,
		Vector<CSGManifoldSurfaceRecord> &r_surface_records) {
	ERR_FAIL_NULL_MSG(p_mesh_merge, "p_mesh_merge is null");

	Vector<Vector<CSGBrush::Face>> faces_by_surface;
	faces_by_surface.resize(p_schema_size);
	for (int face_i = 0; face_i < p_mesh_merge->faces.size(); face_i++) {
		const CSGBrush::Face &face = p_mesh_merge->faces[face_i];
		ERR_FAIL_COND_MSG(face.semantic_surface >= p_schema_size, "CSG brush face has no valid semantic surface.");
		faces_by_surface.write[face.semantic_surface].push_back(face);
	}

	r_surface_records.resize(p_schema_size);
	for (uint32_t surface_i = 0; surface_i < p_schema_size; surface_i++) {
		CSGManifoldSurfaceRecord &record = r_surface_records.write[surface_i];
		record.origin_token = p_origin_base + surface_i;
		record.surface.source_shape = p_source_shape;
		record.surface.semantic_surface = surface_i;
		record.surface.schema_generation = p_schema_generation;
		if (!faces_by_surface[surface_i].is_empty()) {
			const int32_t material_id = faces_by_surface[surface_i][0].material;
			if (material_id >= 0 && material_id < p_mesh_merge->materials.size()) {
				record.source_material = p_mesh_merge->materials[material_id];
			}
		}
	}

	manifold::MeshGL64 mesh;
	mesh.numProp = MANIFOLD_PROPERTY_MAX;
	mesh.runOriginalID.reserve(p_schema_size);
	mesh.runIndex.reserve(p_schema_size + 1);
	mesh.vertProperties.reserve(p_mesh_merge->faces.size() * 3 * MANIFOLD_PROPERTY_MAX);
	mesh.faceID.reserve(p_mesh_merge->faces.size());

	// One run per non-empty semantic surface. Relative triangle order within a
	// surface is unchanged from the brush.
	for (uint32_t surface_i = 0; surface_i < p_schema_size; surface_i++) {
		const Vector<CSGBrush::Face> &faces = faces_by_surface[surface_i];
		if (faces.is_empty()) {
			continue;
		}
		mesh.runIndex.push_back(mesh.triVerts.size());
		mesh.runOriginalID.push_back(p_origin_base + surface_i);
		for (const CSGBrush::Face &face : faces) {
			mesh.faceID.push_back(face.face_id);
			for (int32_t tri_order_i = 0; tri_order_i < 3; tri_order_i++) {
				constexpr int32_t order[3] = { 0, 2, 1 };
				int i = order[tri_order_i];

				mesh.triVerts.push_back(mesh.vertProperties.size() / MANIFOLD_PROPERTY_MAX);

				size_t begin = mesh.vertProperties.size();
				mesh.vertProperties.resize(mesh.vertProperties.size() + MANIFOLD_PROPERTY_MAX);
				// Add the vertex properties.
				// Use CSGBrush constants rather than push_back for clarity.
				double *vert = &mesh.vertProperties[begin];
				vert[MANIFOLD_PROPERTY_POSITION_X] = face.vertices[i].x;
				vert[MANIFOLD_PROPERTY_POSITION_Y] = face.vertices[i].y;
				vert[MANIFOLD_PROPERTY_POSITION_Z] = face.vertices[i].z;
				vert[MANIFOLD_PROPERTY_UV_X_0] = face.uvs[i].x;
				vert[MANIFOLD_PROPERTY_UV_Y_0] = face.uvs[i].y;
				vert[MANIFOLD_PROPERTY_SMOOTH_GROUP] = face.smooth ? 1.0f : 0.0f;
				vert[MANIFOLD_PROPERTY_INVERT] = face.invert ? 1.0f : 0.0f;
			}
		}
	}
	// runIndex needs an explicit end value.
	mesh.runIndex.push_back(mesh.triVerts.size());
	mesh.tolerance = 2 * FLT_EPSILON;
	ERR_FAIL_COND_MSG(mesh.vertProperties.size() % mesh.numProp != 0, "Invalid vertex properties size.");
	mesh.Merge();
#ifdef DEV_ENABLED
	print_verbose(_export_meshgl_as_json(mesh));
#endif // DEV_ENABLED
	r_manifold = manifold::Manifold(mesh);
}

static manifold::OpType _convert_csg_operation(CSGShape3D::Operation p_operation) {
	switch (p_operation) {
		case CSGShape3D::OPERATION_SUBTRACTION:
			return manifold::OpType::Subtract;
		case CSGShape3D::OPERATION_INTERSECTION:
			return manifold::OpType::Intersect;
		default:
			return manifold::OpType::Add;
	}
}

static manifold::mat3x4 _to_manifold_transform(const Transform3D &p_transform) {
	const Vector3 basis_x = p_transform.basis.get_column(0);
	const Vector3 basis_y = p_transform.basis.get_column(1);
	const Vector3 basis_z = p_transform.basis.get_column(2);
	const Vector3 origin = p_transform.origin;
	return manifold::mat3x4(
			manifold::mat3(
					manifold::vec3(basis_x.x, basis_x.y, basis_x.z),
					manifold::vec3(basis_y.x, basis_y.y, basis_y.z),
					manifold::vec3(basis_z.x, basis_z.y, basis_z.z)),
			manifold::vec3(origin.x, origin.y, origin.z));
}

static manifold::Manifold _combine_manifolds(const std::vector<manifold::Manifold> &p_manifolds, manifold::OpType p_operation) {
	if (p_manifolds.empty()) {
		return manifold::Manifold();
	}
	if (p_manifolds.size() == 1) {
		return p_manifolds.front();
	}
#ifdef DEV_ENABLED
	CSGDebugCounters::count_batch_boolean_call();
#endif // DEV_ENABLED
	return manifold::Manifold::BatchBoolean(p_manifolds, p_operation);
}

void CSGShape3D::_ensure_local_manifold() {
	const uint32_t previous_schema_generation = surface_schema_generation;
	_synchronize_surface_schema();
	if (surface_schema_generation != previous_schema_generation) {
		manifold_cache->local_manifold_dirty = true;
	}
	if (!manifold_cache->local_manifold_dirty) {
		return;
	}

	if (manifold_cache->local_brush) {
		memdelete(manifold_cache->local_brush);
	}
	manifold_cache->local_brush = _build_brush();
	manifold_cache->local_manifold = manifold::Manifold();
	manifold_cache->surface_records.clear();

	const uint32_t schema_size = _get_surface_schema_size();
	if (manifold_cache->origin_schema_generation != surface_schema_generation) {
		manifold_cache->origin_base = schema_size > 0 ? manifold::Manifold::ReserveIDs(schema_size) : 0;
		manifold_cache->origin_count = schema_size;
		manifold_cache->origin_schema_generation = surface_schema_generation;
	}

#ifdef DEV_ENABLED
	const bool is_primitive = Object::cast_to<CSGPrimitive3D>(this);
	if (is_primitive) {
		CSGDebugCounters::count_local_primitive_brush_pack();
	}
#endif // DEV_ENABLED
	_pack_manifold(
			manifold_cache->local_brush,
			manifold_cache->local_manifold,
			manifold_cache->origin_base,
			schema_size,
			get_instance_id(),
			surface_schema_generation,
			manifold_cache->surface_records);
#ifdef DEV_ENABLED
	if (is_primitive) {
		CSGDebugCounters::count_leaf_manifold_repack();
	}
#endif // DEV_ENABLED
	manifold_cache->local_manifold_dirty = false;
}

void CSGShape3D::_ensure_subtree_manifold() {
	if (!manifold_cache->subtree_manifold_dirty) {
		return;
	}

	_ensure_local_manifold();
	manifold::OpType current_op = _convert_csg_operation(get_operation());
	std::vector<manifold::Manifold> manifolds;
	manifolds.push_back(manifold_cache->local_manifold);
	for (int i = 0; i < get_child_count(); i++) {
		CSGShape3D *child = Object::cast_to<CSGShape3D>(get_child(i));
		if (!child || !child->is_visible()) {
			continue;
		}
		child->_ensure_transformed_manifold();
		manifold::OpType child_operation = _convert_csg_operation(child->get_operation());
		if (child_operation != current_op) {
#ifdef DEV_ENABLED
			CSGDebugCounters::count_operation_switch_flush();
#endif // DEV_ENABLED
			manifold::Manifold result = _combine_manifolds(manifolds, current_op);
			manifolds.clear();
			manifolds.push_back(result);
			current_op = child_operation;
		}
		manifolds.push_back(child->manifold_cache->transformed_manifold);
	}
	manifold_cache->subtree_manifold = _combine_manifolds(manifolds, current_op);
	manifold_cache->subtree_manifold_dirty = false;
	manifold_cache->transformed_manifold_dirty = true;
	manifold_cache->materialization_dirty = true;
	manifold_cache->subtree_empty_valid = false;
#ifdef DEV_ENABLED
	CSGDebugCounters::count_expression_node_reconstruction();
#endif // DEV_ENABLED

	update_configuration_warnings();
}

void CSGShape3D::_ensure_transformed_manifold() {
	_ensure_subtree_manifold();
	if (!manifold_cache->transformed_manifold_dirty) {
		return;
	}

	manifold_cache->transformed_manifold = manifold_cache->subtree_manifold.Transform(_to_manifold_transform(get_transform()));
	manifold_cache->transformed_manifold_dirty = false;
#ifdef DEV_ENABLED
	CSGDebugCounters::count_transformed_wrapper_construction();
#endif // DEV_ENABLED
}

void CSGShape3D::_gather_manifold_surface_records(HashMap<CSGOriginToken, Ref<Material>> &r_mesh_materials, HashMap<CSGOriginToken, CSGSurfaceKey> &r_surface_keys) {
	_ensure_local_manifold();
	for (const CSGManifoldSurfaceRecord &record : manifold_cache->surface_records) {
		r_mesh_materials.insert(record.origin_token, _resolve_manifold_material(record.source_material));
		r_surface_keys.insert(record.origin_token, record.surface);
	}

	for (int i = 0; i < get_child_count(); i++) {
		CSGShape3D *child = Object::cast_to<CSGShape3D>(get_child(i));
		if (child && child->is_visible()) {
			child->_gather_manifold_surface_records(r_mesh_materials, r_surface_keys);
		}
	}
}

void CSGShape3D::_update_cached_aabb_from_manifold() {
	_ensure_subtree_manifold();
	// IsEmpty()/BoundingBox() collapse the handle they are called on into an
	// evaluated leaf (the same GetCsgLeafNode() subtlety as GetMeshGL64()).
	// Evaluate a copy so the cached subtree keeps its operation-node handle and
	// preserves the clean-subtree identity invariant.
	manifold::Manifold evaluated_manifold = manifold_cache->subtree_manifold;
	manifold_cache->subtree_empty = evaluated_manifold.IsEmpty();
	manifold_cache->subtree_empty_valid = true;
	if (manifold_cache->subtree_empty) {
		node_aabb = AABB();
		return;
	}

	const manifold::Box bounds = evaluated_manifold.BoundingBox();
	const Vector3 position(bounds.min.x, bounds.min.y, bounds.min.z);
	const Vector3 end(bounds.max.x, bounds.max.y, bounds.max.z);
	node_aabb = AABB(position, end - position);
}

void CSGShape3D::_update_child_manifold_aabbs() {
	for (int i = 0; i < get_child_count(); i++) {
		CSGShape3D *child = Object::cast_to<CSGShape3D>(get_child(i));
		if (!child || !child->is_visible()) {
			continue;
		}
		child->_update_cached_aabb_from_manifold();
		child->_update_child_manifold_aabbs();
		child->update_configuration_warnings();
	}
}

CSGBrush *CSGShape3D::_get_brush() {
	if (!manifold_cache->materialization_dirty && brush) {
		dirty = false;
		return brush;
	}

	_ensure_subtree_manifold();
	HashMap<CSGOriginToken, Ref<Material>> mesh_materials;
	HashMap<CSGOriginToken, CSGSurfaceKey> surface_keys;
	_gather_manifold_surface_records(mesh_materials, surface_keys);

	if (brush) {
		memdelete(brush);
	}
	brush = memnew(CSGBrush);
#ifdef DEV_ENABLED
	if (is_root_shape()) {
		CSGDebugCounters::count_root_materialization();
	} else {
		CSGDebugCounters::count_non_root_materialization();
	}
#endif // DEV_ENABLED
	// GetMeshGL64() replaces the handle it is called on with an evaluated leaf.
	// Materialize a copy so the authored subtree keeps its operation-node handle.
	manifold::Manifold manifold_result = manifold_cache->subtree_manifold;
	Vector<CSGManifoldResultTriangle> result_triangles;
	_unpack_manifold(manifold_result, mesh_materials, brush, is_root_shape() ? &result_triangles : nullptr);
	if (is_root_shape()) {
		manifold_cache->result_surface_keys = surface_keys;
		manifold_cache->result_triangles = result_triangles;
		result_generation++;
		if (result_generation == 0) {
			result_generation = 1;
		}
	}

	AABB aabb;
	if (!brush->faces.is_empty()) {
		aabb.position = brush->faces[0].vertices[0];
		for (const CSGBrush::Face &face : brush->faces) {
			for (int i = 0; i < 3; ++i) {
				aabb.expand_to(face.vertices[i]);
			}
		}
	}
	node_aabb = aabb;
	manifold_cache->subtree_empty = brush->faces.is_empty();
	manifold_cache->subtree_empty_valid = true;
	manifold_cache->materialization_dirty = false;
	dirty = false;
	_update_child_manifold_aabbs();
	update_configuration_warnings();
	return brush;
}

static void _generate_tangents_unindexed(float *p_tangents, size_t p_count, const Vector3 *p_positions, const Vector3 *p_normals, const Vector2 *p_uvs) {
	ERR_FAIL_COND_MSG(!SurfaceTool::generate_tangents_func, "Meshoptimizer library is not initialized.");
	ERR_FAIL_COND(p_count % 3 != 0);

	if (p_count == 0) {
		return;
	}

	struct TangentVertex {
		float position[3];
		float normal[3];
		float uv[2];
	};

	// We can't operate on input arrays directly because in double-precision builds, vectors use double components
	// So we convert the inputs to single precision floats before generating tangents.
	LocalVector<TangentVertex> tangent_vertices;
	tangent_vertices.resize(p_count);

	for (size_t i = 0; i < p_count; i++) {
		TangentVertex &tangent_vertex = tangent_vertices[i];

		tangent_vertex.position[0] = p_positions[i].x;
		tangent_vertex.position[1] = p_positions[i].y;
		tangent_vertex.position[2] = p_positions[i].z;
		tangent_vertex.normal[0] = p_normals[i].x;
		tangent_vertex.normal[1] = p_normals[i].y;
		tangent_vertex.normal[2] = p_normals[i].z;
		tangent_vertex.uv[0] = p_uvs[i].x;
		tangent_vertex.uv[1] = p_uvs[i].y;
	}

	SurfaceTool::generate_tangents_func(p_tangents, nullptr, p_count,
			tangent_vertices.ptr()->position, p_count, sizeof(TangentVertex),
			tangent_vertices.ptr()->normal, sizeof(TangentVertex),
			tangent_vertices.ptr()->uv, sizeof(TangentVertex), 0);
}

void CSGShape3D::update_shape() {
	if (!is_root_shape()) {
		return;
	}

	set_base(RID());
	root_mesh.unref(); //byebye root mesh

	CSGBrush *n = _get_brush();
	ERR_FAIL_NULL_MSG(n, "Cannot get CSGBrush.");

	Vector<int> face_count;
	face_count.resize(n->materials.size() + 1);
	face_count.fill(0);

	Vector<ShapeUpdateSurface> surfaces;
	surfaces.resize(face_count.size());

	if (autosmooth) {
		_build_surfaces_smoothed(n, surfaces, face_count);
	} else {
		_build_surfaces_default(n, surfaces, face_count);
	}
#ifdef DEV_ENABLED
	CSGDebugCounters::count_uv_finalization();
	bool tangent_finalization_counted = false;
#endif // DEV_ENABLED

	root_mesh.instantiate();
	//create surfaces

	for (int i = 0; i < surfaces.size(); i++) {
		// calculate tangents for this surface
		bool have_tangents = calculate_tangents && SurfaceTool::generate_tangents_func;
		if (have_tangents) {
#ifdef DEV_ENABLED
			if (!tangent_finalization_counted) {
				CSGDebugCounters::count_tangent_finalization();
				tangent_finalization_counted = true;
			}
#endif // DEV_ENABLED
			ShapeUpdateSurface &surface = surfaces.write[i];

			_generate_tangents_unindexed(surface.tansw, surface.vertices.size(), surface.verticesw, surface.normalsw, surface.uvsw);
		}

		if (surfaces[i].last_added == 0) {
			continue;
		}

		// and convert to surface array
		Array array;
		array.resize(Mesh::ARRAY_MAX);

		array[Mesh::ARRAY_VERTEX] = surfaces[i].vertices;
		array[Mesh::ARRAY_NORMAL] = surfaces[i].normals;
		array[Mesh::ARRAY_TEX_UV] = surfaces[i].uvs;
		if (have_tangents) {
			array[Mesh::ARRAY_TANGENT] = surfaces[i].tans;
		}

		int idx = root_mesh->get_surface_count();
		root_mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, array);
		root_mesh->surface_set_material(idx, surfaces[i].material);
	}

	set_base(root_mesh->get_rid());

	update_gizmos();

#ifndef PHYSICS_3D_DISABLED
	_update_collision_faces();
#endif // PHYSICS_3D_DISABLED
}

void CSGShape3D::_build_surfaces_smoothed(CSGBrush *p_brush, Vector<CSGShape3D::ShapeUpdateSurface> &r_surfaces, Vector<int> &r_face_count) {
	Vector<Vector3> smooth_faces;
	LocalVector<Vector3> smooth_vertex;
	smooth_faces.resize(p_brush->faces.size());
	smooth_vertex.resize(p_brush->faces.size() * 3);

	Vector3 *smooth_faces_ptrw = smooth_faces.ptrw();
	int *face_count_ptrw = r_face_count.ptrw();

	for (int i = 0; i < p_brush->faces.size(); i++) {
		int mat = p_brush->faces[i].material;
		ERR_CONTINUE(mat < -1 || mat >= r_face_count.size());
		int idx = mat == -1 ? r_face_count.size() - 1 : mat;

		Plane p(p_brush->faces[i].vertices[0], p_brush->faces[i].vertices[1], p_brush->faces[i].vertices[2]);

		smooth_faces_ptrw[i] = p.normal;
		// Not sure if resize populates the LocalVector.
		smooth_vertex[i * 3 + 0] = Vector3(p.normal);
		smooth_vertex[i * 3 + 1] = Vector3(p.normal);
		smooth_vertex[i * 3 + 2] = Vector3(p.normal);
		// We could use a AHashMap Vector3, int to store the number of connections of each vertex position and end the loop earlier. But I'm not sure if the performance gains outweigh the cost.
		face_count_ptrw[idx]++;
	}

	const Vector3 *smooth_faces_ptr = smooth_faces.ptr();
	const int smooth_faces_size = smooth_faces.size();

	// We could add a `use_groups` property later to only apply autosmooth on smooth faces or respect smoothing groups in some way.
	if (smoothing_angle > 0.1) {
		float smooth_angle_rad = Math::cos(Math::deg_to_rad(smoothing_angle));
		for (int i = 0; i < smooth_faces_size; i++) {
			for (int k = 0; k < 3; k++) {
				int curr_vert = i * 3 + k;
				// Skip the other vertices of the face as they will never occupy the same position.
				Vector3 vert_a = p_brush->faces[i].vertices[k];
				for (int j = i + 1; j < smooth_faces_size; j++) {
					// Compare the angles of faces instead of vertices.
					if (smooth_faces_ptr[i].dot(smooth_faces_ptr[j]) > smooth_angle_rad) {
						for (int h = 0; h < 3; h++) {
							Vector3 vert_b = p_brush->faces[j].vertices[h];
							if (vert_a == vert_b) {
								int curr_j = j * 3 + h;
								smooth_vertex[curr_vert] += smooth_faces_ptr[j];
								smooth_vertex[curr_j] += smooth_faces_ptr[i];
								// Skip the other 2 vertices as only one vertex of each face can connect with one vertex of other face.
								break;
							}
						}
					}
				}
				smooth_vertex[curr_vert].normalize();
			}
		}
	}

	//create arrays
	for (int i = 0; i < r_surfaces.size(); i++) {
		r_surfaces.write[i].vertices.resize(r_face_count[i] * 3);
		r_surfaces.write[i].normals.resize(r_face_count[i] * 3);
		r_surfaces.write[i].uvs.resize(r_face_count[i] * 3);
		if (calculate_tangents) {
			r_surfaces.write[i].tans.resize(r_face_count[i] * 3 * 4);
		}
		r_surfaces.write[i].last_added = 0;

		if (i != r_surfaces.size() - 1) {
			r_surfaces.write[i].material = p_brush->materials[i];
		}

		r_surfaces.write[i].verticesw = r_surfaces.write[i].vertices.ptrw();
		r_surfaces.write[i].normalsw = r_surfaces.write[i].normals.ptrw();
		r_surfaces.write[i].uvsw = r_surfaces.write[i].uvs.ptrw();
		if (calculate_tangents) {
			r_surfaces.write[i].tansw = r_surfaces.write[i].tans.ptrw();
		}
	}

	//fill arrays
	{
		for (int i = 0; i < p_brush->faces.size(); i++) {
			int order[3] = { 0, 1, 2 };

			if (p_brush->faces[i].invert) {
				SWAP(order[1], order[2]);
			}

			int mat = p_brush->faces[i].material;
			ERR_CONTINUE(mat < -1 || mat >= r_face_count.size());
			int idx = mat == -1 ? r_face_count.size() - 1 : mat;

			int last = r_surfaces[idx].last_added;

			int face_pos_i = i * 3;

			for (int j = 0; j < 3; j++) {
				Vector3 v = p_brush->faces[i].vertices[j];

				Vector3 normal = smooth_vertex[face_pos_i + j];

				if (p_brush->faces[i].invert) {
					normal = -normal;
				}

				int k = last + order[j];
				r_surfaces[idx].verticesw[k] = v;
				r_surfaces[idx].uvsw[k] = p_brush->faces[i].uvs[j];
				r_surfaces[idx].normalsw[k] = normal;

				if (calculate_tangents) {
					// zero out our tangents for now
					k *= 4;
					r_surfaces[idx].tansw[k++] = 0.0;
					r_surfaces[idx].tansw[k++] = 0.0;
					r_surfaces[idx].tansw[k++] = 0.0;
					r_surfaces[idx].tansw[k++] = 0.0;
				}
			}

			r_surfaces.write[idx].last_added += 3;
		}
	}
}

void CSGShape3D::_build_surfaces_default(CSGBrush *p_brush, Vector<CSGShape3D::ShapeUpdateSurface> &r_surfaces, Vector<int> &r_face_count) {
	AHashMap<Vector3, Vector3> vec_map;
	vec_map.reserve(p_brush->faces.size() * 3);

	for (int i = 0; i < p_brush->faces.size(); i++) {
		int mat = p_brush->faces[i].material;
		ERR_CONTINUE(mat < -1 || mat >= r_face_count.size());
		int idx = mat == -1 ? r_face_count.size() - 1 : mat;

		if (p_brush->faces[i].smooth) {
			Plane p(p_brush->faces[i].vertices[0], p_brush->faces[i].vertices[1], p_brush->faces[i].vertices[2]);

			for (int j = 0; j < 3; j++) {
				Vector3 v = p_brush->faces[i].vertices[j];
				Vector3 *vec = vec_map.getptr(v);
				if (vec) {
					*vec += p.normal;
				} else {
					vec_map.insert(v, p.normal);
				}
			}
		}

		r_face_count.write[idx]++;
	}

	//create arrays
	for (int i = 0; i < r_surfaces.size(); i++) {
		r_surfaces.write[i].vertices.resize(r_face_count[i] * 3);
		r_surfaces.write[i].normals.resize(r_face_count[i] * 3);
		r_surfaces.write[i].uvs.resize(r_face_count[i] * 3);
		if (calculate_tangents) {
			r_surfaces.write[i].tans.resize(r_face_count[i] * 3 * 4);
		}
		r_surfaces.write[i].last_added = 0;

		if (i != r_surfaces.size() - 1) {
			r_surfaces.write[i].material = p_brush->materials[i];
		}

		r_surfaces.write[i].verticesw = r_surfaces.write[i].vertices.ptrw();
		r_surfaces.write[i].normalsw = r_surfaces.write[i].normals.ptrw();
		r_surfaces.write[i].uvsw = r_surfaces.write[i].uvs.ptrw();
		if (calculate_tangents) {
			r_surfaces.write[i].tansw = r_surfaces.write[i].tans.ptrw();
		}
	}

	//fill arrays
	{
		for (int i = 0; i < p_brush->faces.size(); i++) {
			int order[3] = { 0, 1, 2 };

			if (p_brush->faces[i].invert) {
				SWAP(order[1], order[2]);
			}

			int mat = p_brush->faces[i].material;
			ERR_CONTINUE(mat < -1 || mat >= r_face_count.size());
			int idx = mat == -1 ? r_face_count.size() - 1 : mat;

			int last = r_surfaces[idx].last_added;

			Plane p(p_brush->faces[i].vertices[0], p_brush->faces[i].vertices[1], p_brush->faces[i].vertices[2]);

			for (int j = 0; j < 3; j++) {
				Vector3 v = p_brush->faces[i].vertices[j];

				Vector3 normal = p.normal;

				if (p_brush->faces[i].smooth) {
					Vector3 *ptr = vec_map.getptr(v);
					if (ptr) {
						normal = ptr->normalized();
					}
				}

				if (p_brush->faces[i].invert) {
					normal = -normal;
				}

				int k = last + order[j];
				r_surfaces[idx].verticesw[k] = v;
				r_surfaces[idx].uvsw[k] = p_brush->faces[i].uvs[j];
				r_surfaces[idx].normalsw[k] = normal;

				if (calculate_tangents) {
					// zero out our tangents for now
					k *= 4;
					r_surfaces[idx].tansw[k++] = 0.0;
					r_surfaces[idx].tansw[k++] = 0.0;
					r_surfaces[idx].tansw[k++] = 0.0;
					r_surfaces[idx].tansw[k++] = 0.0;
				}
			}

			r_surfaces.write[idx].last_added += 3;
		}
	}
}

Ref<ArrayMesh> CSGShape3D::bake_static_mesh() {
	Ref<ArrayMesh> baked_mesh;
	if (is_root_shape() && root_mesh.is_valid()) {
		baked_mesh = root_mesh;
	}
	return baked_mesh;
}

#ifndef PHYSICS_3D_DISABLED
Vector<Vector3> CSGShape3D::_get_brush_collision_faces() {
	Vector<Vector3> collision_faces;
	CSGBrush *n = _get_brush();
	ERR_FAIL_NULL_V_MSG(n, collision_faces, "Cannot get CSGBrush.");
	collision_faces.resize(n->faces.size() * 3);
	Vector3 *collision_faces_ptrw = collision_faces.ptrw();

	for (int i = 0; i < n->faces.size(); i++) {
		int order[3] = { 0, 1, 2 };

		if (n->faces[i].invert) {
			SWAP(order[1], order[2]);
		}

		collision_faces_ptrw[i * 3 + 0] = n->faces[i].vertices[order[0]];
		collision_faces_ptrw[i * 3 + 1] = n->faces[i].vertices[order[1]];
		collision_faces_ptrw[i * 3 + 2] = n->faces[i].vertices[order[2]];
	}

	return collision_faces;
}

void CSGShape3D::_update_collision_faces() {
	if (use_collision && is_root_shape() && root_collision_shape.is_valid()) {
#ifdef DEV_ENABLED
		CSGDebugCounters::count_collision_rebuild();
#endif // DEV_ENABLED
		root_collision_shape->set_faces(_get_brush_collision_faces());

		if (_is_debug_collision_shape_visible()) {
			_update_debug_collision_shape();
		}
	}
}

Ref<ConcavePolygonShape3D> CSGShape3D::bake_collision_shape() {
	Ref<ConcavePolygonShape3D> baked_collision_shape;
	if (is_root_shape() && root_collision_shape.is_valid()) {
		baked_collision_shape.instantiate();
		baked_collision_shape->set_faces(root_collision_shape->get_faces());
	} else if (is_root_shape()) {
		baked_collision_shape.instantiate();
		baked_collision_shape->set_faces(_get_brush_collision_faces());
	}
	return baked_collision_shape;
}

bool CSGShape3D::_is_debug_collision_shape_visible() {
	return !Engine::get_singleton()->is_editor_hint() && is_inside_tree() && get_tree()->is_debugging_collisions_hint();
}

void CSGShape3D::_update_debug_collision_shape() {
	if (!use_collision || !is_root_shape() || root_collision_shape.is_null() || !_is_debug_collision_shape_visible()) {
		return;
	}

	ERR_FAIL_NULL(RenderingServer::get_singleton());

	if (root_collision_debug_instance.is_null()) {
		root_collision_debug_instance = RS::get_singleton()->instance_create();
	}

	Ref<Mesh> debug_mesh = root_collision_shape->get_debug_mesh();
	RS::get_singleton()->instance_set_scenario(root_collision_debug_instance, get_world_3d()->get_scenario());
	RS::get_singleton()->instance_set_base(root_collision_debug_instance, debug_mesh->get_rid());
	RS::get_singleton()->instance_set_transform(root_collision_debug_instance, get_global_transform());
}

void CSGShape3D::_clear_debug_collision_shape() {
	if (root_collision_debug_instance.is_valid()) {
		RS::get_singleton()->free_rid(root_collision_debug_instance);
		root_collision_debug_instance = RID();
	}
}

void CSGShape3D::_on_transform_changed() {
	if (root_collision_debug_instance.is_valid() && !debug_shape_old_transform.is_equal_approx(get_global_transform())) {
		debug_shape_old_transform = get_global_transform();
		RS::get_singleton()->instance_set_transform(root_collision_debug_instance, debug_shape_old_transform);
	}
}
#endif // PHYSICS_3D_DISABLED

AABB CSGShape3D::get_aabb() const {
	return node_aabb;
}

Vector<Vector3> CSGShape3D::get_brush_faces() {
	ERR_FAIL_COND_V(!is_inside_tree(), Vector<Vector3>());
	CSGBrush *b = _get_brush();
	if (!b) {
		return Vector<Vector3>();
	}

	Vector<Vector3> faces;
	int fc = b->faces.size();
	faces.resize(fc * 3);
	{
		Vector3 *w = faces.ptrw();
		for (int i = 0; i < fc; i++) {
			w[i * 3 + 0] = b->faces[i].vertices[0];
			w[i * 3 + 1] = b->faces[i].vertices[1];
			w[i * 3 + 2] = b->faces[i].vertices[2];
		}
	}

	return faces;
}

void CSGShape3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_PARENTED: {
			Node *parentn = get_parent();
			if (parentn) {
				parent_shape = Object::cast_to<CSGShape3D>(parentn);
				if (parent_shape) {
					set_base(RID());
					root_mesh.unref();
					dirty = false;
					parent_shape->_invalidate_subtree_and_ancestors();
				}
			}
			if (!parent_shape && !brush) {
				_queue_root_update();
			}
			last_visible = is_visible();
		} break;

		case NOTIFICATION_UNPARENTED: {
			if (!is_root_shape()) {
				// The subtree itself stays valid. Only its previous parent expression changes.
				parent_shape->_invalidate_subtree_and_ancestors();
				parent_shape = nullptr;
				_queue_root_update(true); // Must be forced because this node only becomes a root after the notification.
			} else {
				parent_shape = nullptr;
			}
		} break;

		case NOTIFICATION_CHILD_ORDER_CHANGED: {
			_invalidate_subtree_and_ancestors();
		} break;

		case NOTIFICATION_VISIBILITY_CHANGED: {
			if (!is_root_shape() && last_visible != is_visible()) {
				// Update this node's parent only if its own visibility has changed, not the visibility of parent nodes
				parent_shape->_invalidate_subtree_and_ancestors();
			}
			last_visible = is_visible();
		} break;

		case NOTIFICATION_LOCAL_TRANSFORM_CHANGED: {
			// A root transform does not affect its local CSG result, but its wrapper must
			// still be invalidated in case the node is subsequently reparented.
			_make_transform_dirty();
		} break;

#ifndef PHYSICS_3D_DISABLED
		case NOTIFICATION_ENTER_TREE: {
			if (use_collision && is_root_shape()) {
				root_collision_shape.instantiate();
				root_collision_body = PhysicsServer3D::get_singleton()->body_create();
				PhysicsServer3D::get_singleton()->body_set_mode(root_collision_body, PS3DE::BODY_MODE_STATIC);
				PhysicsServer3D::get_singleton()->body_set_state(root_collision_body, PS3DE::BODY_STATE_TRANSFORM, get_global_transform());
				PhysicsServer3D::get_singleton()->body_add_shape(root_collision_body, root_collision_shape->get_rid());
				PhysicsServer3D::get_singleton()->body_set_space(root_collision_body, get_world_3d()->get_space());
				PhysicsServer3D::get_singleton()->body_attach_object_instance_id(root_collision_body, get_instance_id());
				set_collision_layer(collision_layer);
				set_collision_mask(collision_mask);
				set_collision_priority(collision_priority);
				debug_shape_old_transform = get_global_transform();
				_make_output_dirty();
			}
		} break;

		case NOTIFICATION_EXIT_TREE: {
			if (use_collision && is_root_shape() && root_collision_body.is_valid()) {
				PhysicsServer3D::get_singleton()->free_rid(root_collision_body);
				root_collision_body = RID();
				root_collision_shape.unref();
				_clear_debug_collision_shape();
			}
		} break;

		case NOTIFICATION_TRANSFORM_CHANGED: {
			if (use_collision && is_root_shape() && root_collision_body.is_valid()) {
				PhysicsServer3D::get_singleton()->body_set_state(root_collision_body, PS3DE::BODY_STATE_TRANSFORM, get_global_transform());
			}
			_on_transform_changed();
		} break;
#endif // PHYSICS_3D_DISABLED
	}
}

void CSGShape3D::set_operation(Operation p_operation) {
	operation = p_operation;
	_make_operation_dirty();
	update_gizmos();
}

CSGShape3D::Operation CSGShape3D::get_operation() const {
	return operation;
}

void CSGShape3D::set_calculate_tangents(bool p_calculate_tangents) {
	calculate_tangents = p_calculate_tangents;
	_make_output_dirty();
}

bool CSGShape3D::is_calculating_tangents() const {
	return calculate_tangents;
}

void CSGShape3D::_validate_property(PropertyInfo &p_property) const {
	if (!Engine::get_singleton()->is_editor_hint()) {
		return;
	}

	if (p_property.name == "smoothing_angle") {
		if (!autosmooth || (is_inside_tree() && !is_root_shape())) {
			p_property.usage = PROPERTY_USAGE_NO_EDITOR;
		}
	}

	if (p_property.name == "autosmooth") {
		if (is_inside_tree() && !is_root_shape()) {
			p_property.usage = PROPERTY_USAGE_NO_EDITOR;
		}
	}

	bool is_collision_prefixed = p_property.name.begins_with("collision_");
	if ((is_collision_prefixed || p_property.name.begins_with("use_collision")) && is_inside_tree() && !is_root_shape()) {
		//hide collision if not root
		p_property.usage = PROPERTY_USAGE_NO_EDITOR;
	} else if (is_collision_prefixed && !bool(get("use_collision"))) {
		p_property.usage = PROPERTY_USAGE_NO_EDITOR;
	}
}

Array CSGShape3D::get_meshes() const {
	if (root_mesh.is_valid()) {
		Array arr;
		arr.resize(2);
		arr[0] = Transform3D();
		arr[1] = root_mesh;
		return arr;
	}

	return Array();
}

PackedStringArray CSGShape3D::get_configuration_warnings() const {
	PackedStringArray warnings = Node::get_configuration_warnings();
	const CSGShape3D *current_shape = this;
	while (current_shape) {
		if (!current_shape->manifold_cache->subtree_empty_valid) {
			const_cast<CSGShape3D *>(current_shape)->_update_cached_aabb_from_manifold();
		}
		if (current_shape->manifold_cache->subtree_empty) {
			warnings.push_back(RTR("The CSGShape3D has an empty shape.\nCSGShape3D empty shapes typically occur because the mesh is not manifold.\nA manifold mesh forms a solid object without gaps, holes, or loose edges.\nEach edge must be a member of exactly two faces."));
			break;
		}
		current_shape = current_shape->parent_shape;
	}
	return warnings;
}

Ref<TriangleMesh> CSGShape3D::generate_triangle_mesh() const {
	if (root_mesh.is_valid()) {
		return root_mesh->generate_triangle_mesh();
	}
	return Ref<TriangleMesh>();
}

void CSGShape3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("is_root_shape"), &CSGShape3D::is_root_shape);

	ClassDB::bind_method(D_METHOD("set_operation", "operation"), &CSGShape3D::set_operation);
	ClassDB::bind_method(D_METHOD("get_operation"), &CSGShape3D::get_operation);

#ifndef DISABLE_DEPRECATED
	ClassDB::bind_method(D_METHOD("_update_shape"), &CSGShape3D::update_shape);
	ClassDB::bind_method(D_METHOD("set_snap", "snap"), &CSGShape3D::set_snap);
	ClassDB::bind_method(D_METHOD("get_snap"), &CSGShape3D::get_snap);
#endif // DISABLE_DEPRECATED

#ifndef PHYSICS_3D_DISABLED
	ClassDB::bind_method(D_METHOD("set_use_collision", "operation"), &CSGShape3D::set_use_collision);
	ClassDB::bind_method(D_METHOD("is_using_collision"), &CSGShape3D::is_using_collision);

	ClassDB::bind_method(D_METHOD("set_collision_layer", "layer"), &CSGShape3D::set_collision_layer);
	ClassDB::bind_method(D_METHOD("get_collision_layer"), &CSGShape3D::get_collision_layer);

	ClassDB::bind_method(D_METHOD("set_collision_mask", "mask"), &CSGShape3D::set_collision_mask);
	ClassDB::bind_method(D_METHOD("get_collision_mask"), &CSGShape3D::get_collision_mask);

	ClassDB::bind_method(D_METHOD("set_collision_mask_value", "layer_number", "value"), &CSGShape3D::set_collision_mask_value);
	ClassDB::bind_method(D_METHOD("get_collision_mask_value", "layer_number"), &CSGShape3D::get_collision_mask_value);

	ClassDB::bind_method(D_METHOD("_get_root_collision_instance"), &CSGShape3D::_get_root_collision_instance);

	ClassDB::bind_method(D_METHOD("set_collision_layer_value", "layer_number", "value"), &CSGShape3D::set_collision_layer_value);
	ClassDB::bind_method(D_METHOD("get_collision_layer_value", "layer_number"), &CSGShape3D::get_collision_layer_value);

	ClassDB::bind_method(D_METHOD("set_collision_priority", "priority"), &CSGShape3D::set_collision_priority);
	ClassDB::bind_method(D_METHOD("get_collision_priority"), &CSGShape3D::get_collision_priority);

	ClassDB::bind_method(D_METHOD("bake_collision_shape"), &CSGShape3D::bake_collision_shape);
#endif // PHYSICS_3D_DISABLED

	ClassDB::bind_method(D_METHOD("set_calculate_tangents", "enabled"), &CSGShape3D::set_calculate_tangents);
	ClassDB::bind_method(D_METHOD("is_calculating_tangents"), &CSGShape3D::is_calculating_tangents);

	ClassDB::bind_method(D_METHOD("get_meshes"), &CSGShape3D::get_meshes);

	ClassDB::bind_method(D_METHOD("bake_static_mesh"), &CSGShape3D::bake_static_mesh);

	ClassDB::bind_method(D_METHOD("set_autosmooth", "autosmooth"), &CSGShape3D::set_autosmooth);
	ClassDB::bind_method(D_METHOD("is_autosmooth"), &CSGShape3D::is_autosmooth);

	ClassDB::bind_method(D_METHOD("set_smoothing_angle", "smoothing_angle"), &CSGShape3D::set_smoothing_angle);
	ClassDB::bind_method(D_METHOD("get_smoothing_angle"), &CSGShape3D::get_smoothing_angle);

	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "autosmooth"), "set_autosmooth", "is_autosmooth");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "smoothing_angle", PROPERTY_HINT_RANGE, "0,180,0.1,degrees"), "set_smoothing_angle", "get_smoothing_angle");

	ADD_PROPERTY(PropertyInfo(Variant::INT, "operation", PROPERTY_HINT_ENUM, "Union,Intersection,Subtraction"), "set_operation", "get_operation");
#ifndef DISABLE_DEPRECATED
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "snap", PROPERTY_HINT_RANGE, "0.000001,1,0.000001,suffix:m", PROPERTY_USAGE_NONE), "set_snap", "get_snap");
#endif // DISABLE_DEPRECATED
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "calculate_tangents"), "set_calculate_tangents", "is_calculating_tangents");

#ifndef PHYSICS_3D_DISABLED
	ADD_GROUP("Collision", "collision_");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "use_collision"), "set_use_collision", "is_using_collision");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "collision_layer", PROPERTY_HINT_LAYERS_3D_PHYSICS), "set_collision_layer", "get_collision_layer");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "collision_mask", PROPERTY_HINT_LAYERS_3D_PHYSICS), "set_collision_mask", "get_collision_mask");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "collision_priority"), "set_collision_priority", "get_collision_priority");
#endif // PHYSICS_3D_DISABLED

	BIND_ENUM_CONSTANT(OPERATION_UNION);
	BIND_ENUM_CONSTANT(OPERATION_INTERSECTION);
	BIND_ENUM_CONSTANT(OPERATION_SUBTRACTION);
}

CSGShape3D::CSGShape3D() {
	manifold_cache = memnew(ManifoldCache);
	set_notify_local_transform(true);
}

CSGShape3D::~CSGShape3D() {
	if (brush) {
		memdelete(brush);
		brush = nullptr;
	}
	memdelete(manifold_cache);
	manifold_cache = nullptr;
}

//////////////////////////////////

CSGBrush *CSGCombiner3D::_build_brush() {
	return memnew(CSGBrush); //does not build anything
}

CSGCombiner3D::CSGCombiner3D() {
}

/////////////////////

CSGBrush *CSGPrimitive3D::_create_brush_from_arrays(const Vector<Vector3> &p_vertices, const Vector<Vector2> &p_uv, const Vector<bool> &p_smooth, const Vector<Ref<Material>> &p_materials) {
	CSGBrush *new_brush = memnew(CSGBrush);

	Vector<bool> invert;
	invert.resize(p_vertices.size() / 3);
	{
		int ic = invert.size();
		bool *w = invert.ptrw();
		for (int i = 0; i < ic; i++) {
			w[i] = flip_faces;
		}
	}
	new_brush->build_from_faces(p_vertices, p_uv, p_smooth, p_materials, invert);

	return new_brush;
}

void CSGPrimitive3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_flip_faces", "flip_faces"), &CSGPrimitive3D::set_flip_faces);
	ClassDB::bind_method(D_METHOD("get_flip_faces"), &CSGPrimitive3D::get_flip_faces);

	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "flip_faces"), "set_flip_faces", "get_flip_faces");
}

void CSGPrimitive3D::set_flip_faces(bool p_invert) {
	if (flip_faces == p_invert) {
		return;
	}

	flip_faces = p_invert;

	_make_dirty();
}

bool CSGPrimitive3D::get_flip_faces() {
	return flip_faces;
}

CSGPrimitive3D::CSGPrimitive3D() {
	flip_faces = false;
}

/////////////////////

uint32_t CSGMesh3D::_get_surface_schema_size() const {
	return mesh.is_valid() ? mesh->get_surface_count() : 0;
}

CSGBrush *CSGMesh3D::_build_brush() {
	if (mesh.is_null()) {
		return memnew(CSGBrush);
	}

	Vector<Vector3> vertices;
	Vector<bool> smooth;
	Vector<Ref<Material>> materials;
	Vector<Vector2> uvs;
	Vector<uint32_t> semantic_surfaces;
	Vector<uint32_t> face_ids;

	for (int i = 0; i < mesh->get_surface_count(); i++) {
		if (mesh->surface_get_primitive_type(i) != Mesh::PRIMITIVE_TRIANGLES) {
			continue;
		}

		Array arrays = mesh->surface_get_arrays(i);

		if (arrays.is_empty()) {
			_make_dirty();
			ERR_FAIL_COND_V(arrays.is_empty(), memnew(CSGBrush));
		}

		Vector<Vector3> avertices = arrays[Mesh::ARRAY_VERTEX];
		if (avertices.is_empty()) {
			continue;
		}

		const Vector3 *vr = avertices.ptr();

		Vector<Vector3> anormals = arrays[Mesh::ARRAY_NORMAL];
		const Vector3 *nr = nullptr;
		if (anormals.size()) {
			nr = anormals.ptr();
		}

		Vector<Vector2> auvs = arrays[Mesh::ARRAY_TEX_UV];
		const Vector2 *uvr = nullptr;
		if (auvs.size()) {
			uvr = auvs.ptr();
		}

		// Preserve the source surface material in the cached leaf. The current
		// CSGMesh3D override is resolved when the root is materialized, so changing
		// only the override never requires new Manifold original IDs.
		Ref<Material> mat = mesh->surface_get_material(i);

		Vector<int> aindices = arrays[Mesh::ARRAY_INDEX];
		if (aindices.size()) {
			int as = vertices.size();
			int is = aindices.size();

			vertices.resize(as + is);
			smooth.resize((as + is) / 3);
			materials.resize((as + is) / 3);
			uvs.resize(as + is);
			semantic_surfaces.resize((as + is) / 3);
			face_ids.resize((as + is) / 3);

			Vector3 *vw = vertices.ptrw();
			bool *sw = smooth.ptrw();
			Vector2 *uvw = uvs.ptrw();
			Ref<Material> *mw = materials.ptrw();

			const int *ir = aindices.ptr();

			for (int j = 0; j < is; j += 3) {
				Vector3 vertex[3];
				Vector3 normal[3];
				Vector2 uv[3];

				for (int k = 0; k < 3; k++) {
					int idx = ir[j + k];
					vertex[k] = vr[idx];
					if (nr) {
						normal[k] = nr[idx];
					}
					if (uvr) {
						uv[k] = uvr[idx];
					}
				}

				bool flat = normal[0].is_equal_approx(normal[1]) && normal[0].is_equal_approx(normal[2]);

				vw[as + j + 0] = vertex[0];
				vw[as + j + 1] = vertex[1];
				vw[as + j + 2] = vertex[2];

				uvw[as + j + 0] = uv[0];
				uvw[as + j + 1] = uv[1];
				uvw[as + j + 2] = uv[2];

				const int face_index = (as + j) / 3;
				sw[face_index] = !flat;
				mw[face_index] = mat;
				semantic_surfaces.write[face_index] = i;
				face_ids.write[face_index] = j / 3;
			}
		} else {
			int as = vertices.size();
			int is = avertices.size();

			vertices.resize(as + is);
			smooth.resize((as + is) / 3);
			uvs.resize(as + is);
			materials.resize((as + is) / 3);
			semantic_surfaces.resize((as + is) / 3);
			face_ids.resize((as + is) / 3);

			Vector3 *vw = vertices.ptrw();
			bool *sw = smooth.ptrw();
			Vector2 *uvw = uvs.ptrw();
			Ref<Material> *mw = materials.ptrw();

			for (int j = 0; j < is; j += 3) {
				Vector3 vertex[3];
				Vector3 normal[3];
				Vector2 uv[3];

				for (int k = 0; k < 3; k++) {
					vertex[k] = vr[j + k];
					if (nr) {
						normal[k] = nr[j + k];
					}
					if (uvr) {
						uv[k] = uvr[j + k];
					}
				}

				bool flat = normal[0].is_equal_approx(normal[1]) && normal[0].is_equal_approx(normal[2]);

				vw[as + j + 0] = vertex[0];
				vw[as + j + 1] = vertex[1];
				vw[as + j + 2] = vertex[2];

				uvw[as + j + 0] = uv[0];
				uvw[as + j + 1] = uv[1];
				uvw[as + j + 2] = uv[2];

				const int face_index = (as + j) / 3;
				sw[face_index] = !flat;
				mw[face_index] = mat;
				semantic_surfaces.write[face_index] = i;
				face_ids.write[face_index] = j / 3;
			}
		}
	}

	if (vertices.is_empty()) {
		return memnew(CSGBrush);
	}

	CSGBrush *new_brush = _create_brush_from_arrays(vertices, uvs, smooth, materials);
	for (int face_i = 0; face_i < new_brush->faces.size(); face_i++) {
		new_brush->faces.write[face_i].semantic_surface = semantic_surfaces[face_i];
		new_brush->faces.write[face_i].face_id = face_ids[face_i];
	}
	return new_brush;
}

void CSGMesh3D::_mesh_changed() {
	_synchronize_surface_schema();
	_make_dirty();

	callable_mp((Node3D *)this, &Node3D::update_gizmos).call_deferred();
}

void CSGMesh3D::set_material(const Ref<Material> &p_material) {
	if (material == p_material) {
		return;
	}
	material = p_material;
	_make_material_dirty();
}

Ref<Material> CSGMesh3D::get_material() const {
	return material;
}

void CSGMesh3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_mesh", "mesh"), &CSGMesh3D::set_mesh);
	ClassDB::bind_method(D_METHOD("get_mesh"), &CSGMesh3D::get_mesh);

	ClassDB::bind_method(D_METHOD("set_material", "material"), &CSGMesh3D::set_material);
	ClassDB::bind_method(D_METHOD("get_material"), &CSGMesh3D::get_material);

	// Hide PrimitiveMeshes that are always non-manifold and therefore can't be used as CSG meshes.
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "mesh", PROPERTY_HINT_RESOURCE_TYPE, "Mesh,-PlaneMesh,-PointMesh,-QuadMesh,-RibbonTrailMesh"), "set_mesh", "get_mesh");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "material", PROPERTY_HINT_RESOURCE_TYPE, "BaseMaterial3D,ShaderMaterial"), "set_material", "get_material");
}

void CSGMesh3D::set_mesh(const Ref<Mesh> &p_mesh) {
	if (mesh == p_mesh) {
		return;
	}
	if (mesh.is_valid()) {
		mesh->disconnect_changed(callable_mp(this, &CSGMesh3D::_mesh_changed));
	}
	mesh = p_mesh;

	if (mesh.is_valid()) {
		mesh->connect_changed(callable_mp(this, &CSGMesh3D::_mesh_changed));
	}

	_mesh_changed();
}

Ref<Mesh> CSGMesh3D::get_mesh() {
	return mesh;
}

////////////////////////////////

// Tag every brush face with one semantic surface, giving each triangle its own
// planar faceID. Used by primitives whose whole hull is a single named surface.
static void _tag_faces_single_surface(CSGBrush *p_brush, uint32_t p_surface) {
	for (int face_i = 0; face_i < p_brush->faces.size(); face_i++) {
		p_brush->faces.write[face_i].semantic_surface = p_surface;
		p_brush->faces.write[face_i].face_id = face_i;
	}
}

CSGBrush *CSGSphere3D::_build_brush() {
	// set our bounding box

	CSGBrush *new_brush = memnew(CSGBrush);

	int face_count = rings * radial_segments * 2 - radial_segments * 2;

	bool invert_val = get_flip_faces();
	Ref<Material> base_material = get_material();

	Vector<Vector3> faces;
	Vector<Vector2> uvs;
	Vector<bool> smooth;
	Vector<Ref<Material>> materials;
	Vector<bool> invert;

	faces.resize(face_count * 3);
	uvs.resize(face_count * 3);

	smooth.resize(face_count);
	materials.resize(face_count);
	invert.resize(face_count);

	{
		Vector3 *facesw = faces.ptrw();
		Vector2 *uvsw = uvs.ptrw();
		bool *smoothw = smooth.ptrw();
		Ref<Material> *materialsw = materials.ptrw();
		bool *invertw = invert.ptrw();

		// We want to follow an order that's convenient for UVs.
		// For latitude step we start at the top and move down like in an image.
		const double latitude_step = -Math::PI / rings;
		const double longitude_step = Math::TAU / radial_segments;
		int face = 0;
		for (int i = 0; i < rings; i++) {
			double cos0 = 0;
			double sin0 = 1;
			if (i > 0) {
				double latitude0 = latitude_step * i + Math::TAU / 4;
				cos0 = Math::cos(latitude0);
				sin0 = Math::sin(latitude0);
			}
			double v0 = double(i) / rings;

			double cos1 = 0;
			double sin1 = -1;
			if (i < rings - 1) {
				double latitude1 = latitude_step * (i + 1) + Math::TAU / 4;
				cos1 = Math::cos(latitude1);
				sin1 = Math::sin(latitude1);
			}
			double v1 = double(i + 1) / rings;

			for (int j = 0; j < radial_segments; j++) {
				double longitude0 = longitude_step * j;
				// We give sin to X and cos to Z on purpose.
				// This allows UVs to be CCW on +X so it maps to images well.
				double x0 = Math::sin(longitude0);
				double z0 = Math::cos(longitude0);
				double u0 = double(j) / radial_segments;

				double longitude1 = longitude_step * (j + 1);
				if (j == radial_segments - 1) {
					longitude1 = 0;
				}

				double x1 = Math::sin(longitude1);
				double z1 = Math::cos(longitude1);
				double u1 = double(j + 1) / radial_segments;

				Vector3 v[4] = {
					Vector3(x0 * cos0, sin0, z0 * cos0) * radius,
					Vector3(x1 * cos0, sin0, z1 * cos0) * radius,
					Vector3(x1 * cos1, sin1, z1 * cos1) * radius,
					Vector3(x0 * cos1, sin1, z0 * cos1) * radius,
				};

				Vector2 u[4] = {
					Vector2(u0, v0),
					Vector2(u1, v0),
					Vector2(u1, v1),
					Vector2(u0, v1),
				};

				// Draw the first face, but skip this at the north pole (i == 0).
				if (i > 0) {
					facesw[face * 3 + 0] = v[0];
					facesw[face * 3 + 1] = v[1];
					facesw[face * 3 + 2] = v[2];

					uvsw[face * 3 + 0] = u[0];
					uvsw[face * 3 + 1] = u[1];
					uvsw[face * 3 + 2] = u[2];

					smoothw[face] = smooth_faces;
					invertw[face] = invert_val;
					materialsw[face] = base_material;

					face++;
				}

				// Draw the second face, but skip this at the south pole (i == rings - 1).
				if (i < rings - 1) {
					facesw[face * 3 + 0] = v[2];
					facesw[face * 3 + 1] = v[3];
					facesw[face * 3 + 2] = v[0];

					uvsw[face * 3 + 0] = u[2];
					uvsw[face * 3 + 1] = u[3];
					uvsw[face * 3 + 2] = u[0];

					smoothw[face] = smooth_faces;
					invertw[face] = invert_val;
					materialsw[face] = base_material;

					face++;
				}
			}
		}

		if (face != face_count) {
			ERR_PRINT("Face mismatch bug! fix code");
		}
	}

	new_brush->build_from_faces(faces, uvs, smooth, materials, invert);
	_tag_faces_single_surface(new_brush, SURFACE_BODY);

	return new_brush;
}

void CSGSphere3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_radius", "radius"), &CSGSphere3D::set_radius);
	ClassDB::bind_method(D_METHOD("get_radius"), &CSGSphere3D::get_radius);

	ClassDB::bind_method(D_METHOD("set_radial_segments", "radial_segments"), &CSGSphere3D::set_radial_segments);
	ClassDB::bind_method(D_METHOD("get_radial_segments"), &CSGSphere3D::get_radial_segments);
	ClassDB::bind_method(D_METHOD("set_rings", "rings"), &CSGSphere3D::set_rings);
	ClassDB::bind_method(D_METHOD("get_rings"), &CSGSphere3D::get_rings);

	ClassDB::bind_method(D_METHOD("set_smooth_faces", "smooth_faces"), &CSGSphere3D::set_smooth_faces);
	ClassDB::bind_method(D_METHOD("get_smooth_faces"), &CSGSphere3D::get_smooth_faces);

	ClassDB::bind_method(D_METHOD("set_material", "material"), &CSGSphere3D::set_material);
	ClassDB::bind_method(D_METHOD("get_material"), &CSGSphere3D::get_material);

	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "radius", PROPERTY_HINT_RANGE, "0.001,100.0,0.001,suffix:m"), "set_radius", "get_radius");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "radial_segments", PROPERTY_HINT_RANGE, "1,100,1"), "set_radial_segments", "get_radial_segments");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "rings", PROPERTY_HINT_RANGE, "1,100,1"), "set_rings", "get_rings");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "smooth_faces"), "set_smooth_faces", "get_smooth_faces");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "material", PROPERTY_HINT_RESOURCE_TYPE, "BaseMaterial3D,ShaderMaterial"), "set_material", "get_material");
}

void CSGSphere3D::set_radius(const float p_radius) {
	ERR_FAIL_COND(p_radius <= 0);
	radius = p_radius;
	_make_dirty();
	update_gizmos();
}

float CSGSphere3D::get_radius() const {
	return radius;
}

void CSGSphere3D::set_radial_segments(const int p_radial_segments) {
	radial_segments = p_radial_segments > 4 ? p_radial_segments : 4;
	_make_dirty();
	update_gizmos();
}

int CSGSphere3D::get_radial_segments() const {
	return radial_segments;
}

void CSGSphere3D::set_rings(const int p_rings) {
	rings = p_rings > 1 ? p_rings : 1;
	_make_dirty();
	update_gizmos();
}

int CSGSphere3D::get_rings() const {
	return rings;
}

void CSGSphere3D::set_smooth_faces(const bool p_smooth_faces) {
	smooth_faces = p_smooth_faces;
	_make_dirty();
}

bool CSGSphere3D::get_smooth_faces() const {
	return smooth_faces;
}

void CSGSphere3D::set_material(const Ref<Material> &p_material) {
	material = p_material;
	_make_material_dirty();
}

Ref<Material> CSGSphere3D::get_material() const {
	return material;
}

CSGSphere3D::CSGSphere3D() {
	// defaults
	radius = 0.5;
	radial_segments = 12;
	rings = 6;
	smooth_faces = true;
}

///////////////

CSGBrush *CSGBox3D::_build_brush() {
	// set our bounding box

	CSGBrush *new_brush = memnew(CSGBrush);

	int face_count = 12; //it's a cube..

	bool invert_val = get_flip_faces();
	Ref<Material> base_material = get_material();

	Vector<Vector3> faces;
	Vector<Vector2> uvs;
	Vector<bool> smooth;
	Vector<Ref<Material>> materials;
	Vector<bool> invert;

	faces.resize(face_count * 3);
	uvs.resize(face_count * 3);

	smooth.resize(face_count);
	materials.resize(face_count);
	invert.resize(face_count);

	{
		Vector3 *facesw = faces.ptrw();
		Vector2 *uvsw = uvs.ptrw();
		bool *smoothw = smooth.ptrw();
		Ref<Material> *materialsw = materials.ptrw();
		bool *invertw = invert.ptrw();

		int face = 0;

		Vector3 vertex_mul = size / 2;

		{
			for (int i = 0; i < 6; i++) {
				Vector3 face_points[4];
				float uv_points[8] = { 0, 0, 0, 1, 1, 1, 1, 0 };

				for (int j = 0; j < 4; j++) {
					float v[3];
					v[0] = 1.0;
					v[1] = 1 - 2 * ((j >> 1) & 1);
					v[2] = v[1] * (1 - 2 * (j & 1));

					for (int k = 0; k < 3; k++) {
						if (i < 3) {
							face_points[j][(i + k) % 3] = v[k];
						} else {
							face_points[3 - j][(i + k) % 3] = -v[k];
						}
					}
				}

				Vector2 u[4];
				for (int j = 0; j < 4; j++) {
					u[j] = Vector2(uv_points[j * 2 + 0], uv_points[j * 2 + 1]);
				}

				//face 1
				facesw[face * 3 + 0] = face_points[0] * vertex_mul;
				facesw[face * 3 + 1] = face_points[1] * vertex_mul;
				facesw[face * 3 + 2] = face_points[2] * vertex_mul;

				uvsw[face * 3 + 0] = u[0];
				uvsw[face * 3 + 1] = u[1];
				uvsw[face * 3 + 2] = u[2];

				smoothw[face] = false;
				invertw[face] = invert_val;
				materialsw[face] = base_material;

				face++;
				//face 2
				facesw[face * 3 + 0] = face_points[2] * vertex_mul;
				facesw[face * 3 + 1] = face_points[3] * vertex_mul;
				facesw[face * 3 + 2] = face_points[0] * vertex_mul;

				uvsw[face * 3 + 0] = u[2];
				uvsw[face * 3 + 1] = u[3];
				uvsw[face * 3 + 2] = u[0];

				smoothw[face] = false;
				invertw[face] = invert_val;
				materialsw[face] = base_material;

				face++;
			}
		}

		if (face != face_count) {
			ERR_PRINT("Face mismatch bug! fix code");
		}
	}

	new_brush->build_from_faces(faces, uvs, smooth, materials, invert);
	static constexpr uint32_t brush_face_surfaces[6] = {
		SURFACE_POSITIVE_X,
		SURFACE_POSITIVE_Y,
		SURFACE_POSITIVE_Z,
		SURFACE_NEGATIVE_X,
		SURFACE_NEGATIVE_Y,
		SURFACE_NEGATIVE_Z,
	};
	for (int face_i = 0; face_i < new_brush->faces.size(); face_i++) {
		// Two triangles per axis face; each box face is a single planar facet, so
		// its faceID is just its semantic surface.
		const uint32_t surface = brush_face_surfaces[face_i / 2];
		new_brush->faces.write[face_i].semantic_surface = surface;
		new_brush->faces.write[face_i].face_id = surface;
	}

	return new_brush;
}

void CSGBox3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_size", "size"), &CSGBox3D::set_size);
	ClassDB::bind_method(D_METHOD("get_size"), &CSGBox3D::get_size);

	ClassDB::bind_method(D_METHOD("set_material", "material"), &CSGBox3D::set_material);
	ClassDB::bind_method(D_METHOD("get_material"), &CSGBox3D::get_material);

	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "size", PROPERTY_HINT_NONE, "suffix:m"), "set_size", "get_size");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "material", PROPERTY_HINT_RESOURCE_TYPE, "BaseMaterial3D,ShaderMaterial"), "set_material", "get_material");
}

void CSGBox3D::set_size(const Vector3 &p_size) {
	size = p_size;
	_make_dirty();
	update_gizmos();
}

Vector3 CSGBox3D::get_size() const {
	return size;
}

#ifndef DISABLE_DEPRECATED
// Kept for compatibility from 3.x to 4.0.
bool CSGBox3D::_set(const StringName &p_name, const Variant &p_value) {
	if (p_name == "width") {
		size.x = p_value;
		_make_dirty();
		update_gizmos();
		return true;
	} else if (p_name == "height") {
		size.y = p_value;
		_make_dirty();
		update_gizmos();
		return true;
	} else if (p_name == "depth") {
		size.z = p_value;
		_make_dirty();
		update_gizmos();
		return true;
	} else {
		return false;
	}
}
#endif

void CSGBox3D::set_material(const Ref<Material> &p_material) {
	material = p_material;
	_make_material_dirty();
	update_gizmos();
}

Ref<Material> CSGBox3D::get_material() const {
	return material;
}

///////////////

CSGBrush *CSGCylinder3D::_build_brush() {
	// set our bounding box

	CSGBrush *new_brush = memnew(CSGBrush);

	int face_count = sides * (cone ? 1 : 2) + sides + (cone ? 0 : sides);

	bool invert_val = get_flip_faces();
	Ref<Material> base_material = get_material();

	Vector<Vector3> faces;
	Vector<Vector2> uvs;
	Vector<bool> smooth;
	Vector<Ref<Material>> materials;
	Vector<bool> invert;

	faces.resize(face_count * 3);
	uvs.resize(face_count * 3);

	smooth.resize(face_count);
	materials.resize(face_count);
	invert.resize(face_count);

	{
		Vector3 *facesw = faces.ptrw();
		Vector2 *uvsw = uvs.ptrw();
		bool *smoothw = smooth.ptrw();
		Ref<Material> *materialsw = materials.ptrw();
		bool *invertw = invert.ptrw();

		int face = 0;

		Vector3 vertex_mul(radius, height * 0.5, radius);

		{
			for (int i = 0; i < sides; i++) {
				float inc = float(i) / sides;
				float inc_n = float((i + 1)) / sides;
				if (i == sides - 1) {
					inc_n = 0;
				}

				float ang = inc * Math::TAU;
				float ang_n = inc_n * Math::TAU;

				Vector3 face_base(Math::cos(ang), 0, Math::sin(ang));
				Vector3 face_base_n(Math::cos(ang_n), 0, Math::sin(ang_n));

				Vector3 face_points[4] = {
					face_base + Vector3(0, -1, 0),
					face_base_n + Vector3(0, -1, 0),
					face_base_n * (cone ? 0.0 : 1.0) + Vector3(0, 1, 0),
					face_base * (cone ? 0.0 : 1.0) + Vector3(0, 1, 0),
				};

				Vector2 u[4] = {
					Vector2(inc, 0),
					Vector2(inc_n, 0),
					Vector2(inc_n, 1),
					Vector2(inc, 1),
				};

				//side face 1
				facesw[face * 3 + 0] = face_points[0] * vertex_mul;
				facesw[face * 3 + 1] = face_points[1] * vertex_mul;
				facesw[face * 3 + 2] = face_points[2] * vertex_mul;

				uvsw[face * 3 + 0] = u[0];
				uvsw[face * 3 + 1] = u[1];
				uvsw[face * 3 + 2] = u[2];

				smoothw[face] = smooth_faces;
				invertw[face] = invert_val;
				materialsw[face] = base_material;

				face++;

				if (!cone) {
					//side face 2
					facesw[face * 3 + 0] = face_points[2] * vertex_mul;
					facesw[face * 3 + 1] = face_points[3] * vertex_mul;
					facesw[face * 3 + 2] = face_points[0] * vertex_mul;

					uvsw[face * 3 + 0] = u[2];
					uvsw[face * 3 + 1] = u[3];
					uvsw[face * 3 + 2] = u[0];

					smoothw[face] = smooth_faces;
					invertw[face] = invert_val;
					materialsw[face] = base_material;
					face++;
				}

				//bottom face 1
				facesw[face * 3 + 0] = face_points[1] * vertex_mul;
				facesw[face * 3 + 1] = face_points[0] * vertex_mul;
				facesw[face * 3 + 2] = Vector3(0, -1, 0) * vertex_mul;

				uvsw[face * 3 + 0] = Vector2(face_points[1].x, face_points[1].y) * 0.5 + Vector2(0.5, 0.5);
				uvsw[face * 3 + 1] = Vector2(face_points[0].x, face_points[0].y) * 0.5 + Vector2(0.5, 0.5);
				uvsw[face * 3 + 2] = Vector2(0.5, 0.5);

				smoothw[face] = false;
				invertw[face] = invert_val;
				materialsw[face] = base_material;
				face++;

				if (!cone) {
					//top face 1
					facesw[face * 3 + 0] = face_points[3] * vertex_mul;
					facesw[face * 3 + 1] = face_points[2] * vertex_mul;
					facesw[face * 3 + 2] = Vector3(0, 1, 0) * vertex_mul;

					uvsw[face * 3 + 0] = Vector2(face_points[1].x, face_points[1].y) * 0.5 + Vector2(0.5, 0.5);
					uvsw[face * 3 + 1] = Vector2(face_points[0].x, face_points[0].y) * 0.5 + Vector2(0.5, 0.5);
					uvsw[face * 3 + 2] = Vector2(0.5, 0.5);

					smoothw[face] = false;
					invertw[face] = invert_val;
					materialsw[face] = base_material;
					face++;
				}
			}
		}

		if (face != face_count) {
			ERR_PRINT("Face mismatch bug! fix code");
		}
	}

	new_brush->build_from_faces(faces, uvs, smooth, materials, invert);
	// Faces are emitted per side in build order: the side quad (two triangles, or
	// one for a cone) followed by its bottom cap triangle and, when not a cone, its
	// top cap triangle. All sides share one SIDE surface but keep distinct faceIDs.
	const int faces_per_side = cone ? 2 : 4;
	for (int side_i = 0; side_i < sides; side_i++) {
		const int first_face = side_i * faces_per_side;
		new_brush->faces.write[first_face].semantic_surface = SURFACE_SIDE;
		new_brush->faces.write[first_face].face_id = side_i;
		int cap_face = first_face + 1;
		if (!cone) {
			new_brush->faces.write[first_face + 1].semantic_surface = SURFACE_SIDE;
			new_brush->faces.write[first_face + 1].face_id = side_i;
			cap_face++;
		}
		new_brush->faces.write[cap_face].semantic_surface = SURFACE_BOTTOM;
		new_brush->faces.write[cap_face].face_id = 0;
		if (!cone) {
			new_brush->faces.write[cap_face + 1].semantic_surface = SURFACE_TOP;
			new_brush->faces.write[cap_face + 1].face_id = 0;
		}
	}

	return new_brush;
}

void CSGCylinder3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_radius", "radius"), &CSGCylinder3D::set_radius);
	ClassDB::bind_method(D_METHOD("get_radius"), &CSGCylinder3D::get_radius);

	ClassDB::bind_method(D_METHOD("set_height", "height"), &CSGCylinder3D::set_height);
	ClassDB::bind_method(D_METHOD("get_height"), &CSGCylinder3D::get_height);

	ClassDB::bind_method(D_METHOD("set_sides", "sides"), &CSGCylinder3D::set_sides);
	ClassDB::bind_method(D_METHOD("get_sides"), &CSGCylinder3D::get_sides);

	ClassDB::bind_method(D_METHOD("set_cone", "cone"), &CSGCylinder3D::set_cone);
	ClassDB::bind_method(D_METHOD("is_cone"), &CSGCylinder3D::is_cone);

	ClassDB::bind_method(D_METHOD("set_material", "material"), &CSGCylinder3D::set_material);
	ClassDB::bind_method(D_METHOD("get_material"), &CSGCylinder3D::get_material);

	ClassDB::bind_method(D_METHOD("set_smooth_faces", "smooth_faces"), &CSGCylinder3D::set_smooth_faces);
	ClassDB::bind_method(D_METHOD("get_smooth_faces"), &CSGCylinder3D::get_smooth_faces);

	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "radius", PROPERTY_HINT_RANGE, "0.001,1000.0,0.001,or_greater,exp,suffix:m"), "set_radius", "get_radius");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "height", PROPERTY_HINT_RANGE, "0.001,1000.0,0.001,or_greater,exp,suffix:m"), "set_height", "get_height");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "sides", PROPERTY_HINT_RANGE, "3,64,1"), "set_sides", "get_sides");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "cone"), "set_cone", "is_cone");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "smooth_faces"), "set_smooth_faces", "get_smooth_faces");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "material", PROPERTY_HINT_RESOURCE_TYPE, "BaseMaterial3D,ShaderMaterial"), "set_material", "get_material");
}

void CSGCylinder3D::set_radius(const float p_radius) {
	radius = p_radius;
	_make_dirty();
	update_gizmos();
}

float CSGCylinder3D::get_radius() const {
	return radius;
}

void CSGCylinder3D::set_height(const float p_height) {
	height = p_height;
	_make_dirty();
	update_gizmos();
}

float CSGCylinder3D::get_height() const {
	return height;
}

void CSGCylinder3D::set_sides(const int p_sides) {
	ERR_FAIL_COND(p_sides < 3);
	sides = p_sides;
	_make_dirty();
	update_gizmos();
}

int CSGCylinder3D::get_sides() const {
	return sides;
}

void CSGCylinder3D::set_cone(const bool p_cone) {
	cone = p_cone;
	_make_dirty();
	update_gizmos();
}

bool CSGCylinder3D::is_cone() const {
	return cone;
}

void CSGCylinder3D::set_smooth_faces(const bool p_smooth_faces) {
	smooth_faces = p_smooth_faces;
	_make_dirty();
}

bool CSGCylinder3D::get_smooth_faces() const {
	return smooth_faces;
}

void CSGCylinder3D::set_material(const Ref<Material> &p_material) {
	material = p_material;
	_make_material_dirty();
}

Ref<Material> CSGCylinder3D::get_material() const {
	return material;
}

CSGCylinder3D::CSGCylinder3D() {
	// defaults
	radius = 0.5;
	height = 2.0;
	sides = 8;
	cone = false;
	smooth_faces = true;
}

///////////////

CSGBrush *CSGTorus3D::_build_brush() {
	// set our bounding box

	float min_radius = inner_radius;
	float max_radius = outer_radius;

	if (min_radius == max_radius) {
		return memnew(CSGBrush); //sorry, can't
	}

	if (min_radius > max_radius) {
		SWAP(min_radius, max_radius);
	}

	float radius = (max_radius - min_radius) * 0.5;

	CSGBrush *new_brush = memnew(CSGBrush);

	int face_count = ring_sides * sides * 2;

	bool invert_val = get_flip_faces();
	Ref<Material> base_material = get_material();

	Vector<Vector3> faces;
	Vector<Vector2> uvs;
	Vector<bool> smooth;
	Vector<Ref<Material>> materials;
	Vector<bool> invert;

	faces.resize(face_count * 3);
	uvs.resize(face_count * 3);

	smooth.resize(face_count);
	materials.resize(face_count);
	invert.resize(face_count);

	{
		Vector3 *facesw = faces.ptrw();
		Vector2 *uvsw = uvs.ptrw();
		bool *smoothw = smooth.ptrw();
		Ref<Material> *materialsw = materials.ptrw();
		bool *invertw = invert.ptrw();

		int face = 0;

		{
			for (int i = 0; i < sides; i++) {
				float inci = float(i) / sides;
				float inci_n = float((i + 1)) / sides;
				if (i == sides - 1) {
					inci_n = 0;
				}

				float angi = inci * Math::TAU;
				float angi_n = inci_n * Math::TAU;

				Vector3 normali = Vector3(Math::cos(angi), 0, Math::sin(angi));
				Vector3 normali_n = Vector3(Math::cos(angi_n), 0, Math::sin(angi_n));

				for (int j = 0; j < ring_sides; j++) {
					float incj = float(j) / ring_sides;
					float incj_n = float((j + 1)) / ring_sides;
					if (j == ring_sides - 1) {
						incj_n = 0;
					}

					float angj = incj * Math::TAU;
					float angj_n = incj_n * Math::TAU;

					Vector2 normalj = Vector2(Math::cos(angj), Math::sin(angj)) * radius + Vector2(min_radius + radius, 0);
					Vector2 normalj_n = Vector2(Math::cos(angj_n), Math::sin(angj_n)) * radius + Vector2(min_radius + radius, 0);

					Vector3 face_points[4] = {
						Vector3(normali.x * normalj.x, normalj.y, normali.z * normalj.x),
						Vector3(normali.x * normalj_n.x, normalj_n.y, normali.z * normalj_n.x),
						Vector3(normali_n.x * normalj_n.x, normalj_n.y, normali_n.z * normalj_n.x),
						Vector3(normali_n.x * normalj.x, normalj.y, normali_n.z * normalj.x)
					};

					Vector2 u[4] = {
						Vector2(inci, incj),
						Vector2(inci, incj_n),
						Vector2(inci_n, incj_n),
						Vector2(inci_n, incj),
					};

					// face 1
					facesw[face * 3 + 0] = face_points[0];
					facesw[face * 3 + 1] = face_points[2];
					facesw[face * 3 + 2] = face_points[1];

					uvsw[face * 3 + 0] = u[0];
					uvsw[face * 3 + 1] = u[2];
					uvsw[face * 3 + 2] = u[1];

					smoothw[face] = smooth_faces;
					invertw[face] = invert_val;
					materialsw[face] = base_material;

					face++;

					//face 2
					facesw[face * 3 + 0] = face_points[3];
					facesw[face * 3 + 1] = face_points[2];
					facesw[face * 3 + 2] = face_points[0];

					uvsw[face * 3 + 0] = u[3];
					uvsw[face * 3 + 1] = u[2];
					uvsw[face * 3 + 2] = u[0];

					smoothw[face] = smooth_faces;
					invertw[face] = invert_val;
					materialsw[face] = base_material;
					face++;
				}
			}
		}

		if (face != face_count) {
			ERR_PRINT("Face mismatch bug! fix code");
		}
	}

	new_brush->build_from_faces(faces, uvs, smooth, materials, invert);
	_tag_faces_single_surface(new_brush, SURFACE_BODY);

	return new_brush;
}

void CSGTorus3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_inner_radius", "radius"), &CSGTorus3D::set_inner_radius);
	ClassDB::bind_method(D_METHOD("get_inner_radius"), &CSGTorus3D::get_inner_radius);

	ClassDB::bind_method(D_METHOD("set_outer_radius", "radius"), &CSGTorus3D::set_outer_radius);
	ClassDB::bind_method(D_METHOD("get_outer_radius"), &CSGTorus3D::get_outer_radius);

	ClassDB::bind_method(D_METHOD("set_sides", "sides"), &CSGTorus3D::set_sides);
	ClassDB::bind_method(D_METHOD("get_sides"), &CSGTorus3D::get_sides);

	ClassDB::bind_method(D_METHOD("set_ring_sides", "sides"), &CSGTorus3D::set_ring_sides);
	ClassDB::bind_method(D_METHOD("get_ring_sides"), &CSGTorus3D::get_ring_sides);

	ClassDB::bind_method(D_METHOD("set_material", "material"), &CSGTorus3D::set_material);
	ClassDB::bind_method(D_METHOD("get_material"), &CSGTorus3D::get_material);

	ClassDB::bind_method(D_METHOD("set_smooth_faces", "smooth_faces"), &CSGTorus3D::set_smooth_faces);
	ClassDB::bind_method(D_METHOD("get_smooth_faces"), &CSGTorus3D::get_smooth_faces);

	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "inner_radius", PROPERTY_HINT_RANGE, "0.001,1000.0,0.001,or_greater,exp,suffix:m"), "set_inner_radius", "get_inner_radius");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "outer_radius", PROPERTY_HINT_RANGE, "0.001,1000.0,0.001,or_greater,exp,suffix:m"), "set_outer_radius", "get_outer_radius");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "sides", PROPERTY_HINT_RANGE, "3,64,1"), "set_sides", "get_sides");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "ring_sides", PROPERTY_HINT_RANGE, "3,64,1"), "set_ring_sides", "get_ring_sides");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "smooth_faces"), "set_smooth_faces", "get_smooth_faces");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "material", PROPERTY_HINT_RESOURCE_TYPE, "BaseMaterial3D,ShaderMaterial"), "set_material", "get_material");
}

void CSGTorus3D::set_inner_radius(const float p_inner_radius) {
	inner_radius = p_inner_radius;
	_make_dirty();
	update_gizmos();
}

float CSGTorus3D::get_inner_radius() const {
	return inner_radius;
}

void CSGTorus3D::set_outer_radius(const float p_outer_radius) {
	outer_radius = p_outer_radius;
	_make_dirty();
	update_gizmos();
}

float CSGTorus3D::get_outer_radius() const {
	return outer_radius;
}

void CSGTorus3D::set_sides(const int p_sides) {
	ERR_FAIL_COND(p_sides < 3);
	sides = p_sides;
	_make_dirty();
	update_gizmos();
}

int CSGTorus3D::get_sides() const {
	return sides;
}

void CSGTorus3D::set_ring_sides(const int p_ring_sides) {
	ERR_FAIL_COND(p_ring_sides < 3);
	ring_sides = p_ring_sides;
	_make_dirty();
	update_gizmos();
}

int CSGTorus3D::get_ring_sides() const {
	return ring_sides;
}

void CSGTorus3D::set_smooth_faces(const bool p_smooth_faces) {
	smooth_faces = p_smooth_faces;
	_make_dirty();
}

bool CSGTorus3D::get_smooth_faces() const {
	return smooth_faces;
}

void CSGTorus3D::set_material(const Ref<Material> &p_material) {
	material = p_material;
	_make_material_dirty();
}

Ref<Material> CSGTorus3D::get_material() const {
	return material;
}

CSGTorus3D::CSGTorus3D() {
	// defaults
	inner_radius = 0.5;
	outer_radius = 1.0;
	sides = 8;
	ring_sides = 6;
	smooth_faces = true;
}

///////////////

CSGBrush *CSGPolygon3D::_build_brush() {
	CSGBrush *new_brush = memnew(CSGBrush);

	if (polygon.size() < 3) {
		return new_brush;
	}

	// Triangulate polygon shape.
	Vector<Point2> shape_polygon = polygon;
	if (Triangulate::get_area(shape_polygon) > 0) {
		shape_polygon.reverse();
	}
	int shape_sides = shape_polygon.size();
	Vector<int> shape_faces = Geometry2D::triangulate_polygon(shape_polygon);
	ERR_FAIL_COND_V_MSG(shape_faces.size() < 3, new_brush, "Failed to triangulate CSGPolygon. Make sure the polygon doesn't have any intersecting edges.");

	// Get polygon enclosing Rect2.
	Rect2 shape_rect(shape_polygon[0], Vector2());
	for (int i = 1; i < shape_sides; i++) {
		shape_rect.expand_to(shape_polygon[i]);
	}

	// If MODE_PATH, check if curve has changed.
	Ref<Curve3D> curve;
	if (mode == MODE_PATH) {
		Path3D *current_path = Object::cast_to<Path3D>(get_node_or_null(path_node));
		if (path != current_path) {
			if (path) {
				path->disconnect(SceneStringName(tree_exited), callable_mp(this, &CSGPolygon3D::_path_exited));
				path->disconnect("curve_changed", callable_mp(this, &CSGPolygon3D::_path_changed));
				path->set_update_callback(Callable());
			}
			path = current_path;
			if (path) {
				path->connect(SceneStringName(tree_exited), callable_mp(this, &CSGPolygon3D::_path_exited));
				path->connect("curve_changed", callable_mp(this, &CSGPolygon3D::_path_changed));
				path->set_update_callback(callable_mp(this, &CSGPolygon3D::_path_changed));
			}
		}

		if (!path) {
			return new_brush;
		}

		curve = path->get_curve();
		if (curve.is_null() || curve->get_point_count() < 2) {
			return new_brush;
		}
	}

	// Calculate the number extrusions, ends and faces.
	int extrusions = 0;
	int extrusion_face_count = shape_sides * 2;
	int end_count = 0;
	int shape_face_count = shape_faces.size() / 3;
	real_t curve_length = 1.0;
	switch (mode) {
		case MODE_DEPTH:
			extrusions = 1;
			end_count = 2;
			break;
		case MODE_SPIN:
			extrusions = spin_sides;
			if (spin_degrees < 360) {
				end_count = 2;
			}
			break;
		case MODE_PATH: {
			curve_length = curve->get_baked_length();
			if (path_interval_type == PATH_INTERVAL_DISTANCE) {
				extrusions = MAX(1, Math::ceil(curve_length / path_interval)) + 1;
			} else {
				extrusions = Math::ceil(1.0 * curve->get_point_count() / path_interval);
			}
			if (!path_joined) {
				end_count = 2;
				extrusions -= 1;
			}
		} break;
	}
	int face_count = extrusions * extrusion_face_count + end_count * shape_face_count;

	// Initialize variables used to create the mesh.
	Ref<Material> base_material = get_material();

	Vector<Vector3> faces;
	Vector<Vector2> uvs;
	Vector<bool> smooth;
	Vector<Ref<Material>> materials;
	Vector<bool> invert;

	faces.resize(face_count * 3);
	uvs.resize(face_count * 3);
	smooth.resize(face_count);
	materials.resize(face_count);
	invert.resize(face_count);
	int faces_removed = 0;

	{
		Vector3 *facesw = faces.ptrw();
		Vector2 *uvsw = uvs.ptrw();
		bool *smoothw = smooth.ptrw();
		Ref<Material> *materialsw = materials.ptrw();
		bool *invertw = invert.ptrw();

		int face = 0;
		Transform3D base_xform;
		Transform3D current_xform;
		Transform3D previous_xform;
		Transform3D previous_previous_xform;
		double u_step = 1.0 / extrusions;
		if (path_u_distance > 0.0) {
			u_step *= curve_length / path_u_distance;
		}
		double v_step = 1.0 / shape_sides;
		double spin_step = Math::deg_to_rad(spin_degrees / spin_sides);
		double extrusion_step = 1.0 / extrusions;
		if (mode == MODE_PATH) {
			if (path_joined) {
				extrusion_step = 1.0 / (extrusions - 1);
			}
			extrusion_step *= curve_length;
		}

		if (mode == MODE_PATH) {
			if (!path_local && path->is_inside_tree()) {
				base_xform = path->get_global_transform();
			}

			Vector3 current_point;
			Vector3 current_up = Vector3(0, 1, 0);
			Vector3 direction;

			switch (path_rotation) {
				case PATH_ROTATION_POLYGON:
					current_point = curve->sample_baked(0);
					direction = Vector3(0, 0, -1);
					break;
				case PATH_ROTATION_PATH:
				case PATH_ROTATION_PATH_FOLLOW:
					if (!path_rotation_accurate) {
						current_point = curve->sample_baked(0);
						Vector3 next_point = curve->sample_baked(extrusion_step);
						direction = next_point - current_point;

						if (path_joined) {
							Vector3 last_point = curve->sample_baked(curve->get_baked_length());
							direction = next_point - last_point;
						}
					} else {
						Transform3D current_sample_xform = curve->sample_baked_with_rotation(0);
						current_point = current_sample_xform.get_origin();
						direction = current_sample_xform.get_basis().xform(Vector3(0, 0, -1));
					}

					if (path_rotation == PATH_ROTATION_PATH_FOLLOW) {
						current_up = curve->sample_baked_up_vector(0, true);
					}
					break;
			}

			Transform3D facing = Transform3D().looking_at(direction, current_up);
			current_xform = base_xform.translated_local(current_point) * facing;
		}

		// Create the mesh.
		if (end_count > 0) {
			// Add front end face.
			for (int face_idx = 0; face_idx < shape_face_count; face_idx++) {
				for (int face_vertex_idx = 0; face_vertex_idx < 3; face_vertex_idx++) {
					// We need to reverse the rotation of the shape face vertices.
					int index = shape_faces[face_idx * 3 + 2 - face_vertex_idx];
					Point2 p = shape_polygon[index];
					Point2 uv = (p - shape_rect.position) / shape_rect.size;

					// Use the left side of the bottom half of the y-inverted texture.
					uv.x = uv.x / 2;
					uv.y = 1 - (uv.y / 2);

					facesw[face * 3 + face_vertex_idx] = current_xform.xform(Vector3(p.x, p.y, 0));
					uvsw[face * 3 + face_vertex_idx] = uv;
				}

				smoothw[face] = false;
				materialsw[face] = base_material;
				invertw[face] = flip_faces;
				face++;
			}
		}

		real_t angle_simplify_dot = Math::cos(Math::deg_to_rad(path_simplify_angle));
		Vector3 previous_simplify_dir = Vector3(0, 0, 0);
		int faces_combined = 0;

		// Add extrusion faces.
		for (int x0 = 0; x0 < extrusions; x0++) {
			previous_previous_xform = previous_xform;
			previous_xform = current_xform;

			switch (mode) {
				case MODE_DEPTH: {
					current_xform.translate_local(Vector3(0, 0, -depth));
				} break;
				case MODE_SPIN: {
					if (end_count == 0 && x0 == extrusions - 1) {
						current_xform = base_xform;
					} else {
						current_xform.rotate(Vector3(0, 1, 0), spin_step);
					}
				} break;
				case MODE_PATH: {
					double previous_offset = x0 * extrusion_step;
					double current_offset = (x0 + 1) * extrusion_step;
					if (path_joined && x0 == extrusions - 1) {
						current_offset = 0;
					}

					Vector3 previous_point = curve->sample_baked(previous_offset);
					Transform3D current_sample_xform = curve->sample_baked_with_rotation(current_offset);
					Vector3 current_point = current_sample_xform.get_origin();
					Vector3 current_up = Vector3(0, 1, 0);
					Vector3 current_extrusion_dir = (current_point - previous_point).normalized();
					Vector3 direction;

					// If the angles are similar, remove the previous face and replace it with this one.
					if (path_simplify_angle > 0.0 && x0 > 0 && previous_simplify_dir.dot(current_extrusion_dir) > angle_simplify_dot) {
						faces_combined += 1;
						previous_xform = previous_previous_xform;
						face -= extrusion_face_count;
						faces_removed += extrusion_face_count;
					} else {
						faces_combined = 0;
						previous_simplify_dir = current_extrusion_dir;
					}

					switch (path_rotation) {
						case PATH_ROTATION_POLYGON:
							direction = Vector3(0, 0, -1);
							break;
						case PATH_ROTATION_PATH:
						case PATH_ROTATION_PATH_FOLLOW:
							if (!path_rotation_accurate) {
								double next_offset = (x0 + 2) * extrusion_step;
								if (x0 == extrusions - 1) {
									next_offset = path_joined ? extrusion_step : current_offset;
								}
								Vector3 next_point = curve->sample_baked(next_offset);
								direction = next_point - previous_point;
							} else {
								direction = current_sample_xform.get_basis().xform(Vector3(0, 0, -1));
							}

							if (path_rotation == PATH_ROTATION_PATH_FOLLOW) {
								current_up = curve->sample_baked_up_vector(current_offset, true);
							}
							break;
					}

					Transform3D facing = Transform3D().looking_at(direction, current_up);
					current_xform = base_xform.translated_local(current_point) * facing;
				} break;
			}

			double u0 = (x0 - faces_combined) * u_step;
			double u1 = ((x0 + 1) * u_step);
			if (mode == MODE_PATH && !path_continuous_u) {
				u0 = 0.0;
				u1 = 1.0;
			}

			for (int y0 = 0; y0 < shape_sides; y0++) {
				int y1 = (y0 + 1) % shape_sides;
				// Use the top half of the texture.
				double v0 = (y0 * v_step) / 2;
				double v1 = ((y0 + 1) * v_step) / 2;

				Vector3 v[4] = {
					previous_xform.xform(Vector3(shape_polygon[y0].x, shape_polygon[y0].y, 0)),
					current_xform.xform(Vector3(shape_polygon[y0].x, shape_polygon[y0].y, 0)),
					current_xform.xform(Vector3(shape_polygon[y1].x, shape_polygon[y1].y, 0)),
					previous_xform.xform(Vector3(shape_polygon[y1].x, shape_polygon[y1].y, 0)),
				};

				Vector2 u[4] = {
					Vector2(u0, v0),
					Vector2(u1, v0),
					Vector2(u1, v1),
					Vector2(u0, v1),
				};

				// Face 1
				facesw[face * 3 + 0] = v[0];
				facesw[face * 3 + 1] = v[1];
				facesw[face * 3 + 2] = v[2];

				uvsw[face * 3 + 0] = u[0];
				uvsw[face * 3 + 1] = u[1];
				uvsw[face * 3 + 2] = u[2];

				smoothw[face] = smooth_faces;
				invertw[face] = flip_faces;
				materialsw[face] = base_material;

				face++;

				// Face 2
				facesw[face * 3 + 0] = v[2];
				facesw[face * 3 + 1] = v[3];
				facesw[face * 3 + 2] = v[0];

				uvsw[face * 3 + 0] = u[2];
				uvsw[face * 3 + 1] = u[3];
				uvsw[face * 3 + 2] = u[0];

				smoothw[face] = smooth_faces;
				invertw[face] = flip_faces;
				materialsw[face] = base_material;

				face++;
			}
		}

		if (end_count > 1) {
			// Add back end face.
			for (int face_idx = 0; face_idx < shape_face_count; face_idx++) {
				for (int face_vertex_idx = 0; face_vertex_idx < 3; face_vertex_idx++) {
					int index = shape_faces[face_idx * 3 + face_vertex_idx];
					Point2 p = shape_polygon[index];
					Point2 uv = (p - shape_rect.position) / shape_rect.size;

					// Use the x-inverted ride side of the bottom half of the y-inverted texture.
					uv.x = 1 - uv.x / 2;
					uv.y = 1 - (uv.y / 2);

					facesw[face * 3 + face_vertex_idx] = current_xform.xform(Vector3(p.x, p.y, 0));
					uvsw[face * 3 + face_vertex_idx] = uv;
				}

				smoothw[face] = false;
				materialsw[face] = base_material;
				invertw[face] = flip_faces;
				face++;
			}
		}

		face_count -= faces_removed;
		ERR_FAIL_COND_V_MSG(face != face_count, new_brush, "Bug: Failed to create the CSGPolygon mesh correctly.");
	}

	if (faces_removed > 0) {
		faces.resize(face_count * 3);
		uvs.resize(face_count * 3);
		smooth.resize(face_count);
		materials.resize(face_count);
		invert.resize(face_count);
	}

	new_brush->build_from_faces(faces, uvs, smooth, materials, invert);
	const int front_face_count = end_count > 0 ? shape_face_count : 0;
	const int back_face_count = end_count > 1 ? shape_face_count : 0;
	const int side_face_end = new_brush->faces.size() - back_face_count;
	for (int face_i = 0; face_i < front_face_count; face_i++) {
		new_brush->faces.write[face_i].semantic_surface = SURFACE_FRONT;
		new_brush->faces.write[face_i].face_id = 0;
	}
	for (int face_i = front_face_count; face_i < side_face_end; face_i++) {
		new_brush->faces.write[face_i].semantic_surface = SURFACE_SIDE;
		// Path frames can twist, so their two triangles are not guaranteed to
		// form one planar quad. Depth/spin side pairs are planar facets.
		new_brush->faces.write[face_i].face_id = mode == MODE_PATH ? face_i - front_face_count : (face_i - front_face_count) / 2;
	}
	for (int face_i = side_face_end; face_i < new_brush->faces.size(); face_i++) {
		new_brush->faces.write[face_i].semantic_surface = SURFACE_BACK;
		new_brush->faces.write[face_i].face_id = 0;
	}

	return new_brush;
}

void CSGPolygon3D::_notification(int p_what) {
	if (p_what == NOTIFICATION_EXIT_TREE) {
		if (path) {
			path->disconnect(SceneStringName(tree_exited), callable_mp(this, &CSGPolygon3D::_path_exited));
			path->disconnect("curve_changed", callable_mp(this, &CSGPolygon3D::_path_changed));
			path = nullptr;
		}
	}
}

void CSGPolygon3D::_validate_property(PropertyInfo &p_property) const {
	if (p_property.name.begins_with("spin") && mode != MODE_SPIN) {
		p_property.usage = PROPERTY_USAGE_NONE;
	}
	if (p_property.name.begins_with("path") && mode != MODE_PATH) {
		p_property.usage = PROPERTY_USAGE_NONE;
	}
	if (p_property.name == "depth" && mode != MODE_DEPTH) {
		p_property.usage = PROPERTY_USAGE_NONE;
	}
}

void CSGPolygon3D::_path_changed() {
	_make_dirty();
	update_gizmos();
}

void CSGPolygon3D::_path_exited() {
	path = nullptr;
}

void CSGPolygon3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_polygon", "polygon"), &CSGPolygon3D::set_polygon);
	ClassDB::bind_method(D_METHOD("get_polygon"), &CSGPolygon3D::get_polygon);

	ClassDB::bind_method(D_METHOD("set_mode", "mode"), &CSGPolygon3D::set_mode);
	ClassDB::bind_method(D_METHOD("get_mode"), &CSGPolygon3D::get_mode);

	ClassDB::bind_method(D_METHOD("set_depth", "depth"), &CSGPolygon3D::set_depth);
	ClassDB::bind_method(D_METHOD("get_depth"), &CSGPolygon3D::get_depth);

	ClassDB::bind_method(D_METHOD("set_spin_degrees", "degrees"), &CSGPolygon3D::set_spin_degrees);
	ClassDB::bind_method(D_METHOD("get_spin_degrees"), &CSGPolygon3D::get_spin_degrees);

	ClassDB::bind_method(D_METHOD("set_spin_sides", "spin_sides"), &CSGPolygon3D::set_spin_sides);
	ClassDB::bind_method(D_METHOD("get_spin_sides"), &CSGPolygon3D::get_spin_sides);

	ClassDB::bind_method(D_METHOD("set_path_node", "path"), &CSGPolygon3D::set_path_node);
	ClassDB::bind_method(D_METHOD("get_path_node"), &CSGPolygon3D::get_path_node);

	ClassDB::bind_method(D_METHOD("set_path_interval_type", "interval_type"), &CSGPolygon3D::set_path_interval_type);
	ClassDB::bind_method(D_METHOD("get_path_interval_type"), &CSGPolygon3D::get_path_interval_type);

	ClassDB::bind_method(D_METHOD("set_path_interval", "interval"), &CSGPolygon3D::set_path_interval);
	ClassDB::bind_method(D_METHOD("get_path_interval"), &CSGPolygon3D::get_path_interval);

	ClassDB::bind_method(D_METHOD("set_path_simplify_angle", "degrees"), &CSGPolygon3D::set_path_simplify_angle);
	ClassDB::bind_method(D_METHOD("get_path_simplify_angle"), &CSGPolygon3D::get_path_simplify_angle);

	ClassDB::bind_method(D_METHOD("set_path_rotation", "path_rotation"), &CSGPolygon3D::set_path_rotation);
	ClassDB::bind_method(D_METHOD("get_path_rotation"), &CSGPolygon3D::get_path_rotation);

	ClassDB::bind_method(D_METHOD("set_path_rotation_accurate", "enable"), &CSGPolygon3D::set_path_rotation_accurate);
	ClassDB::bind_method(D_METHOD("get_path_rotation_accurate"), &CSGPolygon3D::get_path_rotation_accurate);

	ClassDB::bind_method(D_METHOD("set_path_local", "enable"), &CSGPolygon3D::set_path_local);
	ClassDB::bind_method(D_METHOD("is_path_local"), &CSGPolygon3D::is_path_local);

	ClassDB::bind_method(D_METHOD("set_path_continuous_u", "enable"), &CSGPolygon3D::set_path_continuous_u);
	ClassDB::bind_method(D_METHOD("is_path_continuous_u"), &CSGPolygon3D::is_path_continuous_u);

	ClassDB::bind_method(D_METHOD("set_path_u_distance", "distance"), &CSGPolygon3D::set_path_u_distance);
	ClassDB::bind_method(D_METHOD("get_path_u_distance"), &CSGPolygon3D::get_path_u_distance);

	ClassDB::bind_method(D_METHOD("set_path_joined", "enable"), &CSGPolygon3D::set_path_joined);
	ClassDB::bind_method(D_METHOD("is_path_joined"), &CSGPolygon3D::is_path_joined);

	ClassDB::bind_method(D_METHOD("set_material", "material"), &CSGPolygon3D::set_material);
	ClassDB::bind_method(D_METHOD("get_material"), &CSGPolygon3D::get_material);

	ClassDB::bind_method(D_METHOD("set_smooth_faces", "smooth_faces"), &CSGPolygon3D::set_smooth_faces);
	ClassDB::bind_method(D_METHOD("get_smooth_faces"), &CSGPolygon3D::get_smooth_faces);

	ClassDB::bind_method(D_METHOD("_is_editable_3d_polygon"), &CSGPolygon3D::_is_editable_3d_polygon);
	ClassDB::bind_method(D_METHOD("_has_editable_3d_polygon_no_depth"), &CSGPolygon3D::_has_editable_3d_polygon_no_depth);

	ADD_PROPERTY(PropertyInfo(Variant::PACKED_VECTOR2_ARRAY, "polygon"), "set_polygon", "get_polygon");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "mode", PROPERTY_HINT_ENUM, "Depth,Spin,Path"), "set_mode", "get_mode");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "depth", PROPERTY_HINT_RANGE, "0.01,100.0,0.01,or_greater,exp,suffix:m"), "set_depth", "get_depth");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "spin_degrees", PROPERTY_HINT_RANGE, "1,360,0.1"), "set_spin_degrees", "get_spin_degrees");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "spin_sides", PROPERTY_HINT_RANGE, "3,64,1"), "set_spin_sides", "get_spin_sides");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "path_node", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "Path3D"), "set_path_node", "get_path_node");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "path_interval_type", PROPERTY_HINT_ENUM, "Distance,Subdivide"), "set_path_interval_type", "get_path_interval_type");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "path_interval", PROPERTY_HINT_RANGE, "0.01,1.0,0.01,exp,or_greater"), "set_path_interval", "get_path_interval");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "path_simplify_angle", PROPERTY_HINT_RANGE, "0.0,180.0,0.1"), "set_path_simplify_angle", "get_path_simplify_angle");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "path_rotation", PROPERTY_HINT_ENUM, "Polygon,Path,PathFollow"), "set_path_rotation", "get_path_rotation");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "path_rotation_accurate"), "set_path_rotation_accurate", "get_path_rotation_accurate");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "path_local"), "set_path_local", "is_path_local");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "path_continuous_u"), "set_path_continuous_u", "is_path_continuous_u");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "path_u_distance", PROPERTY_HINT_RANGE, "0.0,10.0,0.01,or_greater,suffix:m"), "set_path_u_distance", "get_path_u_distance");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "path_joined"), "set_path_joined", "is_path_joined");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "smooth_faces"), "set_smooth_faces", "get_smooth_faces");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "material", PROPERTY_HINT_RESOURCE_TYPE, "BaseMaterial3D,ShaderMaterial"), "set_material", "get_material");

	BIND_ENUM_CONSTANT(MODE_DEPTH);
	BIND_ENUM_CONSTANT(MODE_SPIN);
	BIND_ENUM_CONSTANT(MODE_PATH);

	BIND_ENUM_CONSTANT(PATH_ROTATION_POLYGON);
	BIND_ENUM_CONSTANT(PATH_ROTATION_PATH);
	BIND_ENUM_CONSTANT(PATH_ROTATION_PATH_FOLLOW);

	BIND_ENUM_CONSTANT(PATH_INTERVAL_DISTANCE);
	BIND_ENUM_CONSTANT(PATH_INTERVAL_SUBDIVIDE);
}

void CSGPolygon3D::set_polygon(const Vector<Vector2> &p_polygon) {
	polygon = p_polygon;
	_make_dirty();
	update_gizmos();
}

Vector<Vector2> CSGPolygon3D::get_polygon() const {
	return polygon;
}

void CSGPolygon3D::set_mode(Mode p_mode) {
	mode = p_mode;
	_synchronize_surface_schema();
	_make_dirty();
	update_gizmos();
	notify_property_list_changed();
}

CSGPolygon3D::Mode CSGPolygon3D::get_mode() const {
	return mode;
}

void CSGPolygon3D::set_depth(const float p_depth) {
	ERR_FAIL_COND(p_depth < 0.001);
	depth = p_depth;
	_make_dirty();
	update_gizmos();
}

float CSGPolygon3D::get_depth() const {
	return depth;
}

void CSGPolygon3D::set_path_continuous_u(bool p_enable) {
	path_continuous_u = p_enable;
	_make_dirty();
}

bool CSGPolygon3D::is_path_continuous_u() const {
	return path_continuous_u;
}

void CSGPolygon3D::set_path_u_distance(real_t p_path_u_distance) {
	path_u_distance = p_path_u_distance;
	_make_dirty();
	update_gizmos();
}

real_t CSGPolygon3D::get_path_u_distance() const {
	return path_u_distance;
}

void CSGPolygon3D::set_spin_degrees(const float p_spin_degrees) {
	ERR_FAIL_COND(p_spin_degrees < 0.01 || p_spin_degrees > 360);
	spin_degrees = p_spin_degrees;
	_make_dirty();
	update_gizmos();
}

float CSGPolygon3D::get_spin_degrees() const {
	return spin_degrees;
}

void CSGPolygon3D::set_spin_sides(int p_spin_sides) {
	ERR_FAIL_COND(p_spin_sides < 3);
	spin_sides = p_spin_sides;
	_make_dirty();
	update_gizmos();
}

int CSGPolygon3D::get_spin_sides() const {
	return spin_sides;
}

void CSGPolygon3D::set_path_node(const NodePath &p_path) {
	path_node = p_path;
	_make_dirty();
	update_gizmos();
}

NodePath CSGPolygon3D::get_path_node() const {
	return path_node;
}

void CSGPolygon3D::set_path_interval_type(PathIntervalType p_interval_type) {
	path_interval_type = p_interval_type;
	_make_dirty();
	update_gizmos();
}

CSGPolygon3D::PathIntervalType CSGPolygon3D::get_path_interval_type() const {
	return path_interval_type;
}

void CSGPolygon3D::set_path_interval(float p_interval) {
	path_interval = p_interval;
	_make_dirty();
	update_gizmos();
}

float CSGPolygon3D::get_path_interval() const {
	return path_interval;
}

void CSGPolygon3D::set_path_simplify_angle(float p_angle) {
	path_simplify_angle = p_angle;
	_make_dirty();
	update_gizmos();
}

float CSGPolygon3D::get_path_simplify_angle() const {
	return path_simplify_angle;
}

void CSGPolygon3D::set_path_rotation(PathRotation p_rotation) {
	path_rotation = p_rotation;
	_make_dirty();
	update_gizmos();
}

CSGPolygon3D::PathRotation CSGPolygon3D::get_path_rotation() const {
	return path_rotation;
}

void CSGPolygon3D::set_path_rotation_accurate(bool p_enabled) {
	path_rotation_accurate = p_enabled;
	_make_dirty();
	update_gizmos();
}

bool CSGPolygon3D::get_path_rotation_accurate() const {
	return path_rotation_accurate;
}

void CSGPolygon3D::set_path_local(bool p_enable) {
	path_local = p_enable;
	_make_dirty();
	update_gizmos();
}

bool CSGPolygon3D::is_path_local() const {
	return path_local;
}

void CSGPolygon3D::set_path_joined(bool p_enable) {
	path_joined = p_enable;
	_make_dirty();
	update_gizmos();
}

bool CSGPolygon3D::is_path_joined() const {
	return path_joined;
}

void CSGPolygon3D::set_smooth_faces(const bool p_smooth_faces) {
	smooth_faces = p_smooth_faces;
	_make_dirty();
}

bool CSGPolygon3D::get_smooth_faces() const {
	return smooth_faces;
}

void CSGPolygon3D::set_material(const Ref<Material> &p_material) {
	material = p_material;
	_make_material_dirty();
}

Ref<Material> CSGPolygon3D::get_material() const {
	return material;
}

bool CSGPolygon3D::_is_editable_3d_polygon() const {
	return true;
}

bool CSGPolygon3D::_has_editable_3d_polygon_no_depth() const {
	return true;
}

CSGPolygon3D::CSGPolygon3D() {
	// defaults
	mode = MODE_DEPTH;
	polygon.push_back(Vector2(0, 0));
	polygon.push_back(Vector2(0, 1));
	polygon.push_back(Vector2(1, 1));
	polygon.push_back(Vector2(1, 0));
	depth = 1.0;
	spin_degrees = 360;
	spin_sides = 8;
	smooth_faces = false;
	path_interval_type = PATH_INTERVAL_DISTANCE;
	path_interval = 1.0;
	path_simplify_angle = 0.0;
	path_rotation = PATH_ROTATION_PATH_FOLLOW;
	path_rotation_accurate = false;
	path_local = false;
	path_continuous_u = true;
	path_u_distance = 1.0;
	path_joined = false;
	path = nullptr;
}
