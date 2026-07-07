/**************************************************************************/
/*  box3d_direct_space_state_3d.h                                          */
/**************************************************************************/

#pragma once

#include "core/templates/local_vector.h"
#include "core/templates/rid_owner.h"
#include "servers/physics_3d/physics_server_3d.h"

#include "box3d/box3d.h"

class Box3DBody3D;
class Box3DShape3D;
class Box3DSpace3D;

class Box3DDirectSpaceState3D : public PhysicsDirectSpaceState3D {
	GDCLASS(Box3DDirectSpaceState3D, PhysicsDirectSpaceState3D);

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
	static Box3DBody3D *_get_body(b3ShapeId p_shape_id);
	static Object *_get_instance(ObjectID p_id);
	static b3QueryFilter _make_filter(uint32_t p_collision_mask);
	static void _warn_ignored_shape_margin(real_t p_margin);

	bool _can_query_shape(b3ShapeId p_shape_id, const HashSet<RID> &p_exclude, bool p_collide_with_bodies, bool p_collide_with_areas) const;
	void _fill_shape_result(b3ShapeId p_shape_id, ShapeResult &r_result) const;
	void _fill_ray_result(b3ShapeId p_shape_id, b3Pos p_point, b3Vec3 p_normal, float p_fraction, int p_triangle_index, RayResult &r_result) const;
	bool _build_query_shape(RID p_shape_rid, const Transform3D &p_transform, QueryShape &r_query_shape) const;

	void setup(Box3DSpace3D *p_space, RID_PtrOwner<Box3DShape3D> *p_shape_owner, RID_PtrOwner<Box3DBody3D> *p_body_owner);

	virtual bool intersect_ray(const RayParameters &p_parameters, RayResult &r_result) override;
	virtual int intersect_point(const PointParameters &p_parameters, ShapeResult *r_results, int p_result_max) override;
	virtual int intersect_shape(const ShapeParameters &p_parameters, ShapeResult *r_results, int p_result_max) override;
	virtual bool cast_motion(const ShapeParameters &p_parameters, real_t &p_closest_safe, real_t &p_closest_unsafe, ShapeRestInfo *r_info = nullptr) override;
	virtual bool collide_shape(const ShapeParameters &p_parameters, Vector3 *r_results, int p_result_max, int &r_result_count) override;
	virtual bool rest_info(const ShapeParameters &p_parameters, ShapeRestInfo *r_info) override;
	virtual Vector3 get_closest_point_to_object_volume(RID p_object, const Vector3 p_point) const override;
};
