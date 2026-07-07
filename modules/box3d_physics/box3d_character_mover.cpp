/**************************************************************************/
/*  box3d_character_mover.cpp                                             */
/**************************************************************************/

#include "box3d_character_mover.h"

#include "box3d_body_3d.h"
#include "box3d_conversions.h"
#include "box3d_direct_space_state_3d.h"
#include "box3d_physics_server_3d.h"
#include "box3d_space_3d.h"

#include "core/object/class_db.h"

#include "box3d/collision.h"

#include <float.h>

static constexpr uint64_t BOX3D_MOVER_QUERY_FILTER_BIT = UINT64_C(1) << 63;

struct Box3DMoverFilterContext {
	const HashSet<RID> *exclusions = nullptr;
};

static bool _mover_filter_callback(b3ShapeId p_shape_id, void *p_context) {
	Box3DMoverFilterContext *ctx = static_cast<Box3DMoverFilterContext *>(p_context);
	Box3DBody3D *body = Box3DDirectSpaceState3D::_get_body(p_shape_id);
	if (body == nullptr) {
		return false;
	}
	return ctx->exclusions == nullptr || !ctx->exclusions->has(body->get_rid());
}

struct Box3DMoverPlaneContext {
	const HashSet<RID> *exclusions = nullptr;
	Array planes;
	Vector3 origin;
};

static Dictionary _plane_to_dictionary(b3ShapeId p_shape_id, const b3PlaneResult &p_plane, const Vector3 &p_origin) {
	Dictionary d;
	d["normal"] = to_godot(p_plane.plane.normal);
	d["offset"] = (real_t)p_plane.plane.offset;
	d["local_point"] = to_godot(p_plane.point);
	d["point"] = p_origin + to_godot(p_plane.point);
	d["push_limit"] = (real_t)FLT_MAX;
	d["push"] = 0.0;
	d["clip_velocity"] = true;

	Box3DBody3D *body = Box3DDirectSpaceState3D::_get_body(p_shape_id);
	if (body != nullptr) {
		d["rid"] = body->get_rid();
		d["shape"] = Box3DDirectSpaceState3D::_get_shape_index(p_shape_id);
	}

	return d;
}

static bool _mover_plane_callback(b3ShapeId p_shape_id, const b3PlaneResult *p_planes, int p_plane_count, void *p_context) {
	Box3DMoverPlaneContext *ctx = static_cast<Box3DMoverPlaneContext *>(p_context);
	Box3DBody3D *body = Box3DDirectSpaceState3D::_get_body(p_shape_id);
	if (body == nullptr || (ctx->exclusions != nullptr && ctx->exclusions->has(body->get_rid()))) {
		return true;
	}

	for (int i = 0; i < p_plane_count; i++) {
		ctx->planes.push_back(_plane_to_dictionary(p_shape_id, p_planes[i], ctx->origin));
	}
	return true;
}

static b3CollisionPlane _dictionary_to_collision_plane(const Dictionary &p_plane) {
	const Vector3 normal = p_plane.has("normal") ? (Vector3)p_plane["normal"] : Vector3(0, 1, 0);
	b3CollisionPlane plane = {};
	plane.plane.normal = to_box3d(normal);
	plane.plane.offset = p_plane.has("offset") ? (float)(real_t)p_plane["offset"] : 0.0f;
	plane.pushLimit = p_plane.has("push_limit") ? (float)(real_t)p_plane["push_limit"] : FLT_MAX;
	plane.push = p_plane.has("push") ? (float)(real_t)p_plane["push"] : 0.0f;
	plane.clipVelocity = p_plane.has("clip_velocity") ? (bool)p_plane["clip_velocity"] : true;
	return plane;
}

static Dictionary _collision_plane_to_dictionary(const b3CollisionPlane &p_plane, const Dictionary &p_source) {
	Dictionary d = p_source;
	d["normal"] = to_godot(p_plane.plane.normal);
	d["offset"] = (real_t)p_plane.plane.offset;
	d["push_limit"] = (real_t)p_plane.pushLimit;
	d["push"] = (real_t)p_plane.push;
	d["clip_velocity"] = p_plane.clipVelocity;
	return d;
}

