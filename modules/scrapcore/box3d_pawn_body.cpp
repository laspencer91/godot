/**************************************************************************/
/*  box3d_pawn_body.cpp                                                   */
/**************************************************************************/

#include "box3d_pawn_body.h"

#include "scene/3d/physics/rigid_body_3d.h"
#include "scene/main/scene_tree.h"
#include "scene/resources/3d/capsule_shape_3d.h"
#include "scene/resources/3d/cylinder_shape_3d.h"
#include "scene/resources/3d/world_3d.h"
#include "servers/physics_3d/direct_states/physics_direct_space_state_3d.h"
#include "servers/physics_3d/physics_server_3d.h"

#include <cmath>

namespace {

inline Vector3 to_gd(const scrap::Vec3 &v) {
	return Vector3(v.x, v.y, v.z);
}
inline scrap::Vec3 to_scrap(const Vector3 &v) {
	return scrap::Vec3{ v.x, v.y, v.z };
}

const StringName &pawn_group() {
	static StringName group = StringName("pawns");
	return group;
}

} // namespace

// ---- ScrapLadderVolume (mirror of src/world/ladder_volume.gd) ---------------

Vector3 ScrapLadderVolume::climb_normal_gd(const Vector3 &p_world_point) const {
	// Whichever face the point is most "outside" of, each measured against that
	// face's own half-extent. Dots are real_t; the ratio compare runs in double,
	// exactly as the GDScript promotes.
	const Vector3 delta = p_world_point - position;
	const Vector3 &n = outward;
	const Vector3 &s = side;
	const real_t dn = delta.dot(n);
	const real_t ds = delta.dot(s);
	if (scrap::absf(double(dn)) / scrap::maxf(ladder_attach_depth, 0.0001) >= scrap::absf(double(ds)) / scrap::maxf(ladder_half_width, 0.0001)) {
		return double(dn) >= 0.0 ? n : -n;
	}
	return double(ds) >= 0.0 ? s : -s;
}

Vector3 ScrapLadderVolume::climb_side_for_normal_gd(const Vector3 &p_normal) const {
	// Comparison literals promote to double exactly where the GDScript does
	// (float lhs vs binary64 literal) -- here and throughout this file.
	Vector3 s = p_normal.cross(Vector3(0, 1, 0));
	if (double(s.length_squared()) <= 0.001) {
		return side; // side_dir() fallback: the registered side axis
	}
	return s.normalized();
}

Vector3 ScrapLadderVolume::local_point_gd(const Vector3 &p_world_point) const {
	const Vector3 delta = p_world_point - position;
	return Vector3(delta.dot(side), delta.y, delta.dot(-outward));
}

scrap::Vec3 ScrapLadderVolume::outward_normal() const {
	return to_scrap(outward);
}

scrap::Vec3 ScrapLadderVolume::climb_normal(const scrap::Vec3 &p_world_point) const {
	return to_scrap(climb_normal_gd(to_gd(p_world_point)));
}

scrap::Vec3 ScrapLadderVolume::climb_side(const scrap::Vec3 &p_world_point) const {
	return to_scrap(climb_side_for_normal_gd(climb_normal_gd(to_gd(p_world_point))));
}

scrap::Vec3 ScrapLadderVolume::climb_side_for_normal(const scrap::Vec3 &p_normal) const {
	return to_scrap(climb_side_for_normal_gd(to_gd(p_normal)));
}

scrap::Vec3 ScrapLadderVolume::local_point(const scrap::Vec3 &p_world_point) const {
	return to_scrap(local_point_gd(to_gd(p_world_point)));
}

bool ScrapLadderVolume::accepts_position(const scrap::Vec3 &p_world_point, scrap::Scalar p_margin) const {
	const Vector3 local = local_point_gd(to_gd(p_world_point));
	return scrap::absf(double(local.x)) <= ladder_half_width + p_margin &&
			double(local.y) >= -p_margin &&
			double(local.y) <= ladder_height + p_margin &&
			double(local.z) >= -ladder_attach_depth - p_margin &&
			double(local.z) <= ladder_attach_depth + p_margin;
}

scrap::Vec3 ScrapLadderVolume::global_position() const {
	return to_scrap(position);
}

bool ScrapLadderVolume::accepts_top_descent(const Vector3 &p_world_point, double p_lateral_margin, double p_top_band, double p_back_reach) const {
	const Vector3 local = local_point_gd(p_world_point);
	if (scrap::absf(double(local.x)) > ladder_half_width + p_lateral_margin) {
		return false;
	}
	if (double(local.y) < ladder_height - p_top_band || double(local.y) > ladder_height + p_top_band) {
		return false;
	}
	return double(local.z) >= -ladder_attach_depth && double(local.z) <= p_back_reach;
}

