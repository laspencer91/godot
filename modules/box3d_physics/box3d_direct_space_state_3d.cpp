/**************************************************************************/
/*  box3d_direct_space_state_3d.cpp                                        */
/**************************************************************************/

#include "box3d_direct_space_state_3d.h"

#include "box3d_body_3d.h"
#include "box3d_conversions.h"
#include "box3d_shape_3d.h"
#include "box3d_space_3d.h"

#include "core/object/object.h"

#include "box3d/collision.h"
#include "box3d/constants.h"

static constexpr uint64_t BOX3D_QUERY_FILTER_BIT = UINT64_C(1) << 63;

void Box3DDirectSpaceState3D::setup(Box3DSpace3D *p_space, RID_PtrOwner<Box3DShape3D> *p_shape_owner, RID_PtrOwner<Box3DBody3D> *p_body_owner) {
	space = p_space;
	shape_owner = p_shape_owner;
	body_owner = p_body_owner;
}

int Box3DDirectSpaceState3D::_get_shape_index(b3ShapeId p_shape_id) {
	return (int)(uintptr_t)b3Shape_GetUserData(p_shape_id);
}

Box3DBody3D *Box3DDirectSpaceState3D::_get_body(b3ShapeId p_shape_id) {
	if (!b3Shape_IsValid(p_shape_id)) {
		return nullptr;
	}
	b3BodyId body_id = b3Shape_GetBody(p_shape_id);
	if (!b3Body_IsValid(body_id)) {
		return nullptr;
	}
	return static_cast<Box3DBody3D *>(b3Body_GetUserData(body_id));
}

Object *Box3DDirectSpaceState3D::_get_instance(ObjectID p_id) {
	return p_id.is_valid() ? ObjectDB::get_instance(p_id) : nullptr;
}

b3QueryFilter Box3DDirectSpaceState3D::_make_filter(uint32_t p_collision_mask) {
	b3QueryFilter filter = b3DefaultQueryFilter();
	filter.categoryBits = BOX3D_QUERY_FILTER_BIT;
	filter.maskBits = (uint64_t)p_collision_mask;
	return filter;
}

void Box3DDirectSpaceState3D::_warn_ignored_shape_margin(real_t p_margin) {
	if (p_margin != 0.0) {
		WARN_PRINT_ONCE("Box3D: direct shape query margin is not implemented yet; using the shape geometry without extra margin.");
	}
}

bool Box3DDirectSpaceState3D::_can_query_shape(b3ShapeId p_shape_id, const HashSet<RID> &p_exclude, bool p_collide_with_bodies, bool p_collide_with_areas) const {
	(void)p_collide_with_areas;
	Box3DBody3D *body = _get_body(p_shape_id);
	if (body == nullptr) {
		return false;
	}
	if (!p_collide_with_bodies) {
		return false;
	}
	if (p_exclude.has(body->get_rid())) {
		return false;
	}
	return true;
}

void Box3DDirectSpaceState3D::_fill_shape_result(b3ShapeId p_shape_id, ShapeResult &r_result) const {
	Box3DBody3D *body = _get_body(p_shape_id);
	ERR_FAIL_NULL(body);

	r_result.rid = body->get_rid();
	r_result.collider_id = body->get_instance_id();
	r_result.collider = _get_instance(r_result.collider_id);
	r_result.shape = _get_shape_index(p_shape_id);
}

void Box3DDirectSpaceState3D::_fill_ray_result(b3ShapeId p_shape_id, b3Pos p_point, b3Vec3 p_normal, float p_fraction, int p_triangle_index, RayResult &r_result) const {
	Box3DBody3D *body = _get_body(p_shape_id);
	ERR_FAIL_NULL(body);

	r_result.position = to_godot(p_point);
	r_result.normal = to_godot(p_normal);
	r_result.rid = body->get_rid();
	r_result.collider_id = body->get_instance_id();
	r_result.collider = _get_instance(r_result.collider_id);
	r_result.shape = _get_shape_index(p_shape_id);
	r_result.face_index = p_triangle_index;
}

