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
	if (box3d_physics == nullptr || p_material_id <= 0) {
		r_result["floor_material_name"] = StringName();
		r_result["floor_material"] = Ref<Box3DSurfaceMaterial>();
		return;
	}

	r_result["floor_material_name"] = box3d_physics->get_material_name(p_material_id);
	r_result["floor_material"] = box3d_physics->get_material(p_material_id);
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
	Vector3 normal;
	float fraction = 1.0f;
	int material_id = 0;
	bool hit = false;
};

static float _mover_ray_probe_callback(b3ShapeId p_shape_id, b3Pos p_point, b3Vec3 p_normal, float p_fraction, uint64_t p_user_material_id, int p_triangle_index, int p_child_index, void *p_context) {
	Box3DMoverRayProbeContext *ctx = static_cast<Box3DMoverRayProbeContext *>(p_context);
	Box3DBody3D *body = Box3DDirectSpaceState3D::_get_body(p_shape_id);
	if (body == nullptr || (ctx->exclusions != nullptr && ctx->exclusions->has(body->get_rid()))) {
		return -1.0f;
	}

	ctx->hit = true;
	ctx->normal = to_godot(p_normal);
	ctx->fraction = p_fraction;
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
	ClassDB::bind_method(D_METHOD("set_exclusions", "bodies"), &Box3DCharacterMover::set_exclusions);
	ClassDB::bind_method(D_METHOD("cast_motion", "position", "translation"), &Box3DCharacterMover::cast_motion);
	ClassDB::bind_method(D_METHOD("collide", "position"), &Box3DCharacterMover::collide);
	ClassDB::bind_method(D_METHOD("solve_planes", "target_delta", "planes"), &Box3DCharacterMover::solve_planes);
	ClassDB::bind_method(D_METHOD("clip_velocity", "velocity", "planes"), &Box3DCharacterMover::clip_velocity);
	ClassDB::bind_method(D_METHOD("move", "position", "velocity", "delta"), &Box3DCharacterMover::move);

	ADD_PROPERTY(PropertyInfo(Variant::INT, "collision_mask", PROPERTY_HINT_LAYERS_3D_PHYSICS), "set_collision_mask", "get_collision_mask");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "floor_max_angle", PROPERTY_HINT_RANGE, "0,1.5707963267949,0.001,radians"), "set_floor_max_angle", "get_floor_max_angle");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "push_strength", PROPERTY_HINT_RANGE, "0,10,0.01,or_greater"), "set_push_strength", "get_push_strength");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "step_height", PROPERTY_HINT_RANGE, "0,1,0.01,or_greater,suffix:m"), "set_step_height", "get_step_height");
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
	const real_t probe_up = MIN((real_t)0.01, step_height * 0.5);
	const real_t probe_length = p_horizontal.length() + capsule.radius + 0.01;
	Box3DMoverRayProbeContext ctx;
	ctx.exclusions = &exclusions;
	b3World_CastRay(query_space->get_world(), to_box3d(p_start + Vector3(0.0, probe_up, 0.0)),
			to_box3d(direction * probe_length), _make_filter(), _mover_ray_probe_callback, &ctx);
	if (!ctx.hit) {
		return false;
	}

	const real_t floor_threshold = Math::cos(floor_max_angle);
	const Vector3 horizontal_normal(ctx.normal.x, 0.0, ctx.normal.z);
	return ctx.normal.y <= floor_threshold && horizontal_normal.dot(direction) < -0.01;
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

	const float forward_fraction = cast_motion(raised, horizontal);
	const Vector3 advanced = raised + horizontal * forward_fraction;

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

Dictionary Box3DCharacterMover::move(const Vector3 &p_position, const Vector3 &p_velocity, float p_delta) const {
	ERR_FAIL_COND_V_MSG(!_can_query(), Dictionary(), "Box3DCharacterMover cannot query the space right now.");

	const Vector3 target_position = p_position + p_velocity * p_delta;
	Vector3 requested_horizontal = target_position - p_position;
	requested_horizontal.y = 0.0;
	const real_t floor_threshold = Math::cos(floor_max_angle);
	Vector3 position = p_position;
	Array solved_planes;
	int total_iterations = 0;
	float last_fraction = 1.0f;

	// Stepping only assists lateral motion that began on walkable support. Testing support at
	// the start prevents an airborne capsule brushing a ledge from being pulled up onto it.
	Array starting_planes;
	bool starting_planes_queried = false;
	bool step_allowed = false;
	if (step_height > 0.0 && p_velocity.y <= 0.05 && requested_horizontal.length_squared() > 1e-8) {
		starting_planes = _collide_internal(p_position);
		starting_planes_queried = true;
		for (int i = 0; i < starting_planes.size(); i++) {
			const Dictionary plane = starting_planes[i];
			const Vector3 normal = plane["normal"];
			if (normal.y > floor_threshold) {
				step_allowed = true;
				break;
			}
		}
		if (!step_allowed) {
			Vector3 probe_normal;
			int probe_material_id = 0;
			step_allowed = _ground_probe(p_position, starting_planes, probe_normal, probe_material_id);
		}
	}
	bool stepped = false;
	Vector3 stepped_floor_normal;
	int stepped_material_id = 0;

	for (int i = 0; i < 5; i++) {
		const Array planes = i == 0 && starting_planes_queried ? starting_planes : _collide_internal(position);
		const Dictionary solve = solve_planes(target_position - position, planes);
		const Vector3 delta = solve["delta"];
		solved_planes = solve["planes"];
		total_iterations += (int)solve["iterations"];

		last_fraction = cast_motion(position, delta);
		const Vector3 applied_delta = delta * last_fraction;
		position += applied_delta;

		if (applied_delta.length_squared() < 0.0001f) {
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
		if (has_lateral_obstruction && regular_progress + STEP_PROGRESS_EPSILON < requested_progress &&
				_has_step_obstruction(p_position, requested_horizontal)) {
			Vector3 stepped_position;
			Array stepped_planes;
			Vector3 step_normal;
			int step_material_id = 0;
			if (_try_step_up(p_position, target_position, stepped_position, stepped_planes, step_normal, step_material_id)) {
				const Vector3 stepped_horizontal(stepped_position.x - p_position.x, 0.0, stepped_position.z - p_position.z);
				const real_t stepped_progress = stepped_horizontal.dot(move_direction);
				if (stepped_progress > regular_progress + STEP_PROGRESS_EPSILON) {
					position = stepped_position;
					stepped = true;
					stepped_floor_normal = step_normal;
					stepped_material_id = step_material_id;
					solved_planes = stepped_planes;
				}
			}
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
	if (!on_floor && step_allowed) {
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
	const Vector3 clipped_velocity = clip_velocity(clip_input, clip_planes);

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