// ---- Box3DPawnBody: backend internals ---------------------------------------

void Box3DPawnBody::configure(CharacterBody3D *p_pawn, const scrap::MovementParams &p_params) {
	pawn = p_pawn;
	pawn_id = pawn != nullptr ? pawn->get_instance_id() : ObjectID();
	cfg = p_params;
	// PlayerPawn: @onready pawn_capsule = get_node_or_null("CollisionShape3D")
	// -- the direct child NAMED "CollisionShape3D", and a null collider is
	// tolerated (feet offset 0, clamped fallback dims), exactly like the
	// reference. Never "the first shape child".
	pawn_capsule = Object::cast_to<CollisionShape3D>(pawn->get_node_or_null(NodePath("CollisionShape3D")));
	pawn_capsule_id = pawn_capsule != nullptr ? pawn_capsule->get_instance_id() : ObjectID();

	if (mover.is_null()) {
		mover.instantiate();
	}
	mover->setup(pawn->get_world_3d()->get_space());
	_sync_mover_settings(p_params);
	_set_capsule_height(p_params.stand_height, p_params.capsule_radius);
	_update_exclusions();

	// Initial cached contact: probe the floor unless moving meaningfully upward.
	CollisionResult result;
	result.position = pawn->get_global_position();
	result.velocity = pawn->get_velocity();
	if (double(pawn->get_velocity().y) <= SNAP_UPWARD_LIMIT) {
		result.floor_normal = _floor_normal_at(_node_position_to_feet(pawn->get_global_position()));
		result.on_floor = result.floor_normal != Vector3();
	}
	last_result = result;
	pending_step_delta_y = 0.0;
}

bool Box3DPawnBody::body_valid() {
	if (pawn == nullptr || ObjectDB::get_instance(pawn_id) != pawn || !pawn->is_inside_tree()) {
		return false;
	}
	// A freed collider degrades to the tolerated-null path instead of dangling.
	if (pawn_capsule != nullptr && ObjectDB::get_instance(pawn_capsule_id) != pawn_capsule) {
		pawn_capsule = nullptr;
		pawn_capsule_id = ObjectID();
	}
	return true;
}

double Box3DPawnBody::consume_step_delta_y() {
	const double delta_y = pending_step_delta_y;
	pending_step_delta_y = 0.0;
	return delta_y;
}

bool Box3DPawnBody::refresh_ground_contact_after_teleport(const scrap::MovementParams &p_params) {
	// Backend.refresh_ground_contact_after_teleport, 1:1: floor probe at the
	// teleported feet, snap-down fallback when the previous contact claimed
	// floor, wall normal from a plane collide, mirror onto the live body.
	cfg = p_params;
	_prepare_mover(p_params);

	const bool was_grounded = last_result.on_floor;
	Vector3 feet = _node_position_to_feet(pawn->get_global_position());
	const Vector3 saved_velocity = pawn->get_velocity();
	Vector3 floor_normal;
	if (double(saved_velocity.y) <= SNAP_UPWARD_LIMIT) {
		floor_normal = _floor_normal_at(feet);
		if (floor_normal == Vector3() && was_grounded) {
			Vector3 snapped_feet;
			Vector3 snapped_normal;
			if (_snap_down(feet, p_params.floor_snap_length, snapped_feet, snapped_normal)) {
				feet = snapped_feet;
				floor_normal = snapped_normal;
			}
		}
	}

	const Array planes = mover->collide(feet);
	CollisionResult result;
	result.position = _feet_position_to_node(feet);
	result.velocity = saved_velocity;
	result.on_floor = floor_normal != Vector3();
	result.floor_normal = floor_normal;
	result.wall_normal = _wall_normal_from_planes(planes, saved_velocity, p_params.floor_max_angle);
	result.on_wall = result.wall_normal != Vector3();

	if (pawn->get_global_position() != result.position) {
		pawn->set_global_position(result.position);
	}
	last_result = result;
	return result.on_floor;
}

void Box3DPawnBody::teleport_to_state(const scrap::MovementState &p_state, const scrap::MovementParams &p_params) {
	// A replay/teleport starts a new collision solve timeline. Do not carry an
	// unpresented stair delta into it.
	pending_step_delta_y = 0.0;
	apply_movement_state(p_state, p_params);
	pawn->reset_physics_interpolation();
}