bool Box3DDirectSpaceState3D::_build_query_shape(RID p_shape_rid, const Transform3D &p_transform, QueryShape &r_query_shape) const {
	ERR_FAIL_NULL_V(shape_owner, false);
	Box3DShape3D *shape = shape_owner->get_or_null(p_shape_rid);
	ERR_FAIL_NULL_V(shape, false);

	r_query_shape.origin = to_box3d(p_transform.origin);
	r_query_shape.world_transform = to_box3d(p_transform);
	r_query_shape.points.clear();
	r_query_shape.proxy = {};
	r_query_shape.kind = QueryShape::KIND_NONE;
	r_query_shape.hull = nullptr;
	r_query_shape.valid = false;

	const Basis &basis = p_transform.basis;

	switch (shape->type) {
		case PhysicsServer3D::SHAPE_SPHERE: {
			r_query_shape.points.push_back(b3Vec3{ 0.0f, 0.0f, 0.0f });
			r_query_shape.kind = QueryShape::KIND_SPHERE;
			r_query_shape.sphere.center = b3Vec3{ 0.0f, 0.0f, 0.0f };
			r_query_shape.sphere.radius = shape->sphere_radius;
			r_query_shape.proxy.radius = shape->sphere_radius;
		} break;

		case PhysicsServer3D::SHAPE_CAPSULE: {
			const float half_cylinder = MAX(0.0f, 0.5f * shape->capsule_height - shape->capsule_radius);
			r_query_shape.kind = QueryShape::KIND_CAPSULE;
			r_query_shape.capsule.center1 = b3Vec3{ 0.0f, half_cylinder, 0.0f };
			r_query_shape.capsule.center2 = b3Vec3{ 0.0f, -half_cylinder, 0.0f };
			r_query_shape.capsule.radius = shape->capsule_radius;
			r_query_shape.points.push_back(to_box3d(basis.xform(Vector3(0, half_cylinder, 0))));
			r_query_shape.points.push_back(to_box3d(basis.xform(Vector3(0, -half_cylinder, 0))));
			r_query_shape.proxy.radius = shape->capsule_radius;
		} break;

		case PhysicsServer3D::SHAPE_BOX: {
			ERR_FAIL_COND_V(!shape->box_built, false);
			const b3Vec3 *points = b3GetHullPoints(&shape->box_hull.base);
			ERR_FAIL_NULL_V(points, false);
			r_query_shape.kind = QueryShape::KIND_HULL;
			r_query_shape.hull = &shape->box_hull.base;
			for (int i = 0; i < shape->box_hull.base.vertexCount; i++) {
				r_query_shape.points.push_back(to_box3d(basis.xform(to_godot(points[i]))));
			}
			r_query_shape.proxy.radius = 0.0f;
		} break;

		case PhysicsServer3D::SHAPE_CYLINDER:
		case PhysicsServer3D::SHAPE_CONVEX_POLYGON: {
			ERR_FAIL_NULL_V(shape->hull, false);
			ERR_FAIL_COND_V(shape->hull->vertexCount > B3_MAX_SHAPE_CAST_POINTS, false);
			const b3Vec3 *points = b3GetHullPoints(shape->hull);
			ERR_FAIL_NULL_V(points, false);
			r_query_shape.kind = QueryShape::KIND_HULL;
			r_query_shape.hull = shape->hull;
			for (int i = 0; i < shape->hull->vertexCount; i++) {
				r_query_shape.points.push_back(to_box3d(basis.xform(to_godot(points[i]))));
			}
			r_query_shape.proxy.radius = 0.0f;
		} break;

		default:
			ERR_FAIL_V_MSG(false, "Box3D: only sphere, capsule, box, cylinder, and convex polygon query shapes are supported.");
	}

	r_query_shape.proxy.points = r_query_shape.points.ptr();
	r_query_shape.proxy.count = r_query_shape.points.size();
	r_query_shape.valid = r_query_shape.proxy.count > 0;
	return r_query_shape.valid;
}

