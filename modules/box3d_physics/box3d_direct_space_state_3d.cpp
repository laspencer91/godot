/**************************************************************************/
/*  box3d_direct_space_state_3d.cpp                                        */
/**************************************************************************/

#include "box3d_direct_space_state_3d.h"

#include "box3d_area_3d.h"
#include "box3d_body_3d.h"
#include "box3d_collision_object_3d.h"
#include "box3d_conversions.h"
#include "box3d_shape_3d.h"
#include "box3d_space_3d.h"
#include "box3d_surface_materials.h"

#include "core/object/class_db.h"
#include "core/object/object.h"

#include "box3d/collision.h"
#include "box3d/constants.h"
#include "box3d/math_functions.h"

static constexpr uint64_t BOX3D_QUERY_FILTER_BIT = UINT64_C(1) << 63;

static Dictionary _box3d_ray_result_to_dictionary(const PS3DT::RayResult &p_result) {
	Dictionary d;
	d["position"] = p_result.position;
	d["normal"] = p_result.normal;
	d["face_index"] = p_result.face_index;
	d["collider_id"] = p_result.collider_id;
	d["collider"] = p_result.collider;
	d["shape"] = p_result.shape;
	d["rid"] = p_result.rid;
	return d;
}

static void _box3d_add_material_fields(Dictionary &r_result, int p_material_id) {
	r_result["material_id"] = p_material_id;

	Box3DPhysics *box3d_physics = Box3DPhysics::get_singleton();
	if (box3d_physics == nullptr || p_material_id <= 0) {
		r_result["material_name"] = StringName();
		r_result["material"] = Ref<Box3DSurfaceMaterial>();
		return;
	}

	r_result["material_name"] = box3d_physics->get_material_name(p_material_id);
	r_result["material"] = box3d_physics->get_material(p_material_id);
}

void Box3DDirectSpaceState3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("intersect_ray_ex", "parameters"), &Box3DDirectSpaceState3D::intersect_ray_ex);
}

void Box3DDirectSpaceState3D::setup(Box3DSpace3D *p_space, RID_PtrOwner<Box3DShape3D> *p_shape_owner, RID_PtrOwner<Box3DBody3D> *p_body_owner) {
	space = p_space;
	shape_owner = p_shape_owner;
	body_owner = p_body_owner;
}

int Box3DDirectSpaceState3D::_get_shape_index(b3ShapeId p_shape_id) {
	return (int)(uintptr_t)b3Shape_GetUserData(p_shape_id);
}

Box3DCollisionObject3D *Box3DDirectSpaceState3D::_get_object(b3ShapeId p_shape_id) {
	if (!b3Shape_IsValid(p_shape_id)) {
		return nullptr;
	}
	b3BodyId body_id = b3Shape_GetBody(p_shape_id);
	if (!b3Body_IsValid(body_id)) {
		return nullptr;
	}
	return static_cast<Box3DCollisionObject3D *>(b3Body_GetUserData(body_id));
}

Box3DBody3D *Box3DDirectSpaceState3D::_get_body(b3ShapeId p_shape_id) {
	Box3DCollisionObject3D *object = _get_object(p_shape_id);
	if (object == nullptr || object->get_type() != Box3DCollisionObject3D::TYPE_BODY) {
		return nullptr;
	}
	return static_cast<Box3DBody3D *>(object);
}

Object *Box3DDirectSpaceState3D::_get_instance(ObjectID p_id) {
	return p_id.is_valid() ? ObjectDB::get_instance(p_id) : nullptr;
}

b3QueryFilter Box3DDirectSpaceState3D::make_query_filter(uint32_t p_collision_mask, bool p_hit_back_faces) {
	b3QueryFilter filter = b3DefaultQueryFilter();
	filter.categoryBits = BOX3D_QUERY_FILTER_BIT;
	filter.maskBits = (uint64_t)p_collision_mask;
	filter.hitBackFaces = p_hit_back_faces;
	return filter;
}

void Box3DDirectSpaceState3D::_warn_ignored_shape_margin(real_t p_margin) {
	if (p_margin != 0.0) {
		WARN_PRINT_ONCE("Box3D: direct shape query margin is not implemented yet; using the shape geometry without extra margin.");
	}
}

bool Box3DDirectSpaceState3D::_can_query_shape(b3ShapeId p_shape_id, const HashSet<RID> &p_exclude, bool p_collide_with_bodies, bool p_collide_with_areas) const {
	Box3DCollisionObject3D *object = _get_object(p_shape_id);
	if (object == nullptr) {
		return false;
	}
	if (object->get_type() == Box3DCollisionObject3D::TYPE_BODY && !p_collide_with_bodies) {
		return false;
	}
	if (object->get_type() == Box3DCollisionObject3D::TYPE_AREA && !p_collide_with_areas) {
		return false;
	}
	if (p_exclude.has(object->get_rid())) {
		return false;
	}
	return true;
}

void Box3DDirectSpaceState3D::_fill_shape_result(b3ShapeId p_shape_id, PS3DT::ShapeResult &r_result) const {
	Box3DCollisionObject3D *object = _get_object(p_shape_id);
	ERR_FAIL_NULL(object);

	r_result.rid = object->get_rid();
	r_result.collider_id = object->get_instance_id();
	r_result.collider = _get_instance(r_result.collider_id);
	r_result.shape = _get_shape_index(p_shape_id);
}