void Box3DPawnBody::_prepare_mover(const scrap::MovementParams &p_params) {
	// Shared pre-solve sync: push settings, restore the live capsule (can_stand
	// may have swapped it), refresh the pawn-exclusion set.
	_sync_mover_settings(p_params);
	mover->set_capsule(mover_height, capsule_radius);
	_update_exclusions();
}

void Box3DPawnBody::_sync_mover_settings(const scrap::MovementParams &p_params) {
	mover->set_collision_mask(pawn->get_collision_mask());
	mover->set_floor_max_angle(p_params.floor_max_angle);
	mover->set_step_height(p_params.step_height);
	mover->set_body_footprint_radius(p_params.body_footprint_radius);
	mover->set_push_strength(0.0);
}

TypedArray<RID> Box3DPawnBody::_pawn_body_exclusions() const {
	// PlayerPawn.pawn_body_exclusions: self first, then every pawn-group body.
	TypedArray<RID> exclude;
	exclude.push_back(pawn->get_rid());
	if (pawn->is_inside_tree()) {
		const Vector<Node *> nodes = pawn->get_tree()->get_nodes_in_group(pawn_group());
		for (Node *node : nodes) {
			CollisionObject3D *other = Object::cast_to<CollisionObject3D>(node);
			if (other != nullptr && other != pawn) {
				exclude.push_back(other->get_rid());
			}
		}
	}
	return exclude;
}

HashSet<RID> Box3DPawnBody::_pawn_ray_exclusion_set() const {
	// pawn_ray_exclusions shares the body set (one source of truth).
	HashSet<RID> exclude;
	exclude.insert(pawn->get_rid());
	if (pawn->is_inside_tree()) {
		const Vector<Node *> nodes = pawn->get_tree()->get_nodes_in_group(pawn_group());
		for (Node *node : nodes) {
			CollisionObject3D *other = Object::cast_to<CollisionObject3D>(node);
			if (other != nullptr && other != pawn) {
				exclude.insert(other->get_rid());
			}
		}
	}
	return exclude;
}

void Box3DPawnBody::_update_exclusions() {
	mover->set_exclusions(_pawn_body_exclusions());
}

Vector3 Box3DPawnBody::_node_origin_to_feet_offset() const {
	if (pawn_capsule == nullptr) {
		return Vector3();
	}
	const Vector3 shape_feet = pawn_capsule->get_global_transform().xform(
			Vector3(0.0f, real_t(-collision_height * 0.5), 0.0f));
	return shape_feet - pawn->get_global_position();
}

Vector3 Box3DPawnBody::_node_position_to_feet(const Vector3 &p_node_position) const {
	return p_node_position + _node_origin_to_feet_offset();
}

Vector3 Box3DPawnBody::_feet_position_to_node(const Vector3 &p_feet_position) const {
	return p_feet_position - _node_origin_to_feet_offset();
}

bool Box3DPawnBody::_snap_down(const Vector3 &p_feet_position, double p_distance, Vector3 &r_position, Vector3 &r_normal) const {
	if (p_distance <= 0.0) {
		return false;
	}
	const Vector3 translation = Vector3(0, -1, 0) * real_t(p_distance);
	const double fraction = scrap::clampf(double(mover->cast_motion(p_feet_position, translation)), 0.0, 1.0);
	const Vector3 snapped_feet = p_feet_position + translation * real_t(fraction);
	const Vector3 floor_normal = _floor_normal_at(snapped_feet);
	if (floor_normal == Vector3()) {
		return false;
	}
	r_position = snapped_feet;
	r_normal = floor_normal;
	return true;
}

Vector3 Box3DPawnBody::_floor_normal_at(const Vector3 &p_feet_position) const {
	const double threshold = std::cos(cfg.floor_max_angle);
	Vector3 best;
	const Vector3 probe_position = p_feet_position - Vector3(0, 1, 0) * real_t(FLOOR_OVERLAP_PROBE);
	const Array planes = mover->collide(probe_position);
	for (int i = 0; i < planes.size(); i++) {
		const Dictionary plane = planes[i];
		const Vector3 normal = plane.get("normal", Vector3());
		if (double(normal.y) > threshold && normal.y > best.y) {
			best = normal;
		}
	}
	return best;
}