struct Box3DRayContext {
	const Box3DDirectSpaceState3D *state = nullptr;
	const PhysicsDirectSpaceState3D::RayParameters *parameters = nullptr;
	PhysicsDirectSpaceState3D::RayResult *result = nullptr;
	bool hit = false;
};

static float _ray_callback(b3ShapeId p_shape_id, b3Pos p_point, b3Vec3 p_normal, float p_fraction, uint64_t p_user_material_id, int p_triangle_index, int p_child_index, void *p_context) {
	Box3DRayContext *ctx = static_cast<Box3DRayContext *>(p_context);
	if (!ctx->state->_can_query_shape(p_shape_id, ctx->parameters->exclude, ctx->parameters->collide_with_bodies, ctx->parameters->collide_with_areas)) {
		return -1.0f;
	}

	ctx->state->_fill_ray_result(p_shape_id, p_point, p_normal, p_fraction, p_triangle_index, *ctx->result);
	ctx->hit = true;
	return p_fraction;
}

bool Box3DDirectSpaceState3D::intersect_ray(const RayParameters &p_parameters, RayResult &r_result) {
	ERR_FAIL_NULL_V(space, false);
	ERR_FAIL_COND_V_MSG(space->is_stepping(), false, "intersect_ray must not be called while the physics space is being stepped.");
	if (p_parameters.hit_from_inside || p_parameters.hit_back_faces || p_parameters.pick_ray) {
		WARN_PRINT_ONCE("Box3D: ray query hit_from_inside, hit_back_faces, and pick_ray flags are not implemented yet; using Box3D ray behavior.");
	}

	Box3DRayContext ctx;
	ctx.state = this;
	ctx.parameters = &p_parameters;
	ctx.result = &r_result;

	b3World_CastRay(space->get_world(), to_box3d(p_parameters.from), to_box3d(p_parameters.to - p_parameters.from), _make_filter(p_parameters.collision_mask), _ray_callback, &ctx);
	return ctx.hit;
}

struct Box3DOverlapContext {
	const Box3DDirectSpaceState3D *state = nullptr;
	const HashSet<RID> *exclude = nullptr;
	PhysicsDirectSpaceState3D::ShapeResult *results = nullptr;
	int result_count = 0;
	int result_max = 0;
	bool collide_with_bodies = true;
	bool collide_with_areas = false;
};

static bool _overlap_callback(b3ShapeId p_shape_id, void *p_context) {
	Box3DOverlapContext *ctx = static_cast<Box3DOverlapContext *>(p_context);
	if (!ctx->state->_can_query_shape(p_shape_id, *ctx->exclude, ctx->collide_with_bodies, ctx->collide_with_areas)) {
		return true;
	}
	if (ctx->result_count >= ctx->result_max) {
		return false;
	}
	ctx->state->_fill_shape_result(p_shape_id, ctx->results[ctx->result_count++]);
	return ctx->result_count < ctx->result_max;
}

int Box3DDirectSpaceState3D::intersect_point(const PointParameters &p_parameters, ShapeResult *r_results, int p_result_max) {
	ERR_FAIL_NULL_V(space, 0);
	ERR_FAIL_COND_V_MSG(space->is_stepping(), 0, "intersect_point must not be called while the physics space is being stepped.");
	if (p_result_max <= 0) {
		return 0;
	}

	b3Vec3 point = b3Vec3{ 0.0f, 0.0f, 0.0f };
	b3ShapeProxy proxy;
	proxy.points = &point;
	proxy.count = 1;
	proxy.radius = B3_LINEAR_SLOP;

	Box3DOverlapContext ctx;
	ctx.state = this;
	ctx.exclude = &p_parameters.exclude;
	ctx.results = r_results;
	ctx.result_max = p_result_max;
	ctx.collide_with_bodies = p_parameters.collide_with_bodies;
	ctx.collide_with_areas = p_parameters.collide_with_areas;

	b3World_OverlapShape(space->get_world(), to_box3d(p_parameters.position), &proxy, _make_filter(p_parameters.collision_mask), _overlap_callback, &ctx);
	return ctx.result_count;
}

