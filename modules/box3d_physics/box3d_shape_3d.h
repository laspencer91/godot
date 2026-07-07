/**************************************************************************/
/*  box3d_shape_3d.h — shared shape resource backed by Box3D geometry     */
/**************************************************************************/

#pragma once

#include "core/templates/hash_set.h"
#include "core/variant/variant.h"
#include "servers/physics_3d/physics_server_3d.h"

#include "box3d/box3d.h"
#include "box3d/collision.h"

class Box3DBody3D;

// One instance per Godot shape RID. Godot shapes are shared across bodies; Box3D
// shapes belong to bodies — so this class owns the reusable geometry (hull/mesh data)
// and each attached body creates its own b3ShapeId from it (see Box3DBody3D).
class Box3DShape3D {
	friend class Box3DBody3D;
	friend class Box3DDirectSpaceState3D;

	PhysicsServer3D::ShapeType type = PhysicsServer3D::SHAPE_CUSTOM;
	Variant data;

	// Built geometry, by type.
	float sphere_radius = 0.5f;
	float capsule_radius = 0.5f;
	float capsule_height = 2.0f; // Total height, including caps (Godot convention).
	b3BoxHull box_hull = {};
	bool box_built = false;
	b3HullData *hull = nullptr; // Convex polygon + cylinder approximation.
	b3MeshData *mesh = nullptr; // Concave polygon (trimesh). Referenced, not cloned, by Box3D.
	bool has_surface_material = false;
	int surface_material_id = 0;
	PackedInt64Array mesh_material_ids;
	PackedByteArray mesh_triangle_material_indices;

	HashSet<Box3DBody3D *> owners;

	void _clear_geometry();

public:
	explicit Box3DShape3D(PhysicsServer3D::ShapeType p_type) :
			type(p_type) {}
	~Box3DShape3D();

	PhysicsServer3D::ShapeType get_type() const { return type; }

	void set_data(const Variant &p_data);
	Variant get_data() const { return data; }
	void set_surface_material(int p_material_id);
	bool has_named_surface_material() const { return has_surface_material; }
	int get_surface_material_id() const { return surface_material_id; }
	void set_surface_map(const PackedInt64Array &p_material_ids, const PackedByteArray &p_triangle_indices);
	const PackedInt64Array &get_mesh_material_ids() const { return mesh_material_ids; }
	PackedByteArray get_mesh_material_indices() const;

	void add_owner(Box3DBody3D *p_body) { owners.insert(p_body); }
	void remove_owner(Box3DBody3D *p_body) { owners.erase(p_body); }
	const HashSet<Box3DBody3D *> &get_owners() const { return owners; }
};
