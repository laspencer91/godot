/**************************************************************************/
/*  box3d_character_mover.cpp                                             */
/**************************************************************************/

#include "box3d_character_mover.h"

#include "box3d_body_3d.h"
#include "box3d_conversions.h"
#include "box3d_direct_space_state_3d.h"
#include "box3d_physics_server_3d.h"
#include "box3d_space_3d.h"
#include "box3d_surface_materials.h"

#include "core/object/class_db.h"

#include "box3d/collision.h"

#include <float.h>

static constexpr uint64_t BOX3D_MOVER_QUERY_FILTER_BIT = UINT64_C(1) << 63;
static constexpr int BOX3D_MOVER_FOOTPRINT_SIDES = 24;
static constexpr int BOX3D_MOVER_FOOTPRINT_POINT_COUNT = BOX3D_MOVER_FOOTPRINT_SIDES * 2;
static constexpr real_t BOX3D_MOVER_FOOTPRINT_PROBE_UP = 0.02;
static constexpr real_t BOX3D_MOVER_FOOTPRINT_PROBE_LENGTH = 0.04;

// Step probing tolerances shared by _try_step_down and the footprint step-down veto: the veto must
// probe the same envelope _try_step_down accepts, or the two disagree about the same drop.
static constexpr real_t BOX3D_MOVER_STEP_PROBE_UP = 0.05;
static constexpr real_t BOX3D_MOVER_STEP_HEIGHT_TOLERANCE = 0.01;
static constexpr real_t BOX3D_MOVER_STEP_EDGE_ROLL_ALLOWANCE = 0.10;

// Minimum upward normal component for a contact to count as (partial) support — e.g. a capsule
// hemisphere riding a step lip — as opposed to a pure wall.
static constexpr real_t BOX3D_MOVER_SUPPORT_MIN_NY = 0.05;

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

struct Box3DMoverFloorProbeContext {
	const HashSet<RID> *exclusions = nullptr;
	RID target_rid;
	int target_shape = -1;
	int material_id = 0;
	bool hit = false;
};

static void _add_floor_material_fields(Dictionary &r_result, int p_material_id) {
	r_result["floor_material_id"] = p_material_id;

	Box3DPhysics *box3d_physics = Box3DPhysics::get_singleton();
	// `floor_material_id` is the durable handle and `floor_material` carries every field,
	// including the name, so no separate name key. See _box3d_add_material_fields.
	if (box3d_physics == nullptr || p_material_id <= 0) {
		r_result["floor_material"] = Ref<Box3DSurfaceMaterial>();
		return;
	}

	r_result["floor_material"] = box3d_physics->surface(p_material_id);
}

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

// Box3D reports material ids as uint64_t; the mover result exposes them as int. Clamp
// out-of-range ids to 0 (the default material) rather than truncating to a negative id.
static int _mover_user_material_id(uint64_t p_user_material_id) {
	return p_user_material_id <= (uint64_t)INT_MAX ? (int)p_user_material_id : 0;
}

struct Box3DMoverRayProbeContext {
	const HashSet<RID> *exclusions = nullptr;
	Vector3 point;
	Vector3 normal;
	float fraction = 1.0f;
	int material_id = 0;
	bool hit = false;
};

struct Box3DMoverDirectionalShapeProbeContext {
	const HashSet<RID> *exclusions = nullptr;
	real_t min_normal_y = -FLT_MAX;
	real_t max_normal_y = FLT_MAX;
	Vector3 normal;
	int material_id = 0;
	bool hit = false;
};

static void _make_footprint_proxy(real_t p_radius, b3Vec3 *r_points, b3ShapeProxy &r_proxy) {
	struct UnitCircle {
		real_t x[BOX3D_MOVER_FOOTPRINT_SIDES];
		real_t z[BOX3D_MOVER_FOOTPRINT_SIDES];
		UnitCircle() {
			for (int i = 0; i < BOX3D_MOVER_FOOTPRINT_SIDES; i++) {
				const real_t angle = Math::TAU * (real_t)i / (real_t)BOX3D_MOVER_FOOTPRINT_SIDES;
				x[i] = Math::cos(angle);
				z[i] = Math::sin(angle);
			}
		}
	};
	static const UnitCircle circle;

	const real_t half_thickness = (real_t)B3_LINEAR_SLOP * 0.5;
	for (int i = 0; i < BOX3D_MOVER_FOOTPRINT_SIDES; i++) {
		const real_t x = circle.x[i] * p_radius;
		const real_t z = circle.z[i] * p_radius;
		r_points[i] = to_box3d(Vector3(x, -half_thickness, z));
		r_points[i + BOX3D_MOVER_FOOTPRINT_SIDES] = to_box3d(Vector3(x, half_thickness, z));
	}
	r_proxy.points = r_points;
	r_proxy.count = BOX3D_MOVER_FOOTPRINT_POINT_COUNT;
	r_proxy.radius = 0.0f;
}

static float _mover_ray_probe_callback(b3ShapeId p_shape_id, b3Pos p_point, b3Vec3 p_normal, float p_fraction, uint64_t p_user_material_id, int p_triangle_index, int p_child_index, void *p_context) {
	Box3DMoverRayProbeContext *ctx = static_cast<Box3DMoverRayProbeContext *>(p_context);
	Box3DBody3D *body = Box3DDirectSpaceState3D::_get_body(p_shape_id);
	if (body == nullptr || (ctx->exclusions != nullptr && ctx->exclusions->has(body->get_rid()))) {
		return -1.0f;
	}

	ctx->hit = true;
	ctx->point = to_godot(p_point);
	ctx->normal = to_godot(p_normal);
	ctx->fraction = p_fraction;
	ctx->material_id = _mover_user_material_id(p_user_material_id);
	return p_fraction;
}