int Box3DDirectSpaceState3D::intersect_shape(const ShapeParameters &p_parameters, ShapeResult *r_results, int p_result_max) {
	ERR_FAIL_NULL_V(space, 0);
	ERR_FAIL_COND_V_MSG(space->is_stepping(), 0, "intersect_shape must not be called while the physics space is being stepped.");
	if (p_result_max <= 0) {
		return 0;
	}

	QueryShape query_shape;
	ERR_FAIL_COND_V(!_build_query_shape(p_parameters.shape_rid, p_parameters.transform, query_shape), 0);
	_warn_ignored_shape_margin(p_parameters.margin);

	Box3DOverlapContext ctx;
	ctx.state = this;
	ctx.exclude = &p_parameters.exclude;
	ctx.results = r_results;
	ctx.result_max = p_result_max;
	ctx.collide_with_bodies = p_parameters.collide_with_bodies;
	ctx.collide_with_areas = p_parameters.collide_with_areas;

	b3World_OverlapShape(space->get_world(), query_shape.origin, &query_shape.proxy, _make_filter(p_parameters.collision_mask), _overlap_callback, &ctx);
	return ctx.result_count;
}

struct Box3DCastContext {
	const Box3DDirectSpaceState3D *state = nullptr;
	const PhysicsDirectSpaceState3D::ShapeParameters *parameters = nullptr;
	PhysicsDirectSpaceState3D::ShapeRestInfo *rest_info = nullptr;
	float fraction = 1.0f;
	bool hit = false;
};

static float _shape_cast_callback(b3ShapeId p_shape_id, b3Pos p_point, b3Vec3 p_normal, float p_fraction, uint64_t p_user_material_id, int p_triangle_index, int p_child_index, void *p_context) {
	Box3DCastContext *ctx = static_cast<Box3DCastContext *>(p_context);
	if (!ctx->state->_can_query_shape(p_shape_id, ctx->parameters->exclude, ctx->parameters->collide_with_bodies, ctx->parameters->collide_with_areas)) {
		return -1.0f;
	}

	ctx->fraction = p_fraction;
	ctx->hit = true;

	if (ctx->rest_info) {
		Box3DBody3D *body = Box3DDirectSpaceState3D::_get_body(p_shape_id);
		ERR_FAIL_NULL_V(body, p_fraction);
		ctx->rest_info->point = to_godot(p_point);
		ctx->rest_info->normal = to_godot(p_normal);
		ctx->rest_info->rid = body->get_rid();
		ctx->rest_info->collider_id = body->get_instance_id();
		ctx->rest_info->shape = Box3DDirectSpaceState3D::_get_shape_index(p_shape_id);
		ctx->rest_info->linear_velocity = to_godot(b3Body_GetWorldPointVelocity(body->get_body_id(), p_point));
	}

	return p_fraction;
}

