/**************************************************************************/
/*  box3d_direct_space_state_3d.h                                          */
/**************************************************************************/

#pragma once

#include "core/templates/local_vector.h"
#include "core/templates/rid_owner.h"
#include "servers/physics_3d/physics_server_3d.h"

#include "box3d/box3d.h"

class Box3DBody3D;
class Box3DCollisionObject3D;
class Box3DShape3D;
class Box3DSpace3D;

class Box3DDirectSpaceState3D : public PhysicsDirectSpaceState3D {
	GDCLASS(Box3DDirectSpaceState3D, PhysicsDirectSpaceState3D);

protected:
	static void _bind_methods();

public:
	struct QueryShape {
		enum Kind {
			KIND_NONE,
			KIND_SPHERE,
			KIND_CAPSULE,
			KIND_HULL,
		};

		LocalVector<b3Vec3> points;
		b3ShapeProxy proxy = {};
		b3Pos origin = {};
		b3WorldTransform world_transform = {};
		Kind kind = KIND_NONE;
		b3Sphere sphere = {};
		b3Capsule capsule = {};
		const b3HullData *hull = nullptr;
		bool valid = false;
	};

	Box3DSpace3D *space = nullptr;
	RID_PtrOwner<Box3DShape3D> *shape_owner = nullptr;
	RID_PtrOwner<Box3DBody3D> *body_owner = nullptr;

	static int _get_shape_index(b3ShapeId p_shape_id);
	static Box3DCollisionObject3D *_get_object(b3ShapeId p_shape_id);
	static Box3DBody3D *_get_body(b3ShapeId p_shape_id);
	static Object *_get_instance(ObjectID p_id);
	static void _warn_ignored_shape_margin(real_t p_margin);

	// Builds the b3QueryFilter every engine-side Box3D scene query uses (the
	// reserved query category bit + the caller's collision mask). Single source
	// of truth for the query category: sibling modules issuing native Box3D
	// queries (horde_sim's agent mover sweeps) must build their filters here so
	// a change to the category bit can never silently stop their queries from
	// matching map shapes.
	static b3QueryFilter make_query_filter(uint32_t p_collision_mask, bool p_hit_back_faces = false);

	bool _can_query_shape(b3ShapeId p_shape_id, const HashSet<RID> &p_exclude, bool p_collide_with_bodies, bool p_collide_with_areas) const;
	void _fill_shape_result(b3ShapeId p_shape_id, PS3DT::ShapeResult &r_result) const;
	void _fill_ray_result(b3ShapeId p_shape_id, b3Pos p_point, b3Vec3 p_normal, float p_fraction, int p_triangle_index, PS3DT::RayResult &r_result) const;
	bool _build_query_shape(RID p_shape_rid, const Transform3D &p_transform, QueryShape &r_query_shape, real_t p_margin = 0.0) const;
	bool _intersect_ray_internal(const PS3DT::RayParameters &p_parameters, PS3DT::RayResult &r_result, uint64_t *r_user_material_id = nullptr) const;

	void setup(Box3DSpace3D *p_space, RID_PtrOwner<Box3DShape3D> *p_shape_owner, RID_PtrOwner<Box3DBody3D> *p_body_owner);

	Dictionary intersect_ray_ex(const Ref<PhysicsRayQueryParameters3D> &p_ray_query) const;
	virtual bool intersect_ray(const PS3DT::RayParameters &p_parameters, PS3DT::RayResult &r_result) override;
	virtual int intersect_point(const PS3DT::PointParameters &p_parameters, PS3DT::ShapeResult *r_results, int p_result_max) override;
	virtual int intersect_shape(const PS3DT::ShapeParameters &p_parameters, PS3DT::ShapeResult *r_results, int p_result_max) override;
	virtual bool cast_motion(const PS3DT::ShapeParameters &p_parameters, real_t &p_closest_safe, real_t &p_closest_unsafe, PS3DT::ShapeRestInfo *r_info = nullptr) override;
	virtual bool collide_shape(const PS3DT::ShapeParameters &p_parameters, Vector3 *r_results, int p_result_max, int &r_result_count) override;
	virtual bool rest_info(const PS3DT::ShapeParameters &p_parameters, PS3DT::ShapeRestInfo *r_info) override;
	virtual Vector3 get_closest_point_to_object_volume(RID p_object, const Vector3 p_point) const override;
	bool body_test_motion(const Box3DBody3D &p_body, const PS3DT::MotionParameters &p_parameters, PS3DT::MotionResult *r_result) const;
};