static float _mover_directional_shape_probe_callback(b3ShapeId p_shape_id, b3Pos p_point, b3Vec3 p_normal, float p_fraction, uint64_t p_user_material_id, int p_triangle_index, int p_child_index, void *p_context) {
	Box3DMoverDirectionalShapeProbeContext *ctx = static_cast<Box3DMoverDirectionalShapeProbeContext *>(p_context);
	Box3DBody3D *body = Box3DDirectSpaceState3D::_get_body(p_shape_id);
	if (body == nullptr || (ctx->exclusions != nullptr && ctx->exclusions->has(body->get_rid()))) {
		return -1.0f;
	}

	const Vector3 normal = to_godot(p_normal);
	if (normal.y < ctx->min_normal_y || normal.y > ctx->max_normal_y) {
		return -1.0f;
	}

	ctx->hit = true;
	ctx->normal = normal;
	ctx->material_id = _mover_user_material_id(p_user_material_id);
	return p_fraction;
}

static float _mover_floor_probe_callback(b3ShapeId p_shape_id, b3Pos p_point, b3Vec3 p_normal, float p_fraction, uint64_t p_user_material_id, int p_triangle_index, int p_child_index, void *p_context) {
	Box3DMoverFloorProbeContext *ctx = static_cast<Box3DMoverFloorProbeContext *>(p_context);
	Box3DBody3D *body = Box3DDirectSpaceState3D::_get_body(p_shape_id);
	if (body == nullptr || (ctx->exclusions != nullptr && ctx->exclusions->has(body->get_rid()))) {
		return -1.0f;
	}
	if (ctx->target_rid.is_valid() && body->get_rid() != ctx->target_rid) {
		return -1.0f;
	}
	if (ctx->target_shape >= 0 && Box3DDirectSpaceState3D::_get_shape_index(p_shape_id) != ctx->target_shape) {
		return -1.0f;
	}

	ctx->material_id = _mover_user_material_id(p_user_material_id);
	ctx->hit = true;
	return p_fraction;
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
	ClassDB::bind_method(D_METHOD("set_floor_max_angle", "radians"), &Box3DCharacterMover::set_floor_max_angle);
	ClassDB::bind_method(D_METHOD("get_floor_max_angle"), &Box3DCharacterMover::get_floor_max_angle);
	ClassDB::bind_method(D_METHOD("set_push_strength", "strength"), &Box3DCharacterMover::set_push_strength);
	ClassDB::bind_method(D_METHOD("get_push_strength"), &Box3DCharacterMover::get_push_strength);
	ClassDB::bind_method(D_METHOD("set_step_height", "height"), &Box3DCharacterMover::set_step_height);
	ClassDB::bind_method(D_METHOD("get_step_height"), &Box3DCharacterMover::get_step_height);
	ClassDB::bind_method(D_METHOD("set_body_footprint_radius", "radius"), &Box3DCharacterMover::set_body_footprint_radius);
	ClassDB::bind_method(D_METHOD("get_body_footprint_radius"), &Box3DCharacterMover::get_body_footprint_radius);
	ClassDB::bind_method(D_METHOD("set_exclusions", "bodies"), &Box3DCharacterMover::set_exclusions);
	ClassDB::bind_method(D_METHOD("cast_motion", "position", "translation"), &Box3DCharacterMover::cast_motion);
	ClassDB::bind_method(D_METHOD("collide", "position"), &Box3DCharacterMover::collide);
	ClassDB::bind_method(D_METHOD("has_head_clearance", "position", "current_height", "target_height"), &Box3DCharacterMover::has_head_clearance);
	ClassDB::bind_method(D_METHOD("solve_planes", "target_delta", "planes"), &Box3DCharacterMover::solve_planes);
	ClassDB::bind_method(D_METHOD("clip_velocity", "velocity", "planes"), &Box3DCharacterMover::clip_velocity);
	ClassDB::bind_method(D_METHOD("move", "position", "velocity", "delta", "was_grounded"), &Box3DCharacterMover::move, DEFVAL(false));

	ADD_PROPERTY(PropertyInfo(Variant::INT, "collision_mask", PROPERTY_HINT_LAYERS_3D_PHYSICS), "set_collision_mask", "get_collision_mask");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "floor_max_angle", PROPERTY_HINT_RANGE, "0,1.5707963267949,0.001,radians"), "set_floor_max_angle", "get_floor_max_angle");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "push_strength", PROPERTY_HINT_RANGE, "0,10,0.01,or_greater"), "set_push_strength", "get_push_strength");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "step_height", PROPERTY_HINT_RANGE, "0,1,0.01,or_greater,suffix:m"), "set_step_height", "get_step_height");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "body_footprint_radius", PROPERTY_HINT_RANGE, "0,1,0.01,or_greater,suffix:m"), "set_body_footprint_radius", "get_body_footprint_radius");
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
	return server != nullptr && server->can_access_space(_get_space());
}

Box3DSpace3D *Box3DCharacterMover::_get_space() const {
	Box3DPhysicsServer3D *server = Box3DPhysicsServer3D::get_singleton();
	if (server == nullptr) {
		return nullptr;
	}
	return server->get_space(space_rid);
}