void Box3DCharacterMover::_bind_methods() {
	ClassDB::bind_method(D_METHOD("setup", "space"), &Box3DCharacterMover::setup);
	ClassDB::bind_method(D_METHOD("set_capsule", "height", "radius"), &Box3DCharacterMover::set_capsule);
	ClassDB::bind_method(D_METHOD("set_collision_mask", "mask"), &Box3DCharacterMover::set_collision_mask);
	ClassDB::bind_method(D_METHOD("get_collision_mask"), &Box3DCharacterMover::get_collision_mask);
	ClassDB::bind_method(D_METHOD("set_exclusions", "bodies"), &Box3DCharacterMover::set_exclusions);
	ClassDB::bind_method(D_METHOD("cast_motion", "position", "translation"), &Box3DCharacterMover::cast_motion);
	ClassDB::bind_method(D_METHOD("collide", "position"), &Box3DCharacterMover::collide);
	ClassDB::bind_method(D_METHOD("solve_planes", "target_delta", "planes"), &Box3DCharacterMover::solve_planes);
	ClassDB::bind_method(D_METHOD("clip_velocity", "velocity", "planes"), &Box3DCharacterMover::clip_velocity);
	ClassDB::bind_method(D_METHOD("move", "position", "velocity", "delta"), &Box3DCharacterMover::move);

	ADD_PROPERTY(PropertyInfo(Variant::INT, "collision_mask", PROPERTY_HINT_LAYERS_3D_PHYSICS), "set_collision_mask", "get_collision_mask");
}

Box3DCharacterMover::Box3DCharacterMover() {
	set_capsule(1.8f, 0.35f);
}

b3QueryFilter Box3DCharacterMover::_make_filter() const {
	b3QueryFilter filter = b3DefaultQueryFilter();
	filter.categoryBits = BOX3D_MOVER_QUERY_FILTER_BIT;
	filter.maskBits = (uint64_t)collision_mask;
	return filter;
}

bool Box3DCharacterMover::_can_query() const {
	Box3DPhysicsServer3D *server = Box3DPhysicsServer3D::get_singleton();
	return server != nullptr && server->can_access_space(space);
}

void Box3DCharacterMover::setup(RID p_space) {
	Box3DPhysicsServer3D *server = Box3DPhysicsServer3D::get_singleton();
	ERR_FAIL_NULL_MSG(server, "Box3DCharacterMover requires the Box3D physics server to be active.");

	space = server->get_space(p_space);
	ERR_FAIL_NULL_MSG(space, "Box3DCharacterMover setup received an invalid Box3D space RID.");
	space_rid = p_space;
}

void Box3DCharacterMover::set_capsule(float p_height, float p_radius) {
	ERR_FAIL_COND_MSG(p_height <= 0.0f, "Box3DCharacterMover capsule height must be positive.");
	ERR_FAIL_COND_MSG(p_radius <= 0.0f, "Box3DCharacterMover capsule radius must be positive.");

	const float clamped_height = MAX(p_height, p_radius * 2.0f);
	capsule.center1 = b3Vec3{ 0.0f, p_radius, 0.0f };
	capsule.center2 = b3Vec3{ 0.0f, clamped_height - p_radius, 0.0f };
	capsule.radius = p_radius;
}

void Box3DCharacterMover::set_collision_mask(uint32_t p_mask) {
	collision_mask = p_mask;
}

void Box3DCharacterMover::set_exclusions(const TypedArray<RID> &p_bodies) {
	exclusions.clear();
	for (int i = 0; i < p_bodies.size(); i++) {
		exclusions.insert(p_bodies[i]);
	}
}

float Box3DCharacterMover::cast_motion(const Vector3 &p_position, const Vector3 &p_translation) const {
	ERR_FAIL_COND_V_MSG(!_can_query(), 1.0f, "Box3DCharacterMover cannot query the space right now.");

	Box3DMoverFilterContext ctx;
	ctx.exclusions = &exclusions;
	return b3World_CastMover(space->get_world(), to_box3d(p_position), &capsule, to_box3d(p_translation), _make_filter(), _mover_filter_callback, &ctx);
}