Vector3 Box3DPawnBody::_wall_normal_from_planes(const Array &p_planes, const Vector3 &p_pre_move_velocity, double p_floor_max_angle) const {
	Vector3 horizontal_velocity = Vector3(p_pre_move_velocity.x, 0.0f, p_pre_move_velocity.z);
	const bool has_direction = double(horizontal_velocity.length_squared()) > 0.000001;
	if (has_direction) {
		horizontal_velocity = horizontal_velocity.normalized();
	}
	const double floor_threshold = std::cos(p_floor_max_angle);
	Vector3 best;
	double best_dot = INFINITY;
	for (int i = 0; i < p_planes.size(); i++) {
		const Dictionary plane = p_planes[i];
		Vector3 normal = plane.get("normal", Vector3());
		if (double(normal.length_squared()) <= 0.000001) {
			continue;
		}
		normal = normal.normalized();
		if (double(normal.y) > floor_threshold || double(normal.y) < -floor_threshold) {
			continue;
		}
		const double opposition_dot = has_direction ? double(normal.dot(horizontal_velocity)) : 0.0;
		if (best == Vector3() || opposition_dot < best_dot) {
			best = normal;
			best_dot = opposition_dot;
		}
	}
	return best;
}

double Box3DPawnBody::_apply_capsule_dimensions(double p_height, double p_radius) {
	// PlayerPawn.apply_capsule_dimensions: write the live collider dims and
	// recenter it, shape-agnostically (capsule OR cylinder).
	const double resolved = scrap::maxf(p_height, p_radius * 2.0);
	if (pawn_capsule == nullptr) {
		return resolved;
	}
	// SAME-VALUE WRITES ARE NOT FREE: a Shape3D height/radius write fires the
	// resource-changed chain into the physics server, and that shape rebuild's
	// cost SCALES WITH BROADPHASE POPULATION (measured 84 -> 165 us per write
	// going from 4 to 16 pawns). The motor resizes twice per pawn per tick, and
	// at steady state the incoming values land on what the shape already stores
	// at its float32 width -- skip writes that would store what is already
	// there. Guard BOTH branches, operation-for-operation with the GDScript
	// twin (PlayerPawn._shape_dims_unchanged): a future collider swap must not
	// resurrect the cost silently on either path.
	Ref<Shape3D> shape = pawn_capsule->get_shape();
	double shape_height = 0.0;
	if (Ref<CapsuleShape3D> capsule = shape; capsule.is_valid()) {
		if (float(resolved) == capsule->get_height() && float(p_radius) == capsule->get_radius()) {
			return double(capsule->get_height());
		}
		capsule->set_height(resolved);
		capsule->set_radius(p_radius);
		shape_height = double(capsule->get_height());
	} else if (Ref<CylinderShape3D> cylinder = shape; cylinder.is_valid()) {
		if (float(p_height) == cylinder->get_height() && float(p_radius) == cylinder->get_radius()) {
			return double(cylinder->get_height());
		}
		cylinder->set_height(p_height);
		cylinder->set_radius(p_radius);
		shape_height = double(cylinder->get_height());
	} else {
		return resolved;
	}
	Vector3 capsule_pos = pawn_capsule->get_position();
	capsule_pos.y = real_t(shape_height * 0.5);
	pawn_capsule->set_position(capsule_pos);
	return shape_height;
}

void Box3DPawnBody::_set_capsule_height(double p_height, double p_radius) {
	// The mover always uses the 2x-radius-clamped height; the node collider
	// write (capsule vs cylinder) resolves the feet-offset height.
	capsule_radius = p_radius;
	mover_height = scrap::maxf(p_height, p_radius * 2.0);
	mover->set_capsule(mover_height, p_radius);
	collision_height = pawn != nullptr ? _apply_capsule_dimensions(p_height, p_radius) : mover_height;
}

// ---- Box3DPawnBody: IPawnBody -----------------------------------------------

void Box3DPawnBody::apply_movement_state(const scrap::MovementState &p_state, const scrap::MovementParams &p_params) {
	// PlayerPawn.apply_movement_state: body/collider only, never presentation.
	pawn->set_global_position(to_gd(p_state.position));
	Vector3 rotation = pawn->get_rotation();
	rotation.y = real_t(p_state.yaw);
	pawn->set_rotation(rotation);
	pawn->set_velocity(to_gd(p_state.velocity));
	if (p_state.current_height > 0.0) {
		set_capsule_for_pose(p_state.current_height, p_state.pose, p_params);
	}
}

