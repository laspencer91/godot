/**************************************************************************/
/*  csg_evaluation.cpp                                                    */
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

#include "csg_evaluation.h"

#include "csg_manifold_cache.h"

#include "core/templates/a_hash_map.h"
#include "core/templates/local_vector.h"
#include "scene/resources/surface_tool.h"

#ifdef DEV_ENABLED
#include "csg_debug_counters.h"

#include "core/io/json.h"
#endif // DEV_ENABLED

#include <cfloat> // FLT_EPSILON

void csg_materialize_brush(
		const manifold::Manifold &p_manifold,
		const HashMap<CSGOriginToken, Ref<Material>> &p_mesh_materials,
		const HashMap<CSGOriginToken, CSGSurfaceUVResolved> &p_mesh_uv_settings,
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
		const CSGSurfaceUVResolved *resolved_uv = p_mesh_uv_settings.getptr(original_id);
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
				if (resolved_uv && resolved_uv->planar) {
					// Materialization is the only stage that still has the Manifold
					// run-to-origin mapping. The frame was fully resolved on the main
					// thread, so applying it here needs no node or Resource access.
					const Vector3 relative_position = face.vertices[tri_order_i] - resolved_uv->origin;
					face.uvs[tri_order_i] = Vector2(
							resolved_uv->axis_u.dot(relative_position) + resolved_uv->offset.x,
							resolved_uv->axis_v.dot(relative_position) + resolved_uv->offset.y);
				} else {
					face.uvs[tri_order_i] = Vector2(
							mesh.vertProperties[property_i * mesh.numProp + MANIFOLD_PROPERTY_UV_X_0],
							mesh.vertProperties[property_i * mesh.numProp + MANIFOLD_PROPERTY_UV_Y_0]);
				}
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

void csg_pack_manifold(
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

manifold::OpType csg_convert_operation(CSGShape3D::Operation p_operation) {
	switch (p_operation) {
		case CSGShape3D::OPERATION_SUBTRACTION:
			return manifold::OpType::Subtract;
		case CSGShape3D::OPERATION_INTERSECTION:
			return manifold::OpType::Intersect;
		default:
			return manifold::OpType::Add;
	}
}

manifold::mat3x4 csg_to_manifold_transform(const Transform3D &p_transform) {
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

manifold::Manifold csg_combine_manifolds(const std::vector<manifold::Manifold> &p_manifolds, manifold::OpType p_operation) {
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

static void _build_surfaces_smoothed(const CSGBrush *p_brush, const CSGEvaluationSettings &p_settings, Vector<CSGRenderSurface> &r_surfaces, Vector<int> &r_face_count) {
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
	if (p_settings.smoothing_angle > 0.1) {
		float smooth_angle_rad = Math::cos(Math::deg_to_rad(p_settings.smoothing_angle));
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
		if (p_settings.calculate_tangents) {
			r_surfaces.write[i].tans.resize(r_face_count[i] * 3 * 4);
		}
		r_surfaces.write[i].last_added = 0;

		if (i != r_surfaces.size() - 1) {
			r_surfaces.write[i].material = p_brush->materials[i];
		}

		r_surfaces.write[i].verticesw = r_surfaces.write[i].vertices.ptrw();
		r_surfaces.write[i].normalsw = r_surfaces.write[i].normals.ptrw();
		r_surfaces.write[i].uvsw = r_surfaces.write[i].uvs.ptrw();
		if (p_settings.calculate_tangents) {
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

				if (p_settings.calculate_tangents) {
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

static void _build_surfaces_default(const CSGBrush *p_brush, const CSGEvaluationSettings &p_settings, Vector<CSGRenderSurface> &r_surfaces, Vector<int> &r_face_count) {
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
		if (p_settings.calculate_tangents) {
			r_surfaces.write[i].tans.resize(r_face_count[i] * 3 * 4);
		}
		r_surfaces.write[i].last_added = 0;

		if (i != r_surfaces.size() - 1) {
			r_surfaces.write[i].material = p_brush->materials[i];
		}

		r_surfaces.write[i].verticesw = r_surfaces.write[i].vertices.ptrw();
		r_surfaces.write[i].normalsw = r_surfaces.write[i].normals.ptrw();
		r_surfaces.write[i].uvsw = r_surfaces.write[i].uvs.ptrw();
		if (p_settings.calculate_tangents) {
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

				if (p_settings.calculate_tangents) {
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

void csg_build_render_surfaces(const CSGBrush *p_brush, const CSGEvaluationSettings &p_settings, Vector<CSGRenderSurface> &r_surfaces, bool &r_built_tangents) {
	r_surfaces.clear();
	r_built_tangents = false;
	ERR_FAIL_NULL_MSG(p_brush, "Cannot build CSG render surfaces without a brush.");

	Vector<int> face_count;
	face_count.resize(p_brush->materials.size() + 1);
	face_count.fill(0);

	r_surfaces.resize(face_count.size());
	if (p_settings.autosmooth) {
		_build_surfaces_smoothed(p_brush, p_settings, r_surfaces, face_count);
	} else {
		_build_surfaces_default(p_brush, p_settings, r_surfaces, face_count);
	}

	r_built_tangents = p_settings.calculate_tangents && SurfaceTool::generate_tangents_func;
	if (r_built_tangents) {
		for (int i = 0; i < r_surfaces.size(); i++) {
			CSGRenderSurface &surface = r_surfaces.write[i];
			_generate_tangents_unindexed(surface.tansw, surface.vertices.size(), surface.verticesw, surface.normalsw, surface.uvsw);
		}
	}

	// Write pointers are build scratch only and must not survive a surface copy.
	for (int i = 0; i < r_surfaces.size(); i++) {
		CSGRenderSurface &surface = r_surfaces.write[i];
		surface.verticesw = nullptr;
		surface.normalsw = nullptr;
		surface.uvsw = nullptr;
		surface.tansw = nullptr;
	}
}

void csg_extract_collision_faces(const CSGBrush *p_brush, Vector<Vector3> &r_collision_faces) {
	r_collision_faces.clear();
	ERR_FAIL_NULL_MSG(p_brush, "Cannot extract CSG collision faces without a brush.");

	r_collision_faces.resize(p_brush->faces.size() * 3);
	Vector3 *collision_faces_ptrw = r_collision_faces.ptrw();
	for (int i = 0; i < p_brush->faces.size(); i++) {
		int order[3] = { 0, 1, 2 };

		if (p_brush->faces[i].invert) {
			SWAP(order[1], order[2]);
		}

		collision_faces_ptrw[i * 3 + 0] = p_brush->faces[i].vertices[order[0]];
		collision_faces_ptrw[i * 3 + 1] = p_brush->faces[i].vertices[order[1]];
		collision_faces_ptrw[i * 3 + 2] = p_brush->faces[i].vertices[order[2]];
	}
}

CSGEvaluationSnapshot csg_build_snapshot(const CSGEvaluationInputs &p_inputs) {
	CSGEvaluationSnapshot snapshot;
	snapshot.root_id = p_inputs.root_id;
	snapshot.schema_generation = p_inputs.schema_generation;
	snapshot.request_generation = p_inputs.request_generation;
	snapshot.brush = memnew(CSGBrush);

	Vector<CSGManifoldResultTriangle> *result_triangles = p_inputs.want_result_metadata ? &snapshot.result_triangles : nullptr;
	csg_materialize_brush(p_inputs.subtree, p_inputs.mesh_materials, p_inputs.mesh_uv_settings, snapshot.brush, result_triangles);
	if (p_inputs.want_result_metadata) {
		snapshot.result_surface_keys = p_inputs.surface_keys;
	}

	if (!snapshot.brush->faces.is_empty()) {
		snapshot.node_aabb.position = snapshot.brush->faces[0].vertices[0];
		for (const CSGBrush::Face &face : snapshot.brush->faces) {
			for (int i = 0; i < 3; i++) {
				snapshot.node_aabb.expand_to(face.vertices[i]);
			}
		}
	}
	snapshot.subtree_empty = snapshot.brush->faces.is_empty();

	if (p_inputs.settings.want_render) {
		csg_build_render_surfaces(snapshot.brush, p_inputs.settings, snapshot.render_surfaces, snapshot.built_tangents);
		snapshot.built_render = true;
	}
	if (p_inputs.settings.want_collision) {
		csg_extract_collision_faces(snapshot.brush, snapshot.collision_faces);
	}
	snapshot.collision_built = p_inputs.settings.want_collision;

	return snapshot;
}
