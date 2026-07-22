/**************************************************************************/
/*  box3d_shape_3d.cpp                                                    */
/**************************************************************************/

#include "box3d_shape_3d.h"

#include "box3d_body_3d.h"
#include "box3d_collision_object_3d.h"
#include "box3d_conversions.h"

#include "core/io/image.h"
#include "core/templates/local_vector.h"

static void _copy_material_indices(const PackedByteArray &p_source, LocalVector<uint8_t> &r_indices, int p_triangle_count) {
	if (p_source.size() != p_triangle_count) {
		return;
	}
	r_indices.resize(p_triangle_count);
	for (int i = 0; i < p_triangle_count; i++) {
		r_indices[i] = p_source[i];
	}
}

void Box3DShape3D::_clear_geometry() {
	if (hull) {
		b3DestroyHull(hull);
		hull = nullptr;
	}
	if (mesh) {
		b3DestroyMesh(mesh);
		mesh = nullptr;
	}
	box_built = false;
}

Box3DShape3D::~Box3DShape3D() {
	// Server detaches from all owner bodies before deleting a shape.
	_clear_geometry();
}

void Box3DShape3D::set_data(const Variant &p_data) {
	data = p_data;
	_clear_geometry();

	switch (type) {
		case PS3DE::SHAPE_SPHERE: {
			sphere_radius = p_data;
		} break;

		case PS3DE::SHAPE_BOX: {
			Vector3 half_extents = p_data;
			box_hull = b3MakeBoxHull((float)half_extents.x, (float)half_extents.y, (float)half_extents.z);
			box_built = true;
		} break;

		case PS3DE::SHAPE_CAPSULE: {
			Dictionary d = p_data;
			capsule_radius = d["radius"];
			capsule_height = d["height"];
		} break;

		case PS3DE::SHAPE_CYLINDER: {
			Dictionary d = p_data;
			float radius = d["radius"];
			float height = d["height"];
			// Box3D has no cylinder primitive; tessellated hull approximation (see llm/04 in the box3d repo).
			// b3CreateCylinder builds rings starting at yOffset, so center it on the origin.
			hull = b3CreateCylinder(height, radius, -0.5f * height, 24);
			ERR_FAIL_NULL_MSG(hull, "Box3D: failed to build cylinder hull.");
		} break;

		case PS3DE::SHAPE_CONVEX_POLYGON: {
			PackedVector3Array points = p_data;
			int count = points.size();
			ERR_FAIL_COND_MSG(count < 4, "Box3D: convex polygon shape needs at least 4 points.");
			LocalVector<b3Vec3> b3_points;
			b3_points.resize(count);
			for (int i = 0; i < count; i++) {
				b3_points[i] = to_box3d(points[i]);
			}
			// Box3D simplifies to maxVertexCount internally (uint8 half-edge indexing).
			hull = b3CreateHull(b3_points.ptr(), count, 64);
			ERR_FAIL_NULL_MSG(hull, "Box3D: failed to build convex hull (degenerate input).");
		} break;

		case PS3DE::SHAPE_CONCAVE_POLYGON: {
			Dictionary d = p_data;
			PackedVector3Array faces = d["faces"];
			const bool backface_collision = d.get("backface_collision", false);
			int vertex_count = faces.size();
			ERR_FAIL_COND_MSG(vertex_count == 0 || vertex_count % 3 != 0, "Box3D: concave shape needs a triangle soup (3 vertices per face).");
			const int source_triangle_count = vertex_count / 3;
			const int output_triangle_count = backface_collision ? source_triangle_count * 2 : source_triangle_count;
			LocalVector<b3Vec3> vertices;
			LocalVector<int32_t> indices;
			LocalVector<uint8_t> material_indices;
			vertices.resize(vertex_count);
			indices.resize(output_triangle_count * 3);
			for (int i = 0; i < vertex_count; i++) {
				vertices[i] = to_box3d(faces[i]);
			}
			int index = 0;
			for (int i = 0; i < vertex_count; i += 3) {
				// Godot-authored faces are clockwise-front; Box3D meshes use the opposite winding.
				indices[index++] = i;
				indices[index++] = i + 2;
				indices[index++] = i + 1;
				if (backface_collision) {
					indices[index++] = i;
					indices[index++] = i + 1;
					indices[index++] = i + 2;
				}
			}
			b3MeshDef mesh_def = {}; // Geometry defs are plain zero-init structs (no b3Default* factory).
			mesh_def.vertices = vertices.ptr();
			mesh_def.indices = indices.ptr();
			mesh_def.vertexCount = vertex_count;
			mesh_def.triangleCount = output_triangle_count;
			if (mesh_triangle_material_indices.size() == source_triangle_count) {
				material_indices.resize(output_triangle_count);
				for (int i = 0; i < source_triangle_count; i++) {
					material_indices[backface_collision ? i * 2 : i] = mesh_triangle_material_indices[i];
					if (backface_collision) {
						material_indices[i * 2 + 1] = mesh_triangle_material_indices[i];
					}
				}
			}
			mesh_def.materialIndices = material_indices.size() == mesh_def.triangleCount ? material_indices.ptr() : nullptr;
			mesh_def.weldVertices = true;
			mesh_def.weldTolerance = 0.0001f;
			mesh_def.identifyEdges = true; // Internal-edge (ghost collision) suppression.
			mesh = b3CreateMesh(&mesh_def, nullptr, 0);
			ERR_FAIL_NULL_MSG(mesh, "Box3D: failed to build collision mesh.");
		} break;

		case PS3DE::SHAPE_HEIGHTMAP: {
			Dictionary d = p_data;
			ERR_FAIL_COND_MSG(!d.has("width") || !d.has("depth") || !d.has("heights"), "Box3D: heightmap data must contain width, depth, and heights.");

			const int width = d["width"];
			const int depth = d["depth"];
			ERR_FAIL_COND_MSG(width < 2 || depth < 2, "Box3D: heightmap shape needs at least a 2x2 grid.");

			Variant heights_variant = d["heights"];
			Vector<real_t> heights;
#ifdef REAL_T_IS_DOUBLE
			if (heights_variant.get_type() == Variant::PACKED_FLOAT64_ARRAY) {
#else
			if (heights_variant.get_type() == Variant::PACKED_FLOAT32_ARRAY) {
#endif
				heights = heights_variant;
			} else if (heights_variant.get_type() == Variant::OBJECT) {
				Ref<Image> image = heights_variant;
				ERR_FAIL_COND_MSG(image.is_null() || image->get_format() != Image::FORMAT_RF, "Box3D: heightmap Image data must use FORMAT_RF.");
				PackedByteArray image_data = image->get_data();
				heights.resize(image->get_width() * image->get_height());
				real_t *height_write = heights.ptrw();
				const float *image_read = (const float *)image_data.ptr();
				for (int i = 0; i < heights.size(); i++) {
					height_write[i] = image_read[i];
				}
			} else {
				ERR_FAIL_MSG("Box3D: heightmap heights must be a packed float array or FORMAT_RF image.");
			}

			ERR_FAIL_COND_MSG(heights.size() != width * depth, "Box3D: heightmap heights size must equal width * depth.");

			LocalVector<b3Vec3> vertices;
			LocalVector<int32_t> indices;
			LocalVector<uint8_t> material_indices;
			vertices.resize(width * depth);
			const int source_triangle_count = (width - 1) * (depth - 1) * 2;
			const int output_triangle_count = source_triangle_count * 2;
			indices.resize(output_triangle_count * 3);

			const real_t x_offset = (real_t)(width - 1) * 0.5;
			const real_t z_offset = (real_t)(depth - 1) * 0.5;
			for (int z = 0; z < depth; z++) {
				for (int x = 0; x < width; x++) {
					const int idx = z * width + x;
					vertices[idx] = to_box3d(Vector3((real_t)x - x_offset, heights[idx], (real_t)z - z_offset));
				}
			}

			int index = 0;
			for (int z = 0; z < depth - 1; z++) {
				for (int x = 0; x < width - 1; x++) {
					const int i00 = z * width + x;
					const int i10 = z * width + x + 1;
					const int i01 = (z + 1) * width + x;
					const int i11 = (z + 1) * width + x + 1;

					// Heightmaps are currently represented by Box3D meshes. Keep both
					// windings so ray queries can honor Godot's per-query back-face flag;
					// the direct-space callback filters the reverse winding when disabled.
					indices[index++] = i00;
					indices[index++] = i01;
					indices[index++] = i10;
					indices[index++] = i00;
					indices[index++] = i10;
					indices[index++] = i01;
					indices[index++] = i10;
					indices[index++] = i01;
					indices[index++] = i11;
					indices[index++] = i10;
					indices[index++] = i11;
					indices[index++] = i01;
				}
			}

			b3MeshDef mesh_def = {};
			mesh_def.vertices = vertices.ptr();
			mesh_def.indices = indices.ptr();
			mesh_def.vertexCount = vertices.size();
			mesh_def.triangleCount = output_triangle_count;
			if (mesh_triangle_material_indices.size() == source_triangle_count) {
				material_indices.resize(output_triangle_count);
				for (int i = 0; i < source_triangle_count; i++) {
					material_indices[i * 2] = mesh_triangle_material_indices[i];
					material_indices[i * 2 + 1] = mesh_triangle_material_indices[i];
				}
			}
			mesh_def.materialIndices = material_indices.size() == mesh_def.triangleCount ? material_indices.ptr() : nullptr;
			mesh_def.weldVertices = true;
			mesh_def.weldTolerance = 0.0001f;
			mesh_def.useMedianSplit = true;
			mesh_def.identifyEdges = true;
			mesh = b3CreateMesh(&mesh_def, nullptr, 0);
			ERR_FAIL_NULL_MSG(mesh, "Box3D: failed to build heightmap mesh.");
		} break;

		case PS3DE::SHAPE_WORLD_BOUNDARY: {
			WARN_PRINT_ONCE("Box3D: world boundary shapes not implemented yet; shape will not collide.");
		} break;

		case PS3DE::SHAPE_SEPARATION_RAY: {
			WARN_PRINT_ONCE("Box3D: separation ray shapes not implemented yet; shape will not collide.");
		} break;

		default: {
			ERR_FAIL_MSG(vformat("Box3D: unsupported shape type %d.", (int)type));
		} break;
	}

	// Rebuild every body this shape is attached to.
	for (Box3DCollisionObject3D *object : owners) {
		object->shapes_changed();
	}
}

void Box3DShape3D::set_surface_material(int p_material_id) {
	has_surface_material = p_material_id > 0;
	surface_material_id = MAX(0, p_material_id);
	for (Box3DCollisionObject3D *object : owners) {
		object->shapes_changed();
	}
}

void Box3DShape3D::set_surface_map(const PackedInt64Array &p_material_ids, const PackedByteArray &p_triangle_indices) {
	mesh_material_ids = p_material_ids;
	mesh_triangle_material_indices = p_triangle_indices;
	if (data.get_type() == Variant::NIL) {
		return;
	}
	set_data(data);
}

int Box3DShape3D::get_face_material_id(int p_face_index) const {
	if (mesh == nullptr) {
		return has_surface_material ? surface_material_id : 0;
	}
	if (p_face_index < 0 || p_face_index >= mesh->triangleCount || mesh_material_ids.is_empty()) {
		return has_surface_material ? surface_material_id : 0;
	}
	const uint8_t *built_indices = b3GetMeshMaterialIndices(mesh);
	if (built_indices == nullptr) {
		return has_surface_material ? surface_material_id : 0;
	}
	const int material_index = CLAMP((int)built_indices[p_face_index], 0, mesh_material_ids.size() - 1);
	return MAX(0, (int)mesh_material_ids[material_index]);
}

PackedByteArray Box3DShape3D::get_mesh_material_indices() const {
	PackedByteArray indices;
	if (mesh == nullptr) {
		return indices;
	}
	const uint8_t *src = b3GetMeshMaterialIndices(mesh);
	if (src == nullptr) {
		return indices;
	}
	indices.resize(mesh->triangleCount);
	uint8_t *write = indices.ptrw();
	for (int i = 0; i < mesh->triangleCount; i++) {
		write[i] = src[i];
	}
	return indices;
}