void Box3DPawnBody::move_body(const scrap::Vec3 &p_next_velocity, const scrap::MovementParams &p_params) {
	cfg = p_params;
	_prepare_mover(p_params);

	const Vector3 next_velocity = to_gd(p_next_velocity);
	// The node<->feet offset is invariant within a single move; resolve once.
	const Vector3 feet_offset = _node_origin_to_feet_offset();
	const bool was_grounded = last_result.on_floor;
	const Vector3 start_feet = pawn->get_global_position() + feet_offset;
	const Dictionary raw = mover->move(
			start_feet,
			next_velocity,
			pawn->get_physics_process_delta_time(),
			was_grounded);
	CollisionResult result;
	if (raw.is_empty()) {
		// The empty-mover answer still flows through PlayerPawn.move_body's
		// mirror in the reference: position is unchanged (write no-ops) but the
		// live body's velocity becomes next_velocity.
		result.position = pawn->get_global_position();
		result.velocity = next_velocity;
		last_result = result;
		pending_step_delta_y += result.step_delta_y;
		if (pawn->get_global_position() != result.position) {
			pawn->set_global_position(result.position);
		}
		if (pawn->get_velocity() != result.velocity) {
			pawn->set_velocity(result.velocity);
		}
		return;
	}

	const Vector3 solved_feet = raw.get("position", start_feet);
	const Array planes = raw.get("planes", Array());
	if (bool(raw.get("stepped", false)) || bool(raw.get("stepped_down", false))) {
		result.step_delta_y = double(raw.get("step_delta_y", solved_feet.y - start_feet.y));
	}
	result.position = solved_feet - feet_offset;
	result.velocity = raw.get("velocity", next_velocity);
	result.on_floor = bool(raw.get("on_floor", false));
	result.on_wall = bool(raw.get("on_wall", false));
	result.floor_normal = raw.get("floor_normal", Vector3());
	result.wall_normal = _wall_normal_from_planes(planes, next_velocity, p_params.floor_max_angle);
	result.on_wall = result.on_wall || result.wall_normal != Vector3();
	last_result = result;

	// PlayerPawn.move_body: mirror the query-only solution onto the live body.
	pending_step_delta_y += result.step_delta_y;
	if (pawn->get_global_position() != result.position) {
		pawn->set_global_position(result.position);
	}
	if (pawn->get_velocity() != result.velocity) {
		pawn->set_velocity(result.velocity);
	}
}

scrap::Vec3 Box3DPawnBody::get_position() const {
	return to_scrap(pawn->get_global_position());
}

scrap::Vec3 Box3DPawnBody::get_velocity() const {
	return to_scrap(pawn->get_velocity());
}

void Box3DPawnBody::set_velocity(const scrap::Vec3 &p_velocity) {
	pawn->set_velocity(to_gd(p_velocity));
}

bool Box3DPawnBody::is_grounded() const {
	// PlayerPawn.is_grounded routes to the backend explicitly (the native
	// CharacterBody3D is_on_floor is always false under the query-only mover).
	return last_result.on_floor;
}

bool Box3DPawnBody::is_on_floor() const {
	return last_result.on_floor;
}

bool Box3DPawnBody::is_on_wall() const {
	return last_result.on_wall;
}

scrap::Vec3 Box3DPawnBody::get_wall_normal() const {
	return to_scrap(last_result.wall_normal);
}

scrap::Vec3 Box3DPawnBody::get_floor_normal() const {
	// PlayerCollisionBackend.get_floor_normal (player_collision_backend.gd:61):
	// the cached normal from the last solve, ZERO when airborne. Routed through
	// the cached result rather than CharacterBody3D::get_floor_normal() for the
	// same reason is_grounded() is -- the query-only mover never runs
	// move_and_slide, so the native value is a flat ZERO and every slope would
	// read as level ground. Same freshness contract as is_on_floor(): it
	// describes the END of the previous move, which is what the motor reads at
	// the top of the tick, one read per tick immediately after is_grounded()
	// (movement_motor.cpp:1215-1223).
	return to_scrap(last_result.floor_normal);
}

bool Box3DPawnBody::can_stand_up(scrap::Scalar p_current_height, const scrap::MovementParams &p_params) const {
	// Backend.can_stand: collide a STAND capsule at the feet and reject on any
	// ceiling-ish plane, then sweep the matched flat head footprint through the
	// rounded-cap blind region from the live height to full standing height.
	Box3DPawnBody *self = const_cast<Box3DPawnBody *>(this);
	self->_sync_mover_settings(p_params);
	self->_update_exclusions();
	const Vector3 feet = _node_position_to_feet(pawn->get_global_position());
	mover->set_capsule(p_params.stand_height, p_params.capsule_radius);
	const Array planes = mover->collide(feet);
	bool clear = true;
	for (int i = 0; i < planes.size(); i++) {
		const Dictionary plane = planes[i];
		const Vector3 normal = plane.get("normal", Vector3(0, 1, 0));
		if (double(normal.y) < -0.1) {
			clear = false;
			break;
		}
	}
	if (clear) {
		clear = mover->has_head_clearance(feet, p_current_height, p_params.stand_height);
	}
	mover->set_capsule(mover_height, capsule_radius);
	return clear;
}