void Box3DCharacterMover::setup(RID p_space) {
	Box3DPhysicsServer3D *server = Box3DPhysicsServer3D::get_singleton();
	ERR_FAIL_NULL_MSG(server, "Box3DCharacterMover requires the Box3D physics server to be active.");

	ERR_FAIL_NULL_MSG(server->get_space(p_space), "Box3DCharacterMover setup received an invalid Box3D space RID.");
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

void Box3DCharacterMover::set_floor_max_angle(real_t p_angle) {
	floor_max_angle = CLAMP(p_angle, 0.0, 1.5707963267948966);
}

void Box3DCharacterMover::set_push_strength(real_t p_strength) {
	push_strength = MAX((real_t)0.0, p_strength);
}

void Box3DCharacterMover::set_step_height(real_t p_height) {
	step_height = MAX((real_t)0.0, p_height);
}

void Box3DCharacterMover::set_body_footprint_radius(real_t p_radius) {
	body_footprint_radius = MAX((real_t)0.0, p_radius);
}

void Box3DCharacterMover::set_exclusions(const TypedArray<RID> &p_bodies) {
	exclusions.clear();
	for (int i = 0; i < p_bodies.size(); i++) {
		exclusions.insert(p_bodies[i]);
	}
}

float Box3DCharacterMover::cast_motion(const Vector3 &p_position, const Vector3 &p_translation) const {
	Box3DSpace3D *query_space = _get_space();
	ERR_FAIL_COND_V_MSG(!_can_query(), 0.0f, "Box3DCharacterMover cannot query the space right now.");

	Box3DMoverFilterContext ctx;
	ctx.exclusions = &exclusions;
	return b3World_CastMover(query_space->get_world(), to_box3d(p_position), &capsule, to_box3d(p_translation), _make_filter(), _mover_filter_callback, &ctx);
}

Array Box3DCharacterMover::_collide_internal(const Vector3 &p_position) const {
	Array planes;
	Box3DSpace3D *query_space = _get_space();
	ERR_FAIL_COND_V_MSG(!_can_query(), planes, "Box3DCharacterMover cannot query the space right now.");

	Box3DMoverPlaneContext ctx;
	ctx.exclusions = &exclusions;
	ctx.origin = p_position;
	b3World_CollideMover(query_space->get_world(), to_box3d(p_position), &capsule, _make_filter(), _mover_plane_callback, &ctx);
	return ctx.planes;
}

Array Box3DCharacterMover::collide(const Vector3 &p_position) const {
	return _collide_internal(p_position);
}

bool Box3DCharacterMover::has_head_clearance(const Vector3 &p_position, real_t p_current_height, real_t p_target_height) const {
	ERR_FAIL_COND_V_MSG(!_can_query(), false, "Box3DCharacterMover cannot query the space right now.");
	if (body_footprint_radius <= 0.0 || p_target_height <= p_current_height) {
		return true;
	}

	// Start one footprint radius below the current apex. Geometry below that is already covered by
	// the live capsule's full-width body; beginning here also makes an overhang already cutting through
	// the rounded shoulder visible to the upward cast rather than relying on an initial-overlap report.
	const real_t start_height = MAX((real_t)0.0, p_current_height - MIN(body_footprint_radius, (real_t)capsule.radius));
	Vector3 normal;
	int material_id = 0;
	return !_cast_footprint(p_position + Vector3(0.0, start_height, 0.0),
			Vector3(0.0, p_target_height - start_height, 0.0), -FLT_MAX, -0.1, normal, material_id);
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

bool Box3DCharacterMover::_probe_walkable(const Vector3 &p_from, real_t p_length, Vector3 &r_normal, real_t &r_hit_y, int &r_material_id) const {
	Box3DSpace3D *query_space = _get_space();
	if (query_space == nullptr) {
		return false;
	}

	Box3DMoverRayProbeContext ctx;
	ctx.exclusions = &exclusions;
	b3World_CastRay(query_space->get_world(), to_box3d(p_from), to_box3d(Vector3(0.0, -p_length, 0.0)), _make_filter(), _mover_ray_probe_callback, &ctx);
	if (!ctx.hit) {
		return false;
	}

	r_normal = ctx.normal;
	r_hit_y = p_from.y - p_length * ctx.fraction;
	r_material_id = ctx.material_id;
	return true;
}

// Sweep the matched flat footprint disc, reporting only hits whose normal.y lies inside
// [p_min_normal_y, p_max_normal_y].
bool Box3DCharacterMover::_cast_footprint(const Vector3 &p_from, const Vector3 &p_translation, real_t p_min_normal_y, real_t p_max_normal_y, Vector3 &r_normal, int &r_material_id) const {
	Box3DSpace3D *query_space = _get_space();
	const real_t radius = MIN(body_footprint_radius, (real_t)capsule.radius);
	if (query_space == nullptr || radius <= 0.0) {
		return false;
	}

	b3Vec3 points[BOX3D_MOVER_FOOTPRINT_POINT_COUNT];
	b3ShapeProxy proxy = {};
	_make_footprint_proxy(radius, points, proxy);

	Box3DMoverDirectionalShapeProbeContext ctx;
	ctx.exclusions = &exclusions;
	ctx.min_normal_y = p_min_normal_y;
	ctx.max_normal_y = p_max_normal_y;
	b3World_CastShape(query_space->get_world(), to_box3d(p_from), &proxy, to_box3d(p_translation),
			_make_filter(), _mover_directional_shape_probe_callback, &ctx);
	if (!ctx.hit) {
		return false;
	}

	r_normal = ctx.normal;
	r_material_id = ctx.material_id;
	return true;
}

bool Box3DCharacterMover::_probe_body_footprint(const Vector3 &p_feet, Vector3 &r_normal, int &r_material_id) const {
	return _cast_footprint(p_feet + Vector3(0.0, BOX3D_MOVER_FOOTPRINT_PROBE_UP, 0.0),
			Vector3(0.0, -BOX3D_MOVER_FOOTPRINT_PROBE_LENGTH, 0.0),
			Math::cos(floor_max_angle), FLT_MAX, r_normal, r_material_id);
}

// Capsule-safe ground classification. A capsule climbing a step rides the lip on its bottom
// hemisphere, so the contact normal is tilted and fails the floor_max_angle test even though the
// character is standing on walkable ground. Classify support with downward ray probes instead of
// the contact normal: first under the capsule axis, then just past the support contact point (the
// stair tread). Slopes steeper than floor_max_angle still reject because the probes hit the sloped
// surface itself; thin lips (fence rails) reject because the tread probe overshoots them.
bool Box3DCharacterMover::_ground_probe(const Vector3 &p_feet, const Array &p_planes, Vector3 &r_normal, int &r_material_id) const {
	const real_t PROBE_UP = 0.05;
	const real_t CENTER_PROBE_DEPTH = 0.12;
	const real_t TREAD_AHEAD = 0.04;
	const real_t floor_threshold = Math::cos(floor_max_angle);

	Vector3 support_point;
	bool has_support = false;
	real_t best_support_y = BOX3D_MOVER_SUPPORT_MIN_NY;
	for (int i = 0; i < p_planes.size(); i++) {
		Dictionary plane = p_planes[i];
		const Vector3 normal = plane["normal"];
		if (normal.y > best_support_y && plane.has("point")) {
			best_support_y = normal.y;
			support_point = plane["point"];
			has_support = true;
		}
	}
	if (!has_support) {
		return false;
	}

	// The step_height cap binds against the support contact, and must be checked before any
	// acceptance: a capsule can wedge onto the lip of an over-tall obstacle (a 0.3 m fence with
	// step_height 0.25) while its axis is still over low walkable ground, so the center probe
	// alone would let it inch over height limits.
	if (support_point.y - p_feet.y > step_height + 0.01) {
		return false;
	}

	Vector3 probe_normal;
	real_t hit_y = 0.0;
	int material_id = 0;
	if (_probe_walkable(p_feet + Vector3(0.0, PROBE_UP, 0.0), PROBE_UP + CENTER_PROBE_DEPTH, probe_normal, hit_y, material_id) && probe_normal.y > floor_threshold) {
		r_normal = probe_normal;
		r_material_id = material_id;
		return true;
	}

	Vector3 lip_direction = support_point - p_feet;
	lip_direction.y = 0.0;
	if (lip_direction.length_squared() < 1e-8) {
		return false;
	}
	lip_direction = lip_direction.normalized();

	const Vector3 tread_from = support_point + lip_direction * TREAD_AHEAD + Vector3(0.0, PROBE_UP, 0.0);
	if (_probe_walkable(tread_from, PROBE_UP + MAX(step_height, CENTER_PROBE_DEPTH), probe_normal, hit_y, material_id) && probe_normal.y > floor_threshold && hit_y - p_feet.y <= step_height + 0.01) {
		r_normal = probe_normal;
		r_material_id = material_id;
		return true;
	}
	return false;
}

// Verify that the opposing capsule contact comes from a riser rather than a walkable ramp. The
// capsule plane normal tilts upward while its hemisphere rides a sharp lip, so that normal alone
// eventually looks walkable. A low forward ray sees the actual vertical face throughout the climb.
bool Box3DCharacterMover::_has_step_obstruction(const Vector3 &p_start, const Vector3 &p_horizontal) const {
	Box3DSpace3D *query_space = _get_space();
	if (query_space == nullptr || p_horizontal.length_squared() < 1e-8) {
		return false;
	}

	const Vector3 direction = p_horizontal.normalized();
	const Vector3 lateral(-direction.z, 0.0, direction.x);
	const real_t probe_up = MIN((real_t)0.01, step_height * 0.5);
	const real_t probe_length = p_horizontal.length() + capsule.radius + 0.01;
	const real_t floor_threshold = Math::cos(floor_max_angle);
	const real_t TREAD_AHEAD = 0.04;
	const real_t TREAD_PROBE_UP = 0.05;
	// A center ray alone misses an obstruction first touched by the capsule flank during diagonal
	// movement. Sample most of the footprint so step eligibility does not depend on approach angle.
	const real_t lateral_offset = capsule.radius * 0.8;
	const real_t offsets[] = { 0.0, -lateral_offset, lateral_offset };
	for (real_t offset : offsets) {
		Box3DMoverRayProbeContext ctx;
		ctx.exclusions = &exclusions;
		const Vector3 from = p_start + lateral * offset + Vector3(0.0, probe_up, 0.0);
		b3World_CastRay(query_space->get_world(), to_box3d(from), to_box3d(direction * probe_length),
				_make_filter(), _mover_ray_probe_callback, &ctx);
		if (!ctx.hit) {
			continue;
		}

		const Vector3 horizontal_normal(ctx.normal.x, 0.0, ctx.normal.z);
		if (ctx.normal.y > floor_threshold || horizontal_normal.dot(direction) >= -0.01) {
			continue;
		}

		// Confirm a walkable tread within step_height just behind the low obstruction. Besides enforcing
		// the height limit here, this keeps steep ramps and ordinary walls from receiving stair response.
		Vector3 tread_from = ctx.point + direction * TREAD_AHEAD;
		tread_from.y = p_start.y + step_height + TREAD_PROBE_UP;
		const real_t tread_probe_length = step_height + TREAD_PROBE_UP * 2.0;
		Box3DMoverRayProbeContext tread_ctx;
		tread_ctx.exclusions = &exclusions;
		b3World_CastRay(query_space->get_world(), to_box3d(tread_from),
				to_box3d(Vector3(0.0, -tread_probe_length, 0.0)), _make_filter(),
				_mover_ray_probe_callback, &tread_ctx);
		if (tread_ctx.hit && tread_ctx.normal.y > floor_threshold) {
			const real_t tread_y = tread_from.y - tread_probe_length * tread_ctx.fraction;
			const real_t rise = tread_y - p_start.y;
			if (rise >= 0.002 && rise <= step_height + 0.01) {
				return true;
			}
		}
	}
	return false;
}

// Unreal-style up/forward/down step sweep, but with the landing classified by
// _ground_probe rather than by the sweep contact normal — the fork's CharacterBody3D step-up
// classifies via the capsule contact normal, which reports tilted edge normals on a hemisphere
// and only works reliably with flat-bottomed cylinders.
bool Box3DCharacterMover::_try_step_up(const Vector3 &p_start, const Vector3 &p_target, Vector3 &r_position, Array &r_planes, Vector3 &r_floor_normal, int &r_material_id) const {
	const real_t STEP_MIN_CLEARANCE = 0.01;
	// Low enough that acceleration-limited approach ticks (~1 mm advances after a velocity clip)
	// can still ride a step lip upward; nonzero so flat forward motion never counts as a step.
	const real_t STEP_MIN_RISE = 0.002;

	Vector3 horizontal = p_target - p_start;
	horizontal.y = 0.0;
	if (horizontal.length_squared() < 1e-8) {
		return false;
	}

	const float up_fraction = cast_motion(p_start, Vector3(0.0, step_height, 0.0));
	const real_t clearance = step_height * up_fraction;
	if (clearance < STEP_MIN_CLEARANCE) {
		return false;
	}
	const Vector3 raised = p_start + Vector3(0.0, clearance, 0.0);

	// Sweep toward the horizontal target using the same iterative plane solve as ordinary movement.
	// A single scalar cast fraction stops both components when it hits a side wall; solving the
	// remaining displacement preserves the tangential component, matching expected stair/rail motion.
	const Vector3 raised_target = raised + horizontal;
	Vector3 advanced = raised;
	for (int i = 0; i < 5; i++) {
		const Array planes = _collide_internal(advanced);
		const Dictionary solve = solve_planes(raised_target - advanced, planes);
		const Vector3 delta = solve["delta"];
		const float fraction = cast_motion(advanced, delta);
		const Vector3 applied_delta = delta * fraction;
		advanced += applied_delta;
		// A short first sweep usually means we just reached a contact; the next iteration is what
		// gathers its plane and preserves tangential motion. Stop only after a follow-up solve has
		// genuinely converged, rather than treating any sub-centimeter impact as completion.
		if (i > 0 && applied_delta.length_squared() < 1e-8f) {
			break;
		}
	}

	const float down_fraction = cast_motion(advanced, Vector3(0.0, -clearance, 0.0));
	const Vector3 landed = advanced + Vector3(0.0, -clearance * down_fraction, 0.0);

	if (landed.y - p_start.y < STEP_MIN_RISE) {
		return false;
	}

	const Array landed_planes = _collide_internal(landed);
	Vector3 ground_normal;
	int material_id = 0;
	if (!_ground_probe(landed, landed_planes, ground_normal, material_id)) {
		return false;
	}

	r_position = landed;
	r_planes = landed_planes;
	r_floor_normal = ground_normal;
	r_material_id = material_id;
	return true;
}

// Keep a walking capsule attached to a lower tread while it clears the rounded edge of the previous
// one. A downward mover cast alone is not enough to classify support: while the capsule still touches
// the old lip, that cast stops on the lip before reaching the walkable floor. The ray identifies the
// intended lower floor, while the capsule cast applies only the collision-safe portion of the drop.
// Returning grounded even for a temporarily zero-length cast lets the next horizontal tick clear the
// lip and continue the descent instead of switching permanently to ballistic movement.
bool Box3DCharacterMover::_try_step_down(const Vector3 &p_start, const Vector3 &p_horizontal, const Array &p_planes, Vector3 &r_position, Array &r_planes, Vector3 &r_floor_normal, int &r_material_id, real_t &r_step_delta_y) const {
	const real_t PROBE_UP = BOX3D_MOVER_STEP_PROBE_UP;
	const real_t HEIGHT_TOLERANCE = BOX3D_MOVER_STEP_HEIGHT_TOLERANCE;
	const real_t EDGE_ROLL_ALLOWANCE = BOX3D_MOVER_STEP_EDGE_ROLL_ALLOWANCE;
	const real_t TREAD_BEHIND = 0.05;
	const real_t CAST_OVERRUN = 0.01;
	const real_t floor_threshold = Math::cos(floor_max_angle);

	Vector3 floor_normal;
	real_t floor_y = 0.0;
	int material_id = 0;
	if (!_probe_walkable(p_start + Vector3(0.0, PROBE_UP, 0.0),
			PROBE_UP + step_height + EDGE_ROLL_ALLOWANCE + HEIGHT_TOLERANCE,
			floor_normal, floor_y, material_id) || floor_normal.y <= floor_threshold) {
		return false;
	}

	const real_t drop = p_start.y - floor_y;
	if (drop < -HEIGHT_TOLERANCE) {
		return false;
	}

	// Treads can be shorter than the capsule radius, so the capsule may still ride a lip two treads
	// behind while its center crosses the next edge. Feet height and the active lip contact point are
	// both misleading in that pose. Compare walkable surfaces just behind and below the center to
	// measure the actual adjacent floor-to-floor drop.
	Vector3 upper_normal;
	real_t upper_y = 0.0;
	int upper_material_id = 0;
	bool has_upper_floor = false;
	if (p_horizontal.length_squared() > 1e-8) {
		const Vector3 direction = p_horizontal.normalized();
		const real_t behind_distance = p_horizontal.length() + TREAD_BEHIND;
		const Vector3 upper_from = p_start - direction * behind_distance + Vector3(0.0, PROBE_UP, 0.0);
		has_upper_floor = _probe_walkable(upper_from,
				PROBE_UP + step_height + EDGE_ROLL_ALLOWANCE + HEIGHT_TOLERANCE,
				upper_normal, upper_y, upper_material_id) && upper_normal.y > floor_threshold;
	}
	const real_t floor_drop = has_upper_floor ? upper_y - floor_y : drop;
	if (floor_drop < -HEIGHT_TOLERANCE || floor_drop > step_height + HEIGHT_TOLERANCE) {
		return false;
	}

	// Cast slightly through the ray floor so reaching it produces a fraction below one. A cast that
	// reports no collision is not enough to hold the grounded state: the ray may have found geometry
	// too narrow for the capsule, or initial-overlap filtering may have hidden an invalid landing.
	const real_t cast_distance = MAX(drop + CAST_OVERRUN, CAST_OVERRUN);
	const float down_fraction = cast_motion(p_start, Vector3(0.0, -cast_distance, 0.0));
	if (down_fraction >= 1.0f) {
		return false;
	}

	r_position = p_start + Vector3(0.0, -cast_distance * down_fraction, 0.0);
	r_step_delta_y = r_position.y - p_start.y;
	r_planes = r_step_delta_y < -0.0001 ? _collide_internal(r_position) : p_planes;
	r_floor_normal = floor_normal;
	r_material_id = material_id;
	return true;
}

// The flat footprint can hold support right up to an edge whose adjacent drop is taller than a
// step. Veto step-down there, probing the same envelope _try_step_down accepts so short descents
// stay eligible and only over-height drops are blocked.
bool Box3DCharacterMover::_footprint_blocks_step_down(const Vector3 &p_position) const {
	Vector3 floor_normal;
	real_t floor_y = 0.0;
	int material_id = 0;
	if (!_probe_walkable(p_position + Vector3(0.0, BOX3D_MOVER_STEP_PROBE_UP, 0.0),
			BOX3D_MOVER_STEP_PROBE_UP + step_height + BOX3D_MOVER_STEP_EDGE_ROLL_ALLOWANCE + BOX3D_MOVER_STEP_HEIGHT_TOLERANCE,
			floor_normal, floor_y, material_id)) {
		return false;
	}
	return p_position.y - floor_y > step_height + BOX3D_MOVER_STEP_HEIGHT_TOLERANCE;
}

Dictionary Box3DCharacterMover::move(const Vector3 &p_position, const Vector3 &p_velocity, float p_delta, bool p_was_grounded) const {
	ERR_FAIL_COND_V_MSG(!_can_query(), Dictionary(), "Box3DCharacterMover cannot query the space right now.");

	Vector3 requested_horizontal = p_velocity * p_delta;
	requested_horizontal.y = 0.0;
	const real_t floor_threshold = Math::cos(floor_max_angle);
	Vector3 position = p_position;
	Array solved_planes;
	int total_iterations = 0;
	float last_fraction = 1.0f;

	// Stepping and flat-foot continuation both need the starting contacts. The footprint is deliberately
	// prior-grounded-only: it extends support the way a cylinder's flat bottom would, but never catches an
	// airborne capsule against a nearby lip.
	Array starting_planes;
	bool starting_planes_queried = false;
	bool starting_regular_floor = false;
	const bool footprint_eligible = body_footprint_radius > 0.0 && p_was_grounded && p_velocity.y <= 0.05;
	const bool step_eligible = step_height > 0.0 && p_velocity.y <= 0.05 && requested_horizontal.length_squared() > 1e-8;
	if (footprint_eligible || step_eligible) {
		starting_planes = _collide_internal(p_position);
		starting_planes_queried = true;
		for (int i = 0; i < starting_planes.size(); i++) {
			const Dictionary plane = starting_planes[i];
			if (((Vector3)plane["normal"]).y > floor_threshold) {
				starting_regular_floor = true;
				break;
			}
		}
	}

	Vector3 footprint_probe_normal;
	int footprint_probe_material_id = 0;
	const bool footprint_supported_at_start = footprint_eligible && !starting_regular_floor &&
			_probe_body_footprint(p_position, footprint_probe_normal, footprint_probe_material_id);
	bool footprint_supported_at_target = false;
	if (!footprint_supported_at_start && footprint_eligible && requested_horizontal.length_squared() > 1e-8) {
		const Vector3 projected_feet = p_position + requested_horizontal;
		Vector3 center_normal;
		real_t center_hit_y = 0.0;
		int center_material_id = 0;
		const bool center_supported = _probe_walkable(
				projected_feet + Vector3(0.0, BOX3D_MOVER_FOOTPRINT_PROBE_UP, 0.0),
				BOX3D_MOVER_FOOTPRINT_PROBE_LENGTH, center_normal, center_hit_y, center_material_id) &&
				center_normal.y > floor_threshold;
		if (!center_supported) {
			footprint_supported_at_target = _probe_body_footprint(
					projected_feet, footprint_probe_normal, footprint_probe_material_id);
		}
	}
	const bool footprint_supported_for_move = footprint_supported_at_start || footprint_supported_at_target;
	Vector3 target_position = p_position + p_velocity * p_delta;
	if (p_velocity.y < 0.0 && footprint_supported_for_move) {
		// The motor's grounded -1 m/s stick is vertical intent, not gameplay momentum. A capsule resolves
		// that intent down its rounded cap and manufactures outward drift; a flat bottom resolves it straight
		// up. Removing it only while the matched footprint proves support reproduces the latter response.
		target_position.y = p_position.y;
	}

	// Stepping only assists lateral motion that began on walkable support. Testing support at
	// the start prevents an airborne capsule brushing a ledge from being pulled up onto it.
	bool step_allowed = false;
	if (step_eligible) {
		step_allowed = starting_regular_floor;
		if (!step_allowed) {
			Vector3 probe_normal;
			int probe_material_id = 0;
			step_allowed = _ground_probe(p_position, starting_planes, probe_normal, probe_material_id);
		}
	}
	bool stepped = false;
	bool stepped_down = false;
	bool ground_followed = false;
	bool step_obstruction = false;
	Vector3 stepped_floor_normal;
	int stepped_material_id = 0;
	real_t step_delta_y = 0.0;

	for (int i = 0; i < 5; i++) {
		const Array planes = i == 0 && starting_planes_queried ? starting_planes : _collide_internal(position);
		const Dictionary solve = solve_planes(target_position - position, planes);
		const Vector3 delta = solve["delta"];
		solved_planes = solve["planes"];
		total_iterations += (int)solve["iterations"];

		last_fraction = cast_motion(position, delta);
		const Vector3 applied_delta = delta * last_fraction;
		position += applied_delta;

		if (i > 0 && applied_delta.length_squared() < 1e-8f) {
			break;
		}
	}

	// A round capsule starts climbing a stair lip before it fully stalls. The plane solver can
	// therefore return several centimeters of mostly-upward travel while silently discarding most
	// of the requested horizontal distance. Detect that obstruction in the same move and compare an
	// up/forward/down candidate from the original position. This is collision-triggered rather than
	// waiting for a near-zero displacement, which was both later and frame-rate dependent.
	if (step_allowed) {
		const Vector3 move_direction = requested_horizontal.normalized();
		bool has_lateral_obstruction = false;
		for (int i = 0; i < solved_planes.size(); i++) {
			const Dictionary plane = solved_planes[i];
			const Vector3 normal = plane["normal"];
			const Vector3 horizontal_normal(normal.x, 0.0, normal.z);
			if (horizontal_normal.dot(move_direction) < -0.01) {
				has_lateral_obstruction = true;
				break;
			}
		}

		const real_t STEP_PROGRESS_EPSILON = 0.001;
		const Vector3 regular_horizontal(position.x - p_position.x, 0.0, position.z - p_position.z);
		const real_t requested_progress = requested_horizontal.length();
		const real_t regular_progress = regular_horizontal.dot(move_direction);
		step_obstruction = has_lateral_obstruction && _has_step_obstruction(p_position, requested_horizontal);
		if (step_obstruction) {
			Vector3 stepped_position;
			Array stepped_planes;
			Vector3 step_normal;
			int step_material_id = 0;
			if (_try_step_up(p_position, target_position, stepped_position, stepped_planes, step_normal, step_material_id)) {
				const Vector3 stepped_horizontal(stepped_position.x - p_position.x, 0.0, stepped_position.z - p_position.z);
				const real_t stepped_progress = stepped_horizontal.dot(move_direction);
				// Prefer a valid explicit step when it makes better progress, but also when ordinary
				// capsule motion made equivalent progress by riding the rounded lip. The latter response
				// produces an upward velocity and disables stepping on the next tick, causing the severe
				// angle/speed-dependent stair stick this path exists to avoid.
				const bool regular_was_blocked = regular_progress + STEP_PROGRESS_EPSILON < requested_progress;
				const bool step_is_no_worse = stepped_progress + STEP_PROGRESS_EPSILON >= regular_progress;
				if (step_is_no_worse && (regular_was_blocked || stepped_position.y > position.y + 0.001)) {
					position = stepped_position;
					stepped = true;
					stepped_floor_normal = step_normal;
					stepped_material_id = step_material_id;
					solved_planes = stepped_planes;
					step_delta_y = position.y - p_position.y;
				}
			}
		}
	}

	// Upward motion is an intentional jump and must never be pulled back to the floor. Otherwise a
	// caller that began this tick grounded may follow a walkable descent no taller than step_height.
	// This explicit prior-grounded input is what keeps the stateless mover safe for prediction/replay:
	// velocity alone cannot distinguish walking down from a descending jump.
	bool regular_on_floor = false;
	for (int i = 0; i < solved_planes.size(); i++) {
		const Dictionary plane = solved_planes[i];
		if (((Vector3)plane["normal"]).y > floor_threshold) {
			regular_on_floor = true;
			break;
		}
	}
	if (!stepped && !step_obstruction && !regular_on_floor && p_was_grounded &&
			step_height > 0.0 && p_velocity.y <= 0.05 &&
			!(footprint_supported_for_move && _footprint_blocks_step_down(position))) {
		Vector3 followed_position;
		Array followed_planes;
		Vector3 followed_normal;
		int followed_material_id = 0;
		real_t followed_delta_y = 0.0;
		if (_try_step_down(position, requested_horizontal, solved_planes, followed_position, followed_planes, followed_normal,
				followed_material_id, followed_delta_y)) {
			position = followed_position;
			solved_planes = followed_planes;
			ground_followed = true;
			stepped_down = followed_delta_y < -0.0001;
			stepped_floor_normal = followed_normal;
			stepped_material_id = followed_material_id;
			step_delta_y = followed_delta_y;
		}
	}

	if (push_strength > 0.0 && p_delta > 0.0) {
		Box3DPhysicsServer3D *server = Box3DPhysicsServer3D::get_singleton();
		for (int i = 0; server != nullptr && i < solved_planes.size(); i++) {
			Dictionary plane = solved_planes[i];
			if (!plane.has("rid") || !plane.has("normal") || !plane.has("point")) {
				continue;
			}
			Box3DBody3D *body = server->get_body((RID)plane["rid"]);
			if (body == nullptr || body->get_mode() != PS3DE::BODY_MODE_RIGID) {
				continue;
			}
			const Vector3 normal = plane["normal"];
			const Vector3 point = plane["point"];
			const Vector3 body_velocity = to_godot(b3Body_GetWorldPointVelocity(body->get_body_id(), to_box3d(point)));
			const real_t approach_speed = -(p_velocity - body_velocity).dot(normal);
			if (approach_speed <= 0.0) {
				continue;
			}
			// Effective normal mass at the contact point (samples/sample.cpp SolveMove push pass):
			// scales momentum transfer to what the body can absorb, so push_strength = 1 gives a
			// consistent delta-v across body masses instead of acting as a fixed 1 kg impulse.
			const b3BodyId body_id = body->get_body_id();
			const b3Vec3 push_normal = to_box3d(-normal);
			const b3Vec3 rB = b3SubPos(to_box3d(point), b3Body_GetWorldCenter(body_id));
			const b3Vec3 rnB = b3Cross(rB, push_normal);
			const float k_normal = b3Body_GetInverseMass(body_id) + b3Dot(rnB, b3MulMV(b3Body_GetWorldInverseRotationalInertia(body_id), rnB));
			const float normal_mass = k_normal > 0.0f ? 1.0f / k_normal : 0.0f;
			const Vector3 impulse = -normal * approach_speed * normal_mass * push_strength;
			body->apply_impulse(impulse, point - body->get_transform().origin);
		}
	}
	Vector3 floor_normal;
	Dictionary floor_plane;
	bool on_floor = false;
	bool on_wall = false;
	bool on_ceiling = false;
	for (int i = 0; i < solved_planes.size(); i++) {
		Dictionary plane = solved_planes[i];
		const Vector3 normal = plane["normal"];
		if (normal.y > floor_threshold) {
			on_floor = true;
			if (normal.y > floor_normal.y) {
				floor_normal = normal;
				floor_plane = plane;
			}
		} else if (normal.y < -floor_threshold) {
			on_ceiling = true;
		} else {
			on_wall = true;
		}
	}

	// A capsule mid-step rides the lip on its hemisphere for several ticks; the tilted edge
	// contact fails the plane test above even though the character is supported by walkable
	// ground. Reclassify with ray probes so the floor flag holds through the climb.
	int probe_material_id = -1;
	bool probe_grounded = false;
	bool footprint_supported = false;
	if (ground_followed) {
		on_floor = true;
		probe_grounded = true;
		floor_normal = stepped_floor_normal;
		probe_material_id = stepped_material_id;
	} else if (step_allowed && (stepped || !on_floor || step_obstruction)) {
		if (stepped) {
			on_floor = true;
			probe_grounded = true;
			floor_normal = stepped_floor_normal;
			probe_material_id = stepped_material_id;
		} else {
			Vector3 probe_normal;
			int material_id = 0;
			if (_ground_probe(position, solved_planes, probe_normal, material_id)) {
				on_floor = true;
				probe_grounded = true;
				floor_normal = probe_normal;
				probe_material_id = material_id;
			}
		}
	}
	if (!on_floor && footprint_eligible) {
		Vector3 footprint_normal;
		int footprint_material_id = 0;
		if (_probe_body_footprint(position, footprint_normal, footprint_material_id)) {
			on_floor = true;
			probe_grounded = true;
			footprint_supported = true;
			floor_normal = footprint_normal;
			probe_material_id = footprint_material_id;
		}
	}

	// Do not let the capsule's lower hemisphere turn the end of flat-foot support into a tall
	// grounded glide. Once the shallow footprint no longer reaches the old surface, a walkable
	// capsule contact more than step_height below the previous feet position is a real drop. Short
	// descents remain eligible for the normal step-down behavior.
	if (on_floor && !probe_grounded && footprint_eligible && floor_plane.has("point")) {
		Vector3 final_footprint_normal;
		int final_footprint_material_id = 0;
		const bool final_footprint_supported = _probe_body_footprint(
				position, final_footprint_normal, final_footprint_material_id);
		const real_t floor_drop = p_position.y - ((Vector3)floor_plane["point"]).y;
		if (!final_footprint_supported && floor_drop > step_height + BOX3D_MOVER_STEP_HEIGHT_TOLERANCE) {
			on_floor = false;
			floor_normal = Vector3();
			floor_plane = Dictionary();
		}
	}

	// Velocity clipping, with two floor-stop rules that keep grounded movement from dying at
	// climbable contacts:
	// - The grounded downward stick velocity must not reflect off inclined contacts into lateral
	//   drift — that reversal deadlocks the approach/step cycle at a stair riser.
	// - Planes the character is standing ON must not clip walk velocity: walkable floors always,
	//   and partial-support lip contacts when the ground probes verified walkable tread there.
	//   Otherwise each tick's clip re-zeroes the approach speed and a step climb crawls at the
	//   acceleration floor. True walls and ceilings still clip; penetration is prevented by the
	//   plane solver and cast regardless.
	Vector3 clip_input = p_velocity;
	if (on_floor && p_velocity.y < 0.0) {
		clip_input.y = 0.0;
	}
	Array clip_planes;
	for (int i = 0; i < solved_planes.size(); i++) {
		Dictionary plane = solved_planes[i];
		const Vector3 normal = plane["normal"];
		if (normal.y > floor_threshold) {
			continue;
		}
		if (probe_grounded && normal.y > BOX3D_MOVER_SUPPORT_MIN_NY) {
			continue;
		}
		clip_planes.push_back(plane);
	}
	Vector3 clipped_velocity = clip_velocity(clip_input, clip_planes);
	// Walking velocity is horizontal. Projecting it against an inclined capsule contact can otherwise
	// manufacture upward speed even after clip_input.y was cleared, turning a stair lip into a launch.
	// Keep intentional jumps intact; they do not finish the move on walkable support.
	if (on_floor && p_velocity.y <= 0.05 && clipped_velocity.y > 0.0) {
		clipped_velocity.y = 0.0;
	}

	Dictionary result;
	result["position"] = position;
	result["velocity"] = clipped_velocity;
	result["on_floor"] = on_floor;
	result["on_wall"] = on_wall;
	result["on_ceiling"] = on_ceiling;
	result["floor_normal"] = floor_normal;
	result["planes"] = solved_planes;
	result["cast_fraction"] = last_fraction;
	result["iterations"] = total_iterations;
	result["stepped"] = stepped;
	result["stepped_down"] = stepped_down;
	result["ground_followed"] = ground_followed;
	result["footprint_supported"] = footprint_supported;
	result["step_delta_y"] = step_delta_y;
	if (on_floor && probe_material_id >= 0) {
		_add_floor_material_fields(result, probe_material_id);
	} else if (on_floor && floor_plane.has("point")) {
		Box3DSpace3D *query_space = _get_space();
		Box3DMoverFloorProbeContext ctx;
		ctx.exclusions = &exclusions;
		ctx.target_rid = floor_plane.has("rid") ? (RID)floor_plane["rid"] : RID();
		ctx.target_shape = floor_plane.has("shape") ? (int)floor_plane["shape"] : -1;
		const Vector3 floor_point = floor_plane["point"];
		const Vector3 from = floor_point + Vector3(0.0, 0.05, 0.0);
		const Vector3 translation = Vector3(0.0, -0.25, 0.0);
		b3World_CastRay(query_space->get_world(), to_box3d(from), to_box3d(translation), _make_filter(), _mover_floor_probe_callback, &ctx);
		_add_floor_material_fields(result, ctx.hit ? ctx.material_id : 0);
	} else {
		_add_floor_material_fields(result, 0);
	}
	return result;
}