bool Box3DDirectSpaceState3D::cast_motion(const ShapeParameters &p_parameters, real_t &p_closest_safe, real_t &p_closest_unsafe, ShapeRestInfo *r_info) {
	ERR_FAIL_NULL_V(space, false);
	ERR_FAIL_COND_V_MSG(space->is_stepping(), false, "cast_motion must not be called while the physics space is being stepped.");

	QueryShape query_shape;
	ERR_FAIL_COND_V(!_build_query_shape(p_parameters.shape_rid, p_parameters.transform, query_shape), false);
	_warn_ignored_shape_margin(p_parameters.margin);

	ShapeResult overlap_result;
	if (intersect_shape(p_parameters, &overlap_result, 1) > 0) {
		if (r_info) {
			rest_info(p_parameters, r_info);
		}
		p_closest_safe = 0.0;
		p_closest_unsafe = 0.0;
		return true;
	}

	Box3DCastContext ctx;
	ctx.state = this;
	ctx.parameters = &p_parameters;
	ctx.rest_info = r_info;

	b3World_CastShape(space->get_world(), query_shape.origin, &query_shape.proxy, to_box3d(p_parameters.motion), _make_filter(p_parameters.collision_mask), _shape_cast_callback, &ctx);

	if (ctx.hit) {
		const real_t motion_length = p_parameters.motion.length();
		const real_t safe_backoff = motion_length > CMP_EPSILON ? (real_t)B3_LINEAR_SLOP / motion_length : 0.0;
		p_closest_safe = MAX((real_t)0.0, (real_t)ctx.fraction - safe_backoff);
		p_closest_unsafe = ctx.fraction;
	} else {
		p_closest_safe = 1.0;
		p_closest_unsafe = 1.0;
	}
	return true;
}

struct Box3DQueryContact {
	Vector3 query_point;
	Vector3 collider_point;
	Vector3 normal;
	RID rid;
	ObjectID collider_id;
	int shape = 0;
	Vector3 linear_velocity;
	real_t separation = 0.0;
};

struct Box3DContactContext {
	const Box3DDirectSpaceState3D *state = nullptr;
	const PhysicsDirectSpaceState3D::ShapeParameters *parameters = nullptr;
	const Box3DDirectSpaceState3D::QueryShape *query_shape = nullptr;
	LocalVector<Box3DQueryContact> contacts;
	int result_max = 0;
	bool collect_all = false;
};

static void _append_manifold_contacts(Box3DContactContext *p_context, b3ShapeId p_shape_id, const b3LocalManifold &p_manifold, b3WorldTransform p_frame, bool p_query_is_a) {
	Box3DBody3D *body = Box3DDirectSpaceState3D::_get_body(p_shape_id);
	ERR_FAIL_NULL(body);

	const Vector3 normal_world = to_godot(b3RotateVector(p_frame.q, p_manifold.normal)).normalized();

	for (int i = 0; i < p_manifold.pointCount; i++) {
		if (!p_context->collect_all && p_context->contacts.size() >= (uint32_t)p_context->result_max) {
			return;
		}

		const b3LocalManifoldPoint &point = p_manifold.points[i];
		const Vector3 midpoint = to_godot(b3TransformWorldPoint(p_frame, point.point));
		const Vector3 half_separation = normal_world * ((real_t)point.separation * 0.5);

		Box3DQueryContact contact;
		if (p_query_is_a) {
			contact.query_point = midpoint - half_separation;
			contact.collider_point = midpoint + half_separation;
			contact.normal = -normal_world;
		} else {
			contact.query_point = midpoint + half_separation;
			contact.collider_point = midpoint - half_separation;
			contact.normal = normal_world;
		}
		contact.rid = body->get_rid();
		contact.collider_id = body->get_instance_id();
		contact.shape = Box3DDirectSpaceState3D::_get_shape_index(p_shape_id);
		contact.linear_velocity = to_godot(b3Body_GetWorldPointVelocity(body->get_body_id(), to_box3d(contact.collider_point)));
		contact.separation = point.separation;
		p_context->contacts.push_back(contact);
	}
}

struct Box3DMeshContactContext {
	Box3DContactContext *contact_context = nullptr;
	b3ShapeId shape_id = {};
	b3Transform body_to_query = {};
	const uint8_t *flags = nullptr;
};