void Box3DPawnBody::set_capsule_for_pose(scrap::Scalar p_height, int32_t p_pose, const scrap::MovementParams &p_params) {
	// Prone shrinks the radius so the capsule can sit genuinely low.
	const double radius = p_pose == int32_t(scrap::Pose::PRONE) ? p_params.prone_radius : p_params.capsule_radius;
	_set_capsule_height(p_height, radius);
}

bool Box3DPawnBody::raycast_blocked(const scrap::Vec3 &p_from, const scrap::Vec3 &p_to, uint32_t p_mask) const {
	// PlayerPawn.raycast_blocked: true = something on `mask` blocks from->to,
	// excluding ALL pawn bodies (lean is blocked by world, never a player body).
	PhysicsDirectSpaceState3D *space_state = pawn->get_world_3d()->get_direct_space_state();
	PS3DT::RayParameters ray;
	ray.from = to_gd(p_from);
	ray.to = to_gd(p_to);
	ray.collision_mask = p_mask;
	ray.exclude = _pawn_ray_exclusion_set();
	PS3DT::RayResult hit;
	return space_state->intersect_ray(ray, hit);
}

scrap::MantleProbe Box3DPawnBody::check_mantle_opportunity(const scrap::Vec2 &p_input, const scrap::MovementState &p_state, const scrap::MovementParams &p_params) const {
	// PlayerPawn.check_mantle_opportunity, 1:1.
	if (double(p_input.y) <= 0.5) {
		return {};
	}
	if (scrap::absf(double(p_input.x)) > scrap::absf(double(p_input.y)) * 1.1) {
		return {};
	}

	const Basis yaw_basis = Basis(Vector3(0, 1, 0), real_t(p_state.yaw));
	Vector3 forward = -yaw_basis.get_column(2);
	forward.y = 0.0f;
	forward = forward.normalized();

	const Vector3 probe = pawn->get_global_position() + forward * real_t(p_params.mantle_detect_distance - 0.1) + Vector3(0, real_t(p_params.max_mantle_height + 0.3), 0);
	const Vector3 to = probe + Vector3(0, -1, 0) * real_t(p_params.max_mantle_height + 0.5);
	const HashSet<RID> exclusions = _pawn_ray_exclusion_set();
	PhysicsDirectSpaceState3D *space_state = pawn->get_world_3d()->get_direct_space_state();

	// Box3D ignores hit_from_inside on rays, so explicitly reject a ledge probe
	// whose origin is already buried in static world geometry.
	PS3DT::PointParameters point_query;
	point_query.position = probe;
	point_query.collision_mask = p_params.mantle_static_mask;
	point_query.exclude = exclusions;
	point_query.collide_with_bodies = true;
	point_query.collide_with_areas = false;
	PS3DT::ShapeResult point_hit;
	if (space_state->intersect_point(point_query, &point_hit, 1) > 0) {
		return {};
	}

	PS3DT::RayParameters ray;
	ray.from = probe;
	ray.to = to;
	ray.collision_mask = p_params.mantle_static_mask; // a ledge is static WORLD geometry
	ray.exclude = exclusions; // ...and never another player's body
	// A probe that STARTS buried in solid must self-reject; servers that
	// implement hit_from_inside report a ZERO normal, rejected by the slope gate.
	ray.hit_from_inside = true;
	PS3DT::RayResult hit;
	if (!space_state->intersect_ray(ray, hit)) {
		return {};
	}
	if (double(hit.normal.y) < 0.7) {
		return {};
	}

	const Vector3 ledge_point = hit.position;
	const double ledge_height = double(ledge_point.y) - double(pawn->get_global_position().y);
	const double effective_max = p_state.pose != int32_t(scrap::Pose::STAND) ? p_params.mantle_crouch_max_height : p_params.max_mantle_height;
	if (ledge_height < p_params.min_mantle_height || ledge_height > effective_max) {
		return {};
	}

	Vector3 landing = ledge_point + forward * 0.4f;
	landing.y = ledge_point.y;
	const scrap::MantleProbe swept = swept_mantle_landing(to_scrap(ledge_point), to_scrap(landing));
	if (!swept.valid) {
		return {};
	}

	scrap::MantleProbe probe_result;
	probe_result.valid = true;
	probe_result.ledge_y = double(ledge_point.y);
	probe_result.landing_position = swept.landing_position;
	return probe_result;
}

