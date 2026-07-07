/**************************************************************************/
/*  box3d_shape_3d.cpp                                                    */
/**************************************************************************/

#include "box3d_shape_3d.h"

#include "box3d_body_3d.h"
#include "box3d_conversions.h"

#include "core/templates/local_vector.h"

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
		case PhysicsServer3D::SHAPE_SPHERE: {
			sphere_radius = p_data;
		} break;

		case PhysicsServer3D::SHAPE_BOX: {
			Vector3 half_extents = p_data;
			box_hull = b3MakeBoxHull((float)half_extents.x, (float)half_extents.y, (float)half_extents.z);
			box_built = true;
		} break;

		case PhysicsServer3D::SHAPE_CAPSULE: {
			Dictionary d = p_data;
			capsule_radius = d["radius"];
			capsule_height = d["height"];
		} break;

		case PhysicsServer3D::SHAPE_CYLINDER: {
			Dictionary d = p_data;
			float radius = d["radius"];
			float height = d["height"];
			// Box3D has no cylinder primitive; tessellated hull approximation (see llm/04 in the box3d repo).
			// b3CreateCylinder builds rings starting at yOffset, so center it on the origin.
			hull = b3CreateCylinder(height, radius, -0.5f * height, 24);
			ERR_FAIL_NULL_MSG(hull, "Box3D: failed to build cylinder hull.");
		} break;

		case PhysicsServer3D::SHAPE_CONVEX_POLYGON: {
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

		case PhysicsServer3D::SHAPE_CONCAVE_POLYGON: {
			Dictionary d = p_data;
			PackedVector3Array faces = d["faces"];
			int vertex_count = faces.size();
			ERR_FAIL_COND_MSG(vertex_count == 0 || vertex_count % 3 != 0, "Box3D: concave shape needs a triangle soup (3 vertices per face).");
			LocalVector<b3Vec3> vertices;
			LocalVector<int32_t> indices;
			vertices.resize(vertex_count);
			indices.resize(vertex_count);
			for (int i = 0; i < vertex_count; i++) {
				vertices[i] = to_box3d(faces[i]);
				indices[i] = i;
			}
			b3MeshDef mesh_def = {}; // Geometry defs are plain zero-init structs (no b3Default* factory).
			mesh_def.vertices = vertices.ptr();
			mesh_def.indices = indices.ptr();
			mesh_def.vertexCount = vertex_count;
			mesh_def.triangleCount = vertex_count / 3;
			mesh_def.weldVertices = true;
			mesh_def.weldTolerance = 0.0001f;
			mesh_def.identifyEdges = true; // Internal-edge (ghost collision) suppression.
			mesh = b3CreateMesh(&mesh_def, nullptr, 0);
			ERR_FAIL_NULL_MSG(mesh, "Box3D: failed to build collision mesh.");
		} break;

		case PhysicsServer3D::SHAPE_HEIGHTMAP: {
			WARN_PRINT_ONCE("Box3D: heightmap shapes not implemented yet; shape will not collide.");
		} break;

		case PhysicsServer3D::SHAPE_WORLD_BOUNDARY: {
			WARN_PRINT_ONCE("Box3D: world boundary shapes not implemented yet; shape will not collide.");
		} break;

		case PhysicsServer3D::SHAPE_SEPARATION_RAY: {
			WARN_PRINT_ONCE("Box3D: separation ray shapes not implemented yet; shape will not collide.");
		} break;

		default: {
			ERR_FAIL_MSG(vformat("Box3D: unsupported shape type %d.", (int)type));
		} break;
	}

	// Rebuild every body this shape is attached to.
	for (Box3DBody3D *body : owners) {
		body->shapes_changed();
	}
}