static bool _mesh_contact_callback(b3Vec3 p_a, b3Vec3 p_b, b3Vec3 p_c, int p_triangle_index, void *p_context) {
	Box3DMeshContactContext *ctx = static_cast<Box3DMeshContactContext *>(p_context);
	Box3DContactContext *contact_context = ctx->contact_context;
	if (!contact_context->collect_all && contact_context->contacts.size() >= (uint32_t)contact_context->result_max) {
		return false;
	}

	b3Vec3 triangle_points[3] = {
		b3TransformPoint(ctx->body_to_query, p_a),
		b3TransformPoint(ctx->body_to_query, p_b),
		b3TransformPoint(ctx->body_to_query, p_c),
	};

	b3LocalManifoldPoint point_buffer[32];
	b3LocalManifold manifold = {};
	manifold.points = point_buffer;
	manifold.triangleFlags = ctx->flags ? ctx->flags[p_triangle_index] : 0;

	switch (contact_context->query_shape->kind) {
		case Box3DDirectSpaceState3D::QueryShape::KIND_SPHERE:
			b3CollideSphereAndTriangle(&manifold, 32, &contact_context->query_shape->sphere, triangle_points);
			break;
		case Box3DDirectSpaceState3D::QueryShape::KIND_CAPSULE: {
			b3SimplexCache cache = {};
			b3CollideCapsuleAndTriangle(&manifold, 32, &contact_context->query_shape->capsule, triangle_points, &cache);
		} break;
		case Box3DDirectSpaceState3D::QueryShape::KIND_HULL: {
			b3SATCache cache = {};
			b3CollideHullAndTriangle(&manifold, 32, contact_context->query_shape->hull, triangle_points[0], triangle_points[1], triangle_points[2], manifold.triangleFlags, &cache);
		} break;
		default:
			break;
	}

	if (manifold.pointCount > 0) {
		for (int j = 0; j < manifold.pointCount; j++) {
			manifold.points[j].triangleIndex = p_triangle_index;
		}
		_append_manifold_contacts(contact_context, ctx->shape_id, manifold, contact_context->query_shape->world_transform, false);
	}

	return contact_context->collect_all || contact_context->contacts.size() < (uint32_t)contact_context->result_max;
}

static void _collide_query_with_mesh(Box3DContactContext *p_context, b3ShapeId p_shape_id, b3WorldTransform p_body_transform) {
	const b3Mesh mesh = b3Shape_GetMesh(p_shape_id);
	ERR_FAIL_NULL(mesh.data);
	ERR_FAIL_COND(p_context->query_shape->proxy.count <= 0);

	LocalVector<b3Vec3> body_points;
	body_points.resize(p_context->query_shape->proxy.count);
	for (int i = 0; i < p_context->query_shape->proxy.count; i++) {
		body_points[i] = b3InvTransformWorldPoint(p_body_transform, b3OffsetPos(p_context->query_shape->origin, p_context->query_shape->proxy.points[i]));
	}

	const b3AABB bounds = b3MakeAABB(body_points.ptr(), body_points.size(), p_context->query_shape->proxy.radius + B3_LINEAR_SLOP);
	Box3DMeshContactContext ctx;
	ctx.contact_context = p_context;
	ctx.shape_id = p_shape_id;
	ctx.body_to_query = b3InvMulWorldTransforms(p_context->query_shape->world_transform, p_body_transform);
	ctx.flags = b3GetMeshFlags(mesh.data);
	b3QueryMesh(&mesh, bounds, _mesh_contact_callback, &ctx);
}