Array Box3DCharacterMover::_collide_internal(const Vector3 &p_position) const {
	Array planes;
	ERR_FAIL_COND_V_MSG(!_can_query(), planes, "Box3DCharacterMover cannot query the space right now.");

	Box3DMoverPlaneContext ctx;
	ctx.exclusions = &exclusions;
	ctx.origin = p_position;
	b3World_CollideMover(space->get_world(), to_box3d(p_position), &capsule, _make_filter(), _mover_plane_callback, &ctx);
	return ctx.planes;
}

Array Box3DCharacterMover::collide(const Vector3 &p_position) const {
	return _collide_internal(p_position);
}

Dictionary Box3DCharacterMover::solve_planes(const Vector3 &p_target_delta, const Array &p_planes) const {
	LocalVector<b3CollisionPlane> planes;
	planes.resize(p_planes.size());
	for (int i = 0; i < p_planes.size(); i++) {
		planes[i] = _dictionary_to_collision_plane((Dictionary)p_planes[i]);
	}

	const b3PlaneSolverResult result = b3SolvePlanes(to_box3d(p_target_delta), planes.size() > 0 ? planes.ptr() : nullptr, planes.size());

	Array output_planes;
	for (int i = 0; i < p_planes.size(); i++) {
		output_planes.push_back(_collision_plane_to_dictionary(planes[i], (Dictionary)p_planes[i]));
	}

	Dictionary d;
	d["delta"] = to_godot(result.delta);
	d["iterations"] = result.iterationCount;
	d["planes"] = output_planes;
	return d;
}

Vector3 Box3DCharacterMover::clip_velocity(const Vector3 &p_velocity, const Array &p_planes) const {
	LocalVector<b3CollisionPlane> planes;
	planes.resize(p_planes.size());
	for (int i = 0; i < p_planes.size(); i++) {
		planes[i] = _dictionary_to_collision_plane((Dictionary)p_planes[i]);
	}
	return to_godot(b3ClipVector(to_box3d(p_velocity), planes.size() > 0 ? planes.ptr() : nullptr, planes.size()));
}

Dictionary Box3DCharacterMover::move(const Vector3 &p_position, const Vector3 &p_velocity, float p_delta) const {
	ERR_FAIL_COND_V_MSG(!_can_query(), Dictionary(), "Box3DCharacterMover cannot query the space right now.");

	const Vector3 target_position = p_position + p_velocity * p_delta;
	Vector3 position = p_position;
	Array solved_planes;
	int total_iterations = 0;
	float last_fraction = 1.0f;

	for (int i = 0; i < 5; i++) {
		const Array planes = _collide_internal(position);
		const Dictionary solve = solve_planes(target_position - position, planes);
		const Vector3 delta = solve["delta"];
		solved_planes = solve["planes"];
		total_iterations += (int)solve["iterations"];

		if (delta.length_squared() < 0.0001f) {
			break;
		}

		last_fraction = cast_motion(position, delta);
		const Vector3 applied_delta = delta * last_fraction;
		position += applied_delta;

		if (applied_delta.length_squared() < 0.0001f) {
			break;
		}
	}

	const Array final_planes = _collide_internal(position);
	const Dictionary final_solve = solve_planes(Vector3(), final_planes);
	const Vector3 solve_delta = final_solve["delta"];
	solved_planes = final_solve["planes"];
	total_iterations += (int)final_solve["iterations"];
	position += solve_delta;

	const Vector3 clipped_velocity = clip_velocity(p_velocity, solved_planes);

	Vector3 floor_normal;
	bool on_floor = false;
	bool on_wall = false;
	for (int i = 0; i < solved_planes.size(); i++) {
		Dictionary plane = solved_planes[i];
		const Vector3 normal = plane["normal"];
		if (normal.y > 0.7) {
			on_floor = true;
			if (normal.y > floor_normal.y) {
				floor_normal = normal;
			}
		} else {
			on_wall = true;
		}
	}

	Dictionary result;
	result["position"] = position;
	result["velocity"] = clipped_velocity;
	result["on_floor"] = on_floor;
	result["on_wall"] = on_wall;
	result["floor_normal"] = floor_normal;
	result["planes"] = solved_planes;
	result["cast_fraction"] = last_fraction;
	result["iterations"] = total_iterations;
	return result;
}