void Box3DDirectSpaceState3D::_fill_ray_result(b3ShapeId p_shape_id, b3Pos p_point, b3Vec3 p_normal, float p_fraction, int p_triangle_index, PS3DT::RayResult &r_result) const {
	Box3DCollisionObject3D *object = _get_object(p_shape_id);
	ERR_FAIL_NULL(object);

	r_result.position = to_godot(p_point);
	r_result.normal = to_godot(p_normal);
	r_result.rid = object->get_rid();
	r_result.collider_id = object->get_instance_id();
	r_result.collider = _get_instance(r_result.collider_id);
	r_result.shape = _get_shape_index(p_shape_id);
	r_result.face_index = p_triangle_index;
}

bool Box3DDirectSpaceState3D::_build_query_shape(RID p_shape_rid, const Transform3D &p_transform, QueryShape &r_query_shape, real_t p_margin) const {
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
	const float margin = MAX(0.0f, (float)p_margin);

	switch (shape->type) {
		case PS3DE::SHAPE_SPHERE: {
			r_query_shape.points.push_back(b3Vec3{ 0.0f, 0.0f, 0.0f });
			r_query_shape.kind = QueryShape::KIND_SPHERE;
			r_query_shape.sphere.center = b3Vec3{ 0.0f, 0.0f, 0.0f };
			r_query_shape.sphere.radius = shape->sphere_radius + margin;
			r_query_shape.proxy.radius = shape->sphere_radius + margin;
		} break;

		case PS3DE::SHAPE_CAPSULE: {
			const float half_cylinder = MAX(0.0f, 0.5f * shape->capsule_height - shape->capsule_radius);
			r_query_shape.kind = QueryShape::KIND_CAPSULE;
			r_query_shape.capsule.center1 = b3Vec3{ 0.0f, half_cylinder, 0.0f };
			r_query_shape.capsule.center2 = b3Vec3{ 0.0f, -half_cylinder, 0.0f };
			r_query_shape.capsule.radius = shape->capsule_radius + margin;
			r_query_shape.points.push_back(to_box3d(basis.xform(Vector3(0, half_cylinder, 0))));
			r_query_shape.points.push_back(to_box3d(basis.xform(Vector3(0, -half_cylinder, 0))));
			r_query_shape.proxy.radius = shape->capsule_radius + margin;
		} break;

		case PS3DE::SHAPE_BOX: {
			ERR_FAIL_COND_V(!shape->box_built, false);
			const b3Vec3 *points = b3GetHullPoints(&shape->box_hull.base);
			ERR_FAIL_NULL_V(points, false);
			r_query_shape.kind = QueryShape::KIND_HULL;
			r_query_shape.hull = &shape->box_hull.base;
			for (int i = 0; i < shape->box_hull.base.vertexCount; i++) {
				r_query_shape.points.push_back(to_box3d(basis.xform(to_godot(points[i]))));
			}
			r_query_shape.proxy.radius = margin;
		} break;

		case PS3DE::SHAPE_CYLINDER:
		case PS3DE::SHAPE_CONVEX_POLYGON: {
			ERR_FAIL_NULL_V(shape->hull, false);
			ERR_FAIL_COND_V(shape->hull->vertexCount > B3_MAX_SHAPE_CAST_POINTS, false);
			const b3Vec3 *points = b3GetHullPoints(shape->hull);
			ERR_FAIL_NULL_V(points, false);
			r_query_shape.kind = QueryShape::KIND_HULL;
			r_query_shape.hull = shape->hull;
			for (int i = 0; i < shape->hull->vertexCount; i++) {
				r_query_shape.points.push_back(to_box3d(basis.xform(to_godot(points[i]))));
			}
			r_query_shape.proxy.radius = margin;
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
	const PS3DT::RayParameters *parameters = nullptr;
	PS3DT::RayResult *result = nullptr;
	uint64_t *user_material_id = nullptr;
	bool hit = false;
};

static bool _is_heightmap_back_face(Box3DCollisionObject3D *object, b3ShapeId p_shape_id, b3Vec3 p_normal) {
	const int shape_index = Box3DDirectSpaceState3D::_get_shape_index(p_shape_id);
	Box3DShape3D *shape = nullptr;
	Transform3D shape_transform;
	if (object->get_type() == Box3DCollisionObject3D::TYPE_BODY) {
		Box3DBody3D *body = static_cast<Box3DBody3D *>(object);
		const Box3DBody3D::ShapeSlot *slot = body->get_shape_slot(shape_index);
		if (slot != nullptr) {
			shape = slot->shape;
			shape_transform = body->get_transform() * slot->xform;
		}
	} else {
		Box3DArea3D *area = static_cast<Box3DArea3D *>(object);
		const Box3DArea3D::ShapeSlot *slot = area->get_shape_slot(shape_index);
		if (slot != nullptr) {
			shape = slot->shape;
			shape_transform = area->get_transform() * slot->xform;
		}
	}

	if (shape == nullptr || shape->get_type() != PS3DE::SHAPE_HEIGHTMAP) {
		return false;
	}

	// The source heightmap winding has a positive local-Y normal. Correct for
	// mirrored transforms because transforming triangle vertices flips winding.
	const real_t orientation = shape_transform.basis.determinant() < 0.0 ? -1.0 : 1.0;
	const Vector3 transformed_up = shape_transform.basis.xform(Vector3(0.0, 1.0, 0.0));
	return orientation * to_godot(p_normal).dot(transformed_up) < 0.0;
}

static float _ray_callback(b3ShapeId p_shape_id, b3Pos p_point, b3Vec3 p_normal, float p_fraction, uint64_t p_user_material_id, int p_triangle_index, int p_child_index, void *p_context) {
	Box3DRayContext *ctx = static_cast<Box3DRayContext *>(p_context);
	if (!ctx->state->_can_query_shape(p_shape_id, ctx->parameters->exclude, ctx->parameters->collide_with_bodies, ctx->parameters->collide_with_areas)) {
		return -1.0f;
	}
	if (p_fraction == 0.0f && !ctx->parameters->hit_from_inside) {
		return -1.0f;
	}

	Box3DCollisionObject3D *object = ctx->state->_get_object(p_shape_id);
	if (ctx->parameters->pick_ray && !object->is_ray_pickable()) {
		return -1.0f;
	}
	if (!ctx->parameters->hit_back_faces && _is_heightmap_back_face(object, p_shape_id, p_normal)) {
		return -1.0f;
	}

	ctx->state->_fill_ray_result(p_shape_id, p_point, p_normal, p_fraction, p_triangle_index, *ctx->result);
	if (ctx->user_material_id != nullptr) {
		*ctx->user_material_id = p_user_material_id;
	}
	ctx->hit = true;
	return p_fraction;
}

bool Box3DDirectSpaceState3D::_intersect_ray_internal(const PS3DT::RayParameters &p_parameters, PS3DT::RayResult &r_result, uint64_t *r_user_material_id) const {
	ERR_FAIL_NULL_V(space, false);
	ERR_FAIL_COND_V_MSG(space->is_stepping(), false, "intersect_ray must not be called while the physics space is being stepped.");

	Box3DRayContext ctx;
	ctx.state = this;
	ctx.parameters = &p_parameters;
	ctx.result = &r_result;
	ctx.user_material_id = r_user_material_id;

	b3World_CastRay(space->get_world(), to_box3d(p_parameters.from), to_box3d(p_parameters.to - p_parameters.from), make_query_filter(p_parameters.collision_mask, p_parameters.hit_back_faces), _ray_callback, &ctx);
	return ctx.hit;
}

Dictionary Box3DDirectSpaceState3D::intersect_ray_ex(const Ref<PhysicsRayQueryParameters3D> &p_ray_query) const {
	ERR_FAIL_COND_V(p_ray_query.is_null(), Dictionary());

	PS3DT::RayResult result;
	uint64_t user_material_id = 0;
	if (!_intersect_ray_internal(p_ray_query->get_parameters(), result, &user_material_id)) {
		return Dictionary();
	}

	Dictionary d = _box3d_ray_result_to_dictionary(result);
	_box3d_add_material_fields(d, user_material_id <= (uint64_t)INT_MAX ? (int)user_material_id : 0);
	return d;
}

bool Box3DDirectSpaceState3D::intersect_ray(const PS3DT::RayParameters &p_parameters, PS3DT::RayResult &r_result) {
	return _intersect_ray_internal(p_parameters, r_result);
}

struct Box3DOverlapContext {
	const Box3DDirectSpaceState3D *state = nullptr;
	const HashSet<RID> *exclude = nullptr;
	PS3DT::ShapeResult *results = nullptr;
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

int Box3DDirectSpaceState3D::intersect_point(const PS3DT::PointParameters &p_parameters, PS3DT::ShapeResult *r_results, int p_result_max) {
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

	b3World_OverlapShape(space->get_world(), to_box3d(p_parameters.position), &proxy, make_query_filter(p_parameters.collision_mask), _overlap_callback, &ctx);
	return ctx.result_count;
}

int Box3DDirectSpaceState3D::intersect_shape(const PS3DT::ShapeParameters &p_parameters, PS3DT::ShapeResult *r_results, int p_result_max) {
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

	b3World_OverlapShape(space->get_world(), query_shape.origin, &query_shape.proxy, make_query_filter(p_parameters.collision_mask), _overlap_callback, &ctx);
	return ctx.result_count;
}

struct Box3DCastContext {
	const Box3DDirectSpaceState3D *state = nullptr;
	const PS3DT::ShapeParameters *parameters = nullptr;
	PS3DT::ShapeRestInfo *rest_info = nullptr;
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

bool Box3DDirectSpaceState3D::cast_motion(const PS3DT::ShapeParameters &p_parameters, real_t &p_closest_safe, real_t &p_closest_unsafe, PS3DT::ShapeRestInfo *r_info) {
	ERR_FAIL_NULL_V(space, false);
	ERR_FAIL_COND_V_MSG(space->is_stepping(), false, "cast_motion must not be called while the physics space is being stepped.");

	QueryShape query_shape;
	ERR_FAIL_COND_V(!_build_query_shape(p_parameters.shape_rid, p_parameters.transform, query_shape), false);
	_warn_ignored_shape_margin(p_parameters.margin);

	PS3DT::ShapeResult overlap_result;
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

	b3World_CastShape(space->get_world(), query_shape.origin, &query_shape.proxy, to_box3d(p_parameters.motion), make_query_filter(p_parameters.collision_mask), _shape_cast_callback, &ctx);

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
	int local_shape = 0;
	int shape = 0;
	Vector3 linear_velocity;
	Vector3 angular_velocity;
	real_t separation = 0.0;
};

struct Box3DContactContext {
	const Box3DDirectSpaceState3D *state = nullptr;
	const PS3DT::ShapeParameters *parameters = nullptr;
	const Box3DDirectSpaceState3D::QueryShape *query_shape = nullptr;
	const Box3DBody3D *motion_body = nullptr;
	const HashSet<ObjectID> *exclude_objects = nullptr;
	LocalVector<Box3DQueryContact> contacts;
	int result_max = 0;
	int local_shape = 0;
	real_t contact_margin = 0.0;
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
		contact.local_shape = p_context->local_shape;
		contact.shape = Box3DDirectSpaceState3D::_get_shape_index(p_shape_id);
		contact.linear_velocity = to_godot(b3Body_GetWorldPointVelocity(body->get_body_id(), to_box3d(contact.collider_point)));
		contact.angular_velocity = to_godot(b3Body_GetAngularVelocity(body->get_body_id()));
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

static b3Vec3 _relative_to_origin(b3Pos p_point, b3Pos p_origin) {
	return b3Vec3{
		(float)(p_point.x - p_origin.x),
		(float)(p_point.y - p_origin.y),
		(float)(p_point.z - p_origin.z),
	};
}

static bool _build_shape_proxy_relative_to_origin(b3ShapeId p_shape_id, b3WorldTransform p_body_transform, b3Pos p_origin, LocalVector<b3Vec3> &r_points, b3ShapeProxy &r_proxy) {
	r_points.clear();
	r_proxy = {};

	switch (b3Shape_GetType(p_shape_id)) {
		case b3_sphereShape: {
			const b3Sphere sphere = b3Shape_GetSphere(p_shape_id);
			r_points.push_back(_relative_to_origin(b3TransformWorldPoint(p_body_transform, sphere.center), p_origin));
			r_proxy.radius = sphere.radius;
		} break;

		case b3_capsuleShape: {
			const b3Capsule capsule = b3Shape_GetCapsule(p_shape_id);
			r_points.push_back(_relative_to_origin(b3TransformWorldPoint(p_body_transform, capsule.center1), p_origin));
			r_points.push_back(_relative_to_origin(b3TransformWorldPoint(p_body_transform, capsule.center2), p_origin));
			r_proxy.radius = capsule.radius;
		} break;

		case b3_hullShape: {
			const b3HullData *hull = b3Shape_GetHull(p_shape_id);
			ERR_FAIL_NULL_V(hull, false);
			const b3Vec3 *points = b3GetHullPoints(hull);
			ERR_FAIL_NULL_V(points, false);
			for (int i = 0; i < hull->vertexCount; i++) {
				r_points.push_back(_relative_to_origin(b3TransformWorldPoint(p_body_transform, points[i]), p_origin));
			}
			r_proxy.radius = 0.0f;
		} break;

		default:
			return false;
	}

	r_proxy.points = r_points.ptr();
	r_proxy.count = r_points.size();
	return r_proxy.count > 0;
}

static void _append_distance_margin_contact(Box3DContactContext *p_context, b3ShapeId p_shape_id, b3WorldTransform p_body_transform) {
	if (p_context->contact_margin <= 0.0) {
		return;
	}

	Box3DBody3D *body = Box3DDirectSpaceState3D::_get_body(p_shape_id);
	ERR_FAIL_NULL(body);

	LocalVector<b3Vec3> candidate_points;
	b3ShapeProxy candidate_proxy;
	if (!_build_shape_proxy_relative_to_origin(p_shape_id, p_body_transform, p_context->query_shape->origin, candidate_points, candidate_proxy)) {
		return;
	}

	b3ShapeProxy query_proxy = p_context->query_shape->proxy;
	query_proxy.radius = MAX(0.0f, query_proxy.radius - (float)p_context->contact_margin);

	b3DistanceInput input = {};
	input.proxyA = query_proxy;
	input.proxyB = candidate_proxy;
	input.transform = b3Transform_identity;
	input.useRadii = true;

	b3SimplexCache cache = {};
	const b3DistanceOutput output = b3ShapeDistance(&input, &cache, nullptr, 0);
	if (output.distance <= CMP_EPSILON || output.distance > (float)p_context->contact_margin) {
		return;
	}

	const Vector3 normal = -to_godot(output.normal).normalized();
	Box3DQueryContact contact;
	contact.query_point = to_godot(b3OffsetPos(p_context->query_shape->origin, output.pointA));
	contact.collider_point = to_godot(b3OffsetPos(p_context->query_shape->origin, output.pointB));
	contact.normal = normal;
	contact.rid = body->get_rid();
	contact.collider_id = body->get_instance_id();
	contact.local_shape = p_context->local_shape;
	contact.shape = Box3DDirectSpaceState3D::_get_shape_index(p_shape_id);
	contact.linear_velocity = to_godot(b3Body_GetWorldPointVelocity(body->get_body_id(), to_box3d(contact.collider_point)));
	contact.angular_velocity = to_godot(b3Body_GetAngularVelocity(body->get_body_id()));
	contact.separation = (real_t)output.distance - p_context->contact_margin;
	p_context->contacts.push_back(contact);
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
	} else {
		_append_distance_margin_contact(p_context, p_shape_id, p_body_transform);
	}
}

static bool _contact_callback(b3ShapeId p_shape_id, void *p_context) {
	Box3DContactContext *ctx = static_cast<Box3DContactContext *>(p_context);
	if (!ctx->state->_can_query_shape(p_shape_id, ctx->parameters->exclude, ctx->parameters->collide_with_bodies, ctx->parameters->collide_with_areas)) {
		return true;
	}
	if (ctx->motion_body != nullptr) {
		if (b3Shape_IsSensor(p_shape_id)) {
			return true;
		}
		Box3DCollisionObject3D *object = Box3DDirectSpaceState3D::_get_object(p_shape_id);
		if (object == nullptr || object->get_type() != Box3DCollisionObject3D::TYPE_BODY) {
			return true;
		}
		Box3DBody3D *other_body = static_cast<Box3DBody3D *>(object);
		if (other_body == ctx->motion_body || ctx->motion_body->has_collision_exception(other_body->get_rid()) || other_body->has_collision_exception(ctx->motion_body->get_rid())) {
			return true;
		}
		if (ctx->exclude_objects != nullptr && ctx->exclude_objects->has(other_body->get_instance_id())) {
			return true;
		}
	}
	Box3DBody3D *body = Box3DDirectSpaceState3D::_get_body(p_shape_id);
	ERR_FAIL_NULL_V(body, false);

	_collide_query_with_shape(ctx, p_shape_id, b3Body_GetTransform(body->get_body_id()));
	return ctx->collect_all || ctx->contacts.size() < (uint32_t)ctx->result_max;
}

bool Box3DDirectSpaceState3D::collide_shape(const PS3DT::ShapeParameters &p_parameters, Vector3 *r_results, int p_result_max, int &r_result_count) {
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

	b3World_OverlapShape(space->get_world(), query_shape.origin, &query_shape.proxy, make_query_filter(p_parameters.collision_mask), _contact_callback, &ctx);
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

bool Box3DDirectSpaceState3D::rest_info(const PS3DT::ShapeParameters &p_parameters, PS3DT::ShapeRestInfo *r_info) {
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

	b3World_OverlapShape(space->get_world(), query_shape.origin, &query_shape.proxy, make_query_filter(p_parameters.collision_mask), _contact_callback, &ctx);
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

static bool _motion_can_hit_shape(const Box3DBody3D &p_body, b3ShapeId p_shape_id, const HashSet<RID> &p_excluded_bodies, const HashSet<ObjectID> &p_excluded_objects) {
	if (b3Shape_IsSensor(p_shape_id)) {
		return false;
	}
	Box3DCollisionObject3D *object = Box3DDirectSpaceState3D::_get_object(p_shape_id);
	if (object == nullptr || object->get_type() != Box3DCollisionObject3D::TYPE_BODY) {
		return false;
	}
	Box3DBody3D *other_body = static_cast<Box3DBody3D *>(object);
	if (other_body == &p_body || p_excluded_bodies.has(other_body->get_rid()) || p_excluded_objects.has(other_body->get_instance_id())) {
		return false;
	}
	if (p_body.has_collision_exception(other_body->get_rid()) || other_body->has_collision_exception(p_body.get_rid())) {
		return false;
	}
	return true;
}

struct Box3DMotionCandidateContext {
	const Box3DBody3D *body = nullptr;
	const HashSet<RID> *excluded_bodies = nullptr;
	const HashSet<ObjectID> *excluded_objects = nullptr;
	HashSet<Box3DBody3D *> candidate_bodies;
};

static bool _motion_candidate_callback(b3ShapeId p_shape_id, void *p_context) {
	Box3DMotionCandidateContext *ctx = static_cast<Box3DMotionCandidateContext *>(p_context);
	if (!_motion_can_hit_shape(*ctx->body, p_shape_id, *ctx->excluded_bodies, *ctx->excluded_objects)) {
		return true;
	}
	ctx->candidate_bodies.insert(Box3DDirectSpaceState3D::_get_body(p_shape_id));
	return true;
}

bool Box3DDirectSpaceState3D::body_test_motion(const Box3DBody3D &p_body, const PS3DT::MotionParameters &p_parameters, PS3DT::MotionResult *r_result) const {
	ERR_FAIL_NULL_V(space, false);
	ERR_FAIL_COND_V_MSG(space->is_stepping(), false, "body_test_motion (maybe from move_and_slide?) must not be called while the physics space is being stepped.");
	ERR_FAIL_COND_V(!p_body.in_space(), false);
	ERR_FAIL_COND_V(p_parameters.max_collisions < 0, false);

	if (r_result != nullptr) {
		*r_result = PS3DT::MotionResult();
	}

	const real_t margin = MAX((real_t)0.0001, p_parameters.margin);
	const real_t min_contact_depth = margin * (real_t)0.05;
	// Recovery acts on at most this much of a contact's reported depth per pass. Concave
	// one-sided triangle manifolds can report depth measured through the shape body for a
	// tangential graze against a buried coplanar seam face (two stacked map brushes): a
	// sub-millimeter graze reads as a large push along the face normal, which would drive
	// the body into the floor (and previously catapult it back out). Bounding the step keeps
	// recovery convergent for real penetrations while capping the damage a lying contact can
	// do to roughly the margin scale per call.
	const real_t max_recovery_depth = MAX(margin * (real_t)4.0, (real_t)0.01);
	const int max_collisions = MIN(p_parameters.max_collisions, PS3DT::MotionResult::MAX_COLLISIONS);

	HashSet<RID> excluded_bodies;
	for (const RID &excluded : p_parameters.exclude_bodies) {
		excluded_bodies.insert(excluded);
	}
	excluded_bodies.insert(p_body.get_rid());
	for (const RID &exception : p_body.get_collision_exceptions()) {
		excluded_bodies.insert(exception);
	}

	auto collect_contacts = [&](const Transform3D &p_body_transform, real_t p_contact_margin, LocalVector<Box3DQueryContact> &r_contacts) {
		for (uint32_t i = 0; i < p_body.slots.size(); i++) {
			const Box3DBody3D::ShapeSlot &slot = p_body.slots[i];
			if (slot.disabled || slot.shape == nullptr) {
				continue;
			}
			if (slot.shape->type == PS3DE::SHAPE_SEPARATION_RAY) {
				WARN_PRINT_ONCE("Box3D: body_test_motion collide_separation_ray is not implemented; separation ray shapes are skipped.");
				continue;
			}
			if (slot.shape->type == PS3DE::SHAPE_CONCAVE_POLYGON || slot.shape->type == PS3DE::SHAPE_HEIGHTMAP || slot.shape->type == PS3DE::SHAPE_WORLD_BOUNDARY) {
				WARN_PRINT_ONCE("Box3D: body_test_motion only supports convex moving body shapes; skipping non-convex body shape.");
				continue;
			}

			PS3DT::ShapeParameters shape_parameters;
			shape_parameters.shape_rid = slot.rid;
			shape_parameters.transform = p_body_transform * slot.xform;
			shape_parameters.margin = p_contact_margin;
			shape_parameters.exclude = excluded_bodies;
			shape_parameters.collision_mask = p_body.get_collision_mask();
			shape_parameters.collide_with_bodies = true;
			shape_parameters.collide_with_areas = false;

			QueryShape query_shape;
			if (!_build_query_shape(shape_parameters.shape_rid, shape_parameters.transform, query_shape, p_contact_margin)) {
				continue;
			}

			Box3DContactContext ctx;
			ctx.state = this;
			ctx.parameters = &shape_parameters;
			ctx.query_shape = &query_shape;
			ctx.motion_body = &p_body;
			ctx.exclude_objects = &p_parameters.exclude_objects;
			ctx.collect_all = true;
			ctx.local_shape = (int)i;
			ctx.contact_margin = p_contact_margin;

			b3World_OverlapShape(space->get_world(), query_shape.origin, &query_shape.proxy, make_query_filter(shape_parameters.collision_mask), _contact_callback, &ctx);
			for (const Box3DQueryContact &contact : ctx.contacts) {
				r_contacts.push_back(contact);
			}
		}
	};

	Transform3D transform = p_parameters.from;
	Vector3 recovery;
	bool recovered = false;

	for (int i = 0; i < 4; i++) {
		LocalVector<Box3DQueryContact> contacts;
		collect_contacts(transform, margin, contacts);
		if (contacts.is_empty()) {
			break;
		}

		Vector3 pass_recovery;
		for (const Box3DQueryContact &contact : contacts) {
			// Measure the depth still unresolved after the recovery already accumulated in
			// this pass. Coincident contacts are common (concave meshes report one manifold
			// per triangle, so a body resting on a flat floor gets dozens of identical
			// contacts); adding each one's full depth catapults the body many times the
			// actual penetration.
			const real_t depth = MIN(-contact.separation - contact.normal.dot(pass_recovery), max_recovery_depth);
			if (depth <= min_contact_depth) {
				continue;
			}
			pass_recovery += contact.normal * depth * (real_t)0.4;
		}
		if (pass_recovery.is_zero_approx()) {
			break;
		}

		recovered = true;
		recovery += pass_recovery;
		transform.origin += pass_recovery;
	}

	real_t safe_fraction = 1.0;
	real_t unsafe_fraction = 1.0;
	bool hit = false;
	bool has_zero_radius_cast_shape = false;
	real_t min_zero_radius_extent = 1e20;

	if (!p_parameters.motion.is_zero_approx()) {
		const b3Vec3 translation = to_box3d(p_parameters.motion);
		const Vector3 motion_direction = p_parameters.motion.normalized();
		for (uint32_t i = 0; i < p_body.slots.size(); i++) {
			const Box3DBody3D::ShapeSlot &slot = p_body.slots[i];
			if (slot.disabled || slot.shape == nullptr) {
				continue;
			}
			if (slot.shape->type == PS3DE::SHAPE_SEPARATION_RAY || slot.shape->type == PS3DE::SHAPE_CONCAVE_POLYGON || slot.shape->type == PS3DE::SHAPE_HEIGHTMAP || slot.shape->type == PS3DE::SHAPE_WORLD_BOUNDARY) {
				continue;
			}

			QueryShape query_shape;
			if (!_build_query_shape(slot.rid, transform * slot.xform, query_shape)) {
				continue;
			}

			if (query_shape.proxy.radius <= 0.0f) {
				has_zero_radius_cast_shape = true;
				// Track the shape's support extent along the motion; it bounds the
				// sampling step the fallback below may take without tunneling.
				real_t min_proj = 1e20;
				real_t max_proj = -1e20;
				for (int j = 0; j < query_shape.proxy.count; j++) {
					const real_t proj = motion_direction.dot(to_godot(query_shape.proxy.points[j]));
					min_proj = MIN(min_proj, proj);
					max_proj = MAX(max_proj, proj);
				}
				if (max_proj > min_proj) {
					min_zero_radius_extent = MIN(min_zero_radius_extent, max_proj - min_proj);
				}
				continue;
			}

			LocalVector<b3Vec3> swept_points;
			swept_points.resize(query_shape.proxy.count * 2);
			for (int j = 0; j < query_shape.proxy.count; j++) {
				const b3Vec3 p = query_shape.proxy.points[j];
				swept_points[j] = p;
				swept_points[j + query_shape.proxy.count] = b3Vec3{ p.x + translation.x, p.y + translation.y, p.z + translation.z };
			}

			b3ShapeProxy swept_proxy = query_shape.proxy;
			swept_proxy.points = swept_points.ptr();
			swept_proxy.count = swept_points.size();
			swept_proxy.radius = query_shape.proxy.radius + B3_LINEAR_SLOP;

			Box3DMotionCandidateContext candidate_ctx;
			candidate_ctx.body = &p_body;
			candidate_ctx.excluded_bodies = &excluded_bodies;
			candidate_ctx.excluded_objects = &p_parameters.exclude_objects;
			b3World_OverlapShape(space->get_world(), query_shape.origin, &swept_proxy, make_query_filter(p_body.get_collision_mask()), _motion_candidate_callback, &candidate_ctx);

			for (Box3DBody3D *candidate : candidate_ctx.candidate_bodies) {
				if (candidate == nullptr || !candidate->in_space()) {
					continue;
				}
				const bool can_encroach = query_shape.proxy.radius > 0.0f;
				b3BodyCastResult cast = b3Body_CastShape(candidate->get_body_id(), query_shape.origin, &query_shape.proxy, translation, make_query_filter(p_body.get_collision_mask()), (float)unsafe_fraction, can_encroach, b3Body_GetTransform(candidate->get_body_id()));
				if (!cast.hit || !_motion_can_hit_shape(p_body, cast.shapeId, excluded_bodies, p_parameters.exclude_objects)) {
					continue;
				}
				hit = true;
				unsafe_fraction = MIN(unsafe_fraction, (real_t)cast.fraction);
			}
		}

		if (has_zero_radius_cast_shape) {
			auto has_blocking_contact = [&](real_t p_fraction) {
				Transform3D cast_transform = transform;
				cast_transform.origin += p_parameters.motion * p_fraction;

				LocalVector<Box3DQueryContact> contacts;
				collect_contacts(cast_transform, margin, contacts);
				for (const Box3DQueryContact &contact : contacts) {
					if (motion_direction.dot(contact.normal) < -CMP_EPSILON && contact.separation <= 0.0) {
						return true;
					}
				}
				return false;
			};

			if (has_blocking_contact(0.0)) {
				hit = true;
				unsafe_fraction = 0.0;
			} else if (unsafe_fraction > CMP_EPSILON) {
				// March intermediate fractions so consecutive samples overlap along
				// the motion. Sampling only the endpoints lets a sweep longer than
				// the shape's own extent pass entirely through a thin obstacle
				// (e.g. a trimesh wall, which has no interior for the end pose to
				// touch) without either endpoint reporting a contact. The step is
				// 0.45x (not ~1x) the shape's extent because one-sided triangle
				// collision culls the contact once the shape's center crosses the
				// face plane — only the front half of the extent can detect, so a
				// sample must land inside that half-extent window.
				const real_t motion_length = p_parameters.motion.length();
				real_t step_fraction = unsafe_fraction;
				if (min_zero_radius_extent < 1e19 && motion_length > CMP_EPSILON) {
					step_fraction = (min_zero_radius_extent * (real_t)0.45) / motion_length;
				}
				// Also bound the step in absolute length so samples only ever probe
				// SHALLOW penetrations. Manifolds for a hull deeply overlapping a
				// one-sided triangle are unreliable near triangle boundaries (SAT can
				// emit a positive-separation manifold on a sideways axis for a pose
				// half a shape deep into a floor while straddling its edge), which made
				// long downward casts — e.g. CharacterBody3D::apply_floor_snap's
				// floor_snap_length probe — sail through the floor and flicker the
				// on-floor state every other tick.
				const real_t max_march_step_length = 0.06;
				if (motion_length > CMP_EPSILON) {
					step_fraction = MIN(step_fraction, max_march_step_length / motion_length);
				}
				// Cap the sample count; a shape tiny relative to the sweep can in
				// principle still tunnel past this cap.
				const int max_march_samples = 64;
				step_fraction = MAX(step_fraction, unsafe_fraction / (real_t)max_march_samples);

				real_t low = 0.0;
				real_t first_blocked = -1.0;
				real_t f = step_fraction;
				while (true) {
					const bool last = f >= unsafe_fraction;
					if (last) {
						f = unsafe_fraction;
					}
					if (has_blocking_contact(f)) {
						first_blocked = f;
						break;
					}
					low = f;
					if (last) {
						break;
					}
					f += step_fraction;
				}

				if (first_blocked >= 0.0) {
					hit = true;
					real_t high = first_blocked;
					for (int i = 0; i < 12; i++) {
						const real_t mid = (low + high) * (real_t)0.5;
						if (has_blocking_contact(mid)) {
							high = mid;
						} else {
							low = mid;
						}
					}
					unsafe_fraction = MIN(unsafe_fraction, high);
				}
			}
		}

		if (hit) {
			const real_t motion_length = p_parameters.motion.length();
			const real_t safe_backoff = motion_length > CMP_EPSILON ? (real_t)B3_LINEAR_SLOP / motion_length : 0.0;
			safe_fraction = MAX((real_t)0.0, unsafe_fraction - safe_backoff);
		}
	}

	LocalVector<Box3DQueryContact> collision_contacts;
	if (hit) {
		Transform3D collide_transform = transform;
		collide_transform.origin += p_parameters.motion * unsafe_fraction;
		collect_contacts(collide_transform, margin, collision_contacts);
	} else if (recovered && p_parameters.recovery_as_collision) {
		collect_contacts(transform, margin, collision_contacts);
	}

	if (!p_parameters.motion.is_zero_approx()) {
		const Vector3 direction = p_parameters.motion.normalized();
		for (int i = (int)collision_contacts.size() - 1; i >= 0; i--) {
			if (direction.dot(collision_contacts[i].normal) >= -CMP_EPSILON) {
				collision_contacts.remove_at(i);
			}
		}
	}
	// Keep contacts within half the margin of touching, not only strictly penetrating
	// ones. The marched-cast refine converges the unsafe pose onto the contact boundary,
	// where a strict separation<0 filter is a coin flip against requery float noise —
	// every contact of a real hit could be dropped, reading as collided=false with full
	// travel (CharacterBody3D::apply_floor_snap misses; on-floor flicker at rest poses).
	for (int i = (int)collision_contacts.size() - 1; i >= 0; i--) {
		if (collision_contacts[i].separation > margin * (real_t)0.5) {
			collision_contacts.remove_at(i);
		}
	}

	int collision_count = 0;
	if (r_result != nullptr) {
		while (collision_count < max_collisions && !collision_contacts.is_empty()) {
			int best = 0;
			real_t best_depth = -collision_contacts[0].separation;
			for (uint32_t i = 1; i < collision_contacts.size(); i++) {
				const real_t depth = -collision_contacts[i].separation;
				if (depth > best_depth) {
					best = i;
					best_depth = depth;
				}
			}

			const Box3DQueryContact contact = collision_contacts[best];
			collision_contacts.remove_at(best);

			PS3DT::MotionCollision &collision = r_result->collisions[collision_count++];
			collision.position = contact.collider_point;
			collision.normal = contact.normal;
			collision.collider_velocity = contact.linear_velocity;
			collision.collider_angular_velocity = contact.angular_velocity;
			collision.depth = MAX((real_t)0.0, -contact.separation);
			collision.local_shape = contact.local_shape;
			collision.collider_id = contact.collider_id;
			collision.collider = contact.rid;
			collision.collider_shape = contact.shape;
		}
		r_result->collision_count = collision_count;
	}

	const bool collided = r_result != nullptr ? collision_count > 0 : (hit || (recovered && p_parameters.recovery_as_collision));
	if (r_result != nullptr) {
		if (collided) {
			r_result->travel = recovery + p_parameters.motion * safe_fraction;
			r_result->remainder = p_parameters.motion - p_parameters.motion * safe_fraction;
			r_result->collision_safe_fraction = safe_fraction;
			r_result->collision_unsafe_fraction = unsafe_fraction;
			r_result->collision_depth = collision_count > 0 ? r_result->collisions[0].depth : 0.0;
		} else {
			r_result->travel = recovery + p_parameters.motion;
			r_result->remainder = Vector3();
			r_result->collision_safe_fraction = 1.0;
			r_result->collision_unsafe_fraction = 1.0;
			r_result->collision_depth = 0.0;
			r_result->collision_count = 0;
		}
	}

	return collided;
}