static void _collide_query_with_shape(Box3DContactContext *p_context, b3ShapeId p_shape_id, b3WorldTransform p_body_transform) {
	const b3ShapeType shape_type = b3Shape_GetType(p_shape_id);
	const b3Transform body_to_query = b3InvMulWorldTransforms(p_context->query_shape->world_transform, p_body_transform);
	const b3Transform query_to_body = b3InvMulWorldTransforms(p_body_transform, p_context->query_shape->world_transform);

	b3LocalManifoldPoint point_buffer[32];
	b3LocalManifold manifold = {};
	manifold.points = point_buffer;

	bool query_is_a = true;
	b3WorldTransform manifold_frame = p_context->query_shape->world_transform;

	switch (shape_type) {
		case b3_sphereShape: {
			const b3Sphere sphere = b3Shape_GetSphere(p_shape_id);
			switch (p_context->query_shape->kind) {
				case Box3DDirectSpaceState3D::QueryShape::KIND_SPHERE:
					b3CollideSpheres(&manifold, 32, &p_context->query_shape->sphere, &sphere, body_to_query);
					break;
				case Box3DDirectSpaceState3D::QueryShape::KIND_CAPSULE:
					b3CollideCapsuleAndSphere(&manifold, 32, &p_context->query_shape->capsule, &sphere, body_to_query);
					break;
				case Box3DDirectSpaceState3D::QueryShape::KIND_HULL: {
					b3SimplexCache cache = {};
					b3CollideHullAndSphere(&manifold, 32, p_context->query_shape->hull, &sphere, body_to_query, &cache);
				} break;
				default:
					break;
			}
		} break;

		case b3_capsuleShape: {
			const b3Capsule capsule = b3Shape_GetCapsule(p_shape_id);
			switch (p_context->query_shape->kind) {
				case Box3DDirectSpaceState3D::QueryShape::KIND_SPHERE:
					b3CollideCapsuleAndSphere(&manifold, 32, &capsule, &p_context->query_shape->sphere, query_to_body);
					query_is_a = false;
					manifold_frame = p_body_transform;
					break;
				case Box3DDirectSpaceState3D::QueryShape::KIND_CAPSULE:
					b3CollideCapsules(&manifold, 32, &p_context->query_shape->capsule, &capsule, body_to_query);
					break;
				case Box3DDirectSpaceState3D::QueryShape::KIND_HULL: {
					b3SimplexCache cache = {};
					b3CollideHullAndCapsule(&manifold, 32, p_context->query_shape->hull, &capsule, body_to_query, &cache);
				} break;
				default:
					break;
			}
		} break;

		case b3_hullShape: {
			const b3HullData *hull = b3Shape_GetHull(p_shape_id);
			ERR_FAIL_NULL(hull);
			switch (p_context->query_shape->kind) {
				case Box3DDirectSpaceState3D::QueryShape::KIND_SPHERE: {
					b3SimplexCache cache = {};
					b3CollideHullAndSphere(&manifold, 32, hull, &p_context->query_shape->sphere, query_to_body, &cache);
					query_is_a = false;
					manifold_frame = p_body_transform;
				} break;
				case Box3DDirectSpaceState3D::QueryShape::KIND_CAPSULE: {
					b3SimplexCache cache = {};
					b3CollideHullAndCapsule(&manifold, 32, hull, &p_context->query_shape->capsule, query_to_body, &cache);
					query_is_a = false;
					manifold_frame = p_body_transform;
				} break;
				case Box3DDirectSpaceState3D::QueryShape::KIND_HULL: {
					b3SATCache cache = {};
					b3CollideHulls(&manifold, 32, p_context->query_shape->hull, hull, body_to_query, &cache);
				} break;
				default:
					break;
			}
		} break;

		case b3_meshShape:
			_collide_query_with_mesh(p_context, p_shape_id, p_body_transform);
			return;

		default:
			return;
	}

	if (manifold.pointCount > 0) {
		_append_manifold_contacts(p_context, p_shape_id, manifold, manifold_frame, query_is_a);
	}
}

static bool _contact_callback(b3ShapeId p_shape_id, void *p_context) {
	Box3DContactContext *ctx = static_cast<Box3DContactContext *>(p_context);
	if (!ctx->state->_can_query_shape(p_shape_id, ctx->parameters->exclude, ctx->parameters->collide_with_bodies, ctx->parameters->collide_with_areas)) {
		return true;
	}
	Box3DBody3D *body = Box3DDirectSpaceState3D::_get_body(p_shape_id);
	ERR_FAIL_NULL_V(body, false);

	_collide_query_with_shape(ctx, p_shape_id, b3Body_GetTransform(body->get_body_id()));
	return ctx->collect_all || ctx->contacts.size() < (uint32_t)ctx->result_max;
}