scrap::MantleProbe Box3DPawnBody::swept_mantle_landing(const scrap::Vec3 &p_ledge_point, const scrap::Vec3 &p_target_landing) const {
	// Backend.swept_mantle_landing: validate the mantle path with live-body
	// sweeps (rise in place, then sweep forward), never a destination overlap.
	const double LIFT = 0.06;
	const Vector3 ledge_point = to_gd(p_ledge_point);
	const Vector3 target_landing = to_gd(p_target_landing);
	const double rise = scrap::maxf(double(ledge_point.y) + LIFT - double(pawn->get_global_position().y), 0.0);

	PhysicsServer3D *server = PhysicsServer3D::get_singleton();
	PS3DT::MotionParameters params;
	PS3DT::MotionResult result;
	params.from = pawn->get_global_transform();
	params.motion = Vector3(0, 1, 0) * real_t(rise);
	params.margin = 0.04;
	if (server->body_test_motion(pawn->get_rid(), params, &result)) {
		return {};
	}

	Transform3D elevated = pawn->get_global_transform();
	elevated.origin.y += real_t(rise);
	Vector3 fwd_motion = target_landing - elevated.origin;
	fwd_motion.y = 0.0f;
	params.from = elevated;
	params.motion = fwd_motion;
	const bool blocked = server->body_test_motion(pawn->get_rid(), params, &result);
	Vector3 travel = blocked ? result.travel : fwd_motion;
	travel.y = 0.0f;
	// The body must get at least over the surface point hit by the ledge ray.
	const double needed = double(Vector2(ledge_point.x - elevated.origin.x, ledge_point.z - elevated.origin.z).length());
	if (double(travel.length()) < needed - 0.01) {
		return {};
	}
	Vector3 swept_landing = elevated.origin + travel;
	swept_landing.y = ledge_point.y;
	scrap::MantleProbe out;
	out.valid = true;
	out.landing_position = to_scrap(swept_landing);
	return out;
}

scrap::LadderProbe Box3DPawnBody::check_ladder_opportunity(const scrap::Vec2 &p_input, const scrap::MovementState &p_state, const scrap::MovementParams &p_params) const {
	// PlayerPawn.check_ladder_opportunity over the registered ladder set
	// (register order stands in for the node-path-sorted runtime-id walk; the
	// game-side wiring registers with the same authored-or-index ids).
	if (p_state.pose != int32_t(scrap::Pose::STAND)) {
		return {};
	}
	const bool pressing_forward = double(p_input.y) > p_params.ladder_forward_threshold;
	const bool pressing_back = double(p_input.y) < -p_params.ladder_forward_threshold;
	if (!pressing_forward && !pressing_back) {
		return {};
	}

	const Basis yaw_basis = Basis(Vector3(0, 1, 0), real_t(p_state.yaw));
	Vector3 forward = -yaw_basis.get_column(2);
	forward.y = 0.0f;
	if (double(forward.length_squared()) <= 0.001) {
		return {};
	}
	forward = forward.normalized();

	if (ladders == nullptr) {
		return {};
	}
	const Vector3 pawn_position = pawn->get_global_position();
	for (uint32_t i = 0; i < ladders->size(); i++) {
		const ScrapLadderVolume &ladder = (*ladders)[i];
		const Vector3 out = ladder.outward;
		// Top descent: on the wall TOP behind the out face, facing it, press BACK.
		if (pressing_back &&
				ladder.accepts_top_descent(pawn_position, p_params.ladder_attach_margin, p_params.ladder_top_mount_band, p_params.ladder_top_back_reach) &&
				double(forward.dot(-out)) >= p_params.ladder_enter_facing_dot) {
			scrap::LadderProbe probe;
			probe.valid = true;
			probe.ladder_id = _ladder_runtime_id(ladder);
			return probe;
		}
		// Front / bottom / side grab: inside the slab, facing the face, pressing INTO it.
		if (!ladder.accepts_position(to_scrap(pawn_position), p_params.ladder_attach_margin)) {
			continue;
		}
		const Vector3 face_normal = ladder.climb_normal_gd(pawn_position);
		if (double(face_normal.dot(out)) < -0.5) {
			continue;
		}
		if (double(forward.dot(-face_normal)) < p_params.ladder_enter_facing_dot) {
			continue;
		}
		if (pressing_forward) {
			scrap::LadderProbe probe;
			probe.valid = true;
			probe.ladder_id = _ladder_runtime_id(ladder);
			return probe;
		}
	}
	return {};
}

