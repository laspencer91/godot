/**************************************************************************/
/*  csg_evaluation.h                                                      */
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

#include "csg.h"
#include "csg_shape.h"

#include "core/templates/hash_map.h"
#include "scene/resources/material.h"

#include <manifold/manifold.h>

#include <utility>

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

struct CSGManifoldSurfaceRecord;

struct CSGManifoldResultTriangle {
	CSGOriginToken origin_token = 0;
	uint32_t face_id = 0;
};

struct CSGRenderSurface {
	Vector<Vector3> vertices;
	Vector<Vector3> normals;
	Vector<Vector2> uvs;
	Vector<float> tans;
	Ref<Material> material;
	int last_added = 0;

	// Build-scratch aliases only; csg_build_render_surfaces() clears them
	// before the surface can be copied or moved in a snapshot.
	Vector3 *verticesw = nullptr;
	Vector3 *normalsw = nullptr;
	Vector2 *uvsw = nullptr;
	float *tansw = nullptr;
};

struct CSGEvaluationSettings {
	bool autosmooth = false;
	float smoothing_angle = 50.0f;
	bool calculate_tangents = true;
	bool want_collision = false;
	bool want_render = false;
};

struct CSGSurfaceUVResolved {
	bool planar = false;
	Vector3 origin;
	Vector3 axis_u;
	Vector3 axis_v;
	Vector2 offset;
};

struct CSGEvaluationInputs {
	// GetMeshGL64()/IsEmpty()/BoundingBox() collapse the receiving handle into
	// an evaluated leaf. This value is a copy so the node's cached operation
	// handle remains untouched.
	manifold::Manifold subtree;

	HashMap<CSGOriginToken, Ref<Material>> mesh_materials;
	HashMap<CSGOriginToken, CSGSurfaceUVResolved> mesh_uv_settings;
	HashMap<CSGOriginToken, CSGSurfaceKey> surface_keys;

	CSGEvaluationSettings settings;

	ObjectID root_id;
	uint32_t schema_generation = 0;
	uint64_t request_generation = 0;
	bool want_result_metadata = false;
};

struct CSGEvaluationSnapshot {
	// Owned until publication transfers it to the node.
	CSGBrush *brush = nullptr;
	HashMap<CSGOriginToken, CSGSurfaceKey> result_surface_keys;
	Vector<CSGManifoldResultTriangle> result_triangles;
	AABB node_aabb;
	bool subtree_empty = true;

	Vector<CSGRenderSurface> render_surfaces;
	Vector<Vector3> collision_faces;
	bool built_render = false;
	bool built_tangents = false;
	bool collision_built = false;

	ObjectID root_id;
	uint32_t schema_generation = 0;
	uint64_t request_generation = 0;

	CSGEvaluationSnapshot() = default;
	CSGEvaluationSnapshot(const CSGEvaluationSnapshot &) = delete;
	CSGEvaluationSnapshot &operator=(const CSGEvaluationSnapshot &) = delete;

	CSGEvaluationSnapshot(CSGEvaluationSnapshot &&p_other) noexcept {
		*this = std::move(p_other);
	}

	CSGEvaluationSnapshot &operator=(CSGEvaluationSnapshot &&p_other) noexcept {
		if (this == &p_other) {
			return *this;
		}
		if (brush) {
			memdelete(brush);
		}
		brush = p_other.brush;
		p_other.brush = nullptr;
		result_surface_keys = std::move(p_other.result_surface_keys);
		result_triangles = std::move(p_other.result_triangles);
		node_aabb = p_other.node_aabb;
		subtree_empty = p_other.subtree_empty;
		render_surfaces = std::move(p_other.render_surfaces);
		collision_faces = std::move(p_other.collision_faces);
		built_render = p_other.built_render;
		built_tangents = p_other.built_tangents;
		collision_built = p_other.collision_built;
		root_id = p_other.root_id;
		schema_generation = p_other.schema_generation;
		request_generation = p_other.request_generation;
		return *this;
	}

	~CSGEvaluationSnapshot() {
		if (brush) {
			memdelete(brush);
		}
	}
};

manifold::Manifold csg_combine_manifolds(const std::vector<manifold::Manifold> &p_manifolds, manifold::OpType p_operation);
manifold::OpType csg_convert_operation(CSGShape3D::Operation p_operation);
manifold::mat3x4 csg_to_manifold_transform(const Transform3D &p_transform);
void csg_pack_manifold(const CSGBrush *p_mesh_merge, manifold::Manifold &r_manifold, CSGOriginToken p_origin_base, uint32_t p_schema_size, ObjectID p_source_shape, uint32_t p_schema_generation, Vector<CSGManifoldSurfaceRecord> &r_surface_records);
void csg_materialize_brush(const manifold::Manifold &p_manifold, const HashMap<CSGOriginToken, Ref<Material>> &p_mesh_materials, const HashMap<CSGOriginToken, CSGSurfaceUVResolved> &p_mesh_uv_settings, CSGBrush *r_mesh_merge, Vector<CSGManifoldResultTriangle> *r_result_triangles);
void csg_build_render_surfaces(const CSGBrush *p_brush, const CSGEvaluationSettings &p_settings, Vector<CSGRenderSurface> &r_surfaces, bool &r_built_tangents);
void csg_extract_collision_faces(const CSGBrush *p_brush, Vector<Vector3> &r_collision_faces);
CSGEvaluationSnapshot csg_build_snapshot(const CSGEvaluationInputs &p_inputs);