bool Box3DDirectSpaceState3D::collide_shape(const ShapeParameters &p_parameters, Vector3 *r_results, int p_result_max, int &r_result_count) {
	r_result_count = 0;
	ERR_FAIL_NULL_V(space, false);
	ERR_FAIL_COND_V_MSG(space->is_stepping(), false, "collide_shape must not be called while the physics space is being stepped.");
	if (p_result_max <= 0) {
		return false;
	}

	QueryShape query_shape;
	ERR_FAIL_COND_V(!_build_query_shape(p_parameters.shape_rid, p_parameters.transform, query_shape), false);
	_warn_ignored_shape_margin(p_parameters.margin);

	Box3DContactContext ctx;
	ctx.state = this;
	ctx.parameters = &p_parameters;
	ctx.query_shape = &query_shape;
	ctx.result_max = p_result_max;

	b3World_OverlapShape(space->get_world(), query_shape.origin, &query_shape.proxy, _make_filter(p_parameters.collision_mask), _contact_callback, &ctx);
	if (ctx.contacts.is_empty()) {
		return false;
	}

	const int contact_count = MIN((int)ctx.contacts.size(), p_result_max);
	for (int i = 0; i < contact_count; i++) {
		r_results[i * 2 + 0] = ctx.contacts[i].query_point;
		r_results[i * 2 + 1] = ctx.contacts[i].collider_point;
	}
	r_result_count = contact_count;
	return true;
}

bool Box3DDirectSpaceState3D::rest_info(const ShapeParameters &p_parameters, ShapeRestInfo *r_info) {
	ERR_FAIL_NULL_V(space, false);
	ERR_FAIL_NULL_V(r_info, false);
	ERR_FAIL_COND_V_MSG(space->is_stepping(), false, "rest_info must not be called while the physics space is being stepped.");

	QueryShape query_shape;
	ERR_FAIL_COND_V(!_build_query_shape(p_parameters.shape_rid, p_parameters.transform, query_shape), false);
	_warn_ignored_shape_margin(p_parameters.margin);

	Box3DContactContext ctx;
	ctx.state = this;
	ctx.parameters = &p_parameters;
	ctx.query_shape = &query_shape;
	ctx.collect_all = true;

	b3World_OverlapShape(space->get_world(), query_shape.origin, &query_shape.proxy, _make_filter(p_parameters.collision_mask), _contact_callback, &ctx);
	if (ctx.contacts.is_empty()) {
		return false;
	}

	const Box3DQueryContact *best = &ctx.contacts[0];
	for (uint32_t i = 1; i < ctx.contacts.size(); i++) {
		if (ctx.contacts[i].separation < best->separation) {
			best = &ctx.contacts[i];
		}
	}

	r_info->point = best->collider_point;
	r_info->normal = best->normal;
	r_info->rid = best->rid;
	r_info->collider_id = best->collider_id;
	r_info->shape = best->shape;
	r_info->linear_velocity = best->linear_velocity;
	return true;
}

Vector3 Box3DDirectSpaceState3D::get_closest_point_to_object_volume(RID p_object, const Vector3 p_point) const {
	ERR_FAIL_NULL_V(space, Vector3());
	ERR_FAIL_NULL_V(body_owner, Vector3());
	ERR_FAIL_COND_V_MSG(space->is_stepping(), Vector3(), "get_closest_point_to_object_volume must not be called while the physics space is being stepped.");

	Box3DBody3D *body = body_owner->get_or_null(p_object);
	ERR_FAIL_NULL_V(body, Vector3());
	ERR_FAIL_COND_V(body->get_space() != space || !body->in_space(), Vector3());

	b3Vec3 closest = {};
	b3Body_GetClosestPoint(body->get_body_id(), &closest, to_box3d(p_point));
	return to_godot(closest);
}