scrap::WallJumpProbe Box3DPawnBody::check_wall_jump(const scrap::MovementState &p_state, const scrap::Vec3 &p_wish, const scrap::MovementParams &p_params) const {
	// Backend.check_wall_jump, 1:1: probe fan, mask swap, sweep, normal gates.
	LocalVector<Vector3> directions;
	_append_unique_wall_probe_dir(directions, to_gd(p_wish));
	_append_unique_wall_probe_dir(directions, Vector3(p_state.velocity.x, 0.0f, p_state.velocity.z));

	const Basis yaw_basis = Basis(Vector3(0, 1, 0), real_t(p_state.yaw));
	_append_unique_wall_probe_dir(directions, -yaw_basis.get_column(2));
	_append_unique_wall_probe_dir(directions, yaw_basis.get_column(0));
	_append_unique_wall_probe_dir(directions, -yaw_basis.get_column(0));
	_append_unique_wall_probe_dir(directions, yaw_basis.get_column(2));

	PhysicsServer3D *server = PhysicsServer3D::get_singleton();
	const uint32_t previous_mask = pawn->get_collision_mask();
	pawn->set_collision_mask(p_params.wall_jump_static_mask);
	scrap::WallJumpProbe hit_data;
	for (uint32_t d = 0; d < directions.size(); d++) {
		const Vector3 direction = directions[d];
		PS3DT::MotionResult result;
		PS3DT::MotionParameters params;
		params.from = pawn->get_global_transform();
		params.motion = direction * real_t(p_params.wall_jump_check_distance);
		params.margin = p_params.wall_jump_check_radius;
		if (!server->body_test_motion(pawn->get_rid(), params, &result)) {
			continue;
		}

		const int count = MAX(result.collision_count, 1);
		for (int i = 0; i < count; i++) {
			// Out-of-range reads mirror GDScript's zero-normal answers (the
			// maxi(count, 1) quirk): a zero normal is skipped by the gate below.
			// DELIBERATE DIAGNOSTIC DIFFERENCE: on a zero-collision report the
			// GDScript's get_collision_normal(0) also emits an engine index
			// error; here the zero normal is synthesized silently. The state
			// outcome is identical -- only the log line differs.
			Vector3 normal = i < result.collision_count ? result.collisions[i].normal : Vector3();
			if (double(normal.length_squared()) <= 0.001) {
				continue;
			}
			if (scrap::absf(double(normal.y)) > p_params.wall_jump_max_normal_y) {
				continue;
			}
			Object *collider = i < result.collision_count ? ObjectDB::get_instance(result.collisions[i].collider_id) : nullptr;
			if (Object::cast_to<RigidBody3D>(collider) != nullptr) {
				continue;
			}
			if (!_wall_jump_face_reaches_min_height(direction, p_params)) {
				continue;
			}
			normal.y = 0.0f;
			normal = normal.normalized();
			hit_data.valid = true;
			hit_data.normal = to_scrap(normal);
			hit_data.point = i < result.collision_count ? to_scrap(result.collisions[i].position) : scrap::Vec3::zero();
			hit_data.direction = to_scrap(direction);
			break;
		}
		if (hit_data.valid) {
			break;
		}
	}
	pawn->set_collision_mask(previous_mask);
	return hit_data;
}

bool Box3DPawnBody::_wall_jump_face_reaches_min_height(const Vector3 &p_direction, const scrap::MovementParams &p_params) const {
	Transform3D elevated = pawn->get_global_transform();
	elevated.origin.y += real_t(p_params.stand_height * 0.5);
	PS3DT::MotionParameters params;
	PS3DT::MotionResult result;
	params.from = elevated;
	params.motion = p_direction * real_t(p_params.wall_jump_check_distance);
	params.margin = p_params.wall_jump_check_radius;
	return PhysicsServer3D::get_singleton()->body_test_motion(pawn->get_rid(), params, &result);
}

void Box3DPawnBody::_append_unique_wall_probe_dir(LocalVector<Vector3> &r_directions, Vector3 p_direction) {
	p_direction.y = 0.0f;
	if (double(p_direction.length_squared()) <= 0.001) {
		return;
	}
	p_direction = p_direction.normalized();
	for (uint32_t i = 0; i < r_directions.size(); i++) {
		if (double(r_directions[i].dot(p_direction)) > 0.98) {
			return;
		}
	}
	r_directions.push_back(p_direction);
}

const scrap::ILadderVolume *Box3DPawnBody::get_ladder_by_id(int32_t p_ladder_id) const {
	if (p_ladder_id <= 0 || ladders == nullptr) {
		return nullptr;
	}
	for (uint32_t i = 0; i < ladders->size(); i++) {
		if ((*ladders)[i].id == p_ladder_id) {
			return &(*ladders)[i];
		}
	}
	return nullptr;
}
