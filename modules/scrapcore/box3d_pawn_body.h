/**************************************************************************/
/*  box3d_pawn_body.h                                                     */
/**************************************************************************/

#pragma once

// The Gate-B adapter (plan D3): scrap::IPawnBody answered by LIVE physics --
// Box3DCharacterMover for the collide-and-slide plus PhysicsServer3D sweeps and
// space-state rays for the probes. Faithful C++ translation of
//   src/player/box3d_character_collision_backend.gd  (mover, normals, snap,
//       stand check, capsule resize, wall-jump probes, mantle sweeps)
//   src/player/player_pawn.gd                        (node<->state pushes,
//       mantle/ladder probes, ray exclusions)
//   src/world/ladder_volume.gd                       (ScrapLadderVolume math)
// Translation discipline is Phase 1's: Godot's exact op order and float widths
// (real_t vector math, double scalars, comparisons where GDScript compares).

#include "core/templates/local_vector.h"
#include "modules/box3d_physics/box3d_character_mover.h"
#include "scene/3d/physics/character_body_3d.h"
#include "scene/3d/physics/collision_shape_3d.h"

#include <scrapcore/pawn_body.h>

// Mirror of src/world/ladder_volume.gd's geometry math over registered data
// (id, base position, outward + side axes, extents). Pure functions of the
// stored geometry, exactly like the node's -- the motor's ladder path stays
// deterministic. Axes arrive pre-resolved from the game side (the node's
// outward_normal()/side_dir()), so cardinal and free-form facings both work.
class ScrapLadderVolume final : public scrap::ILadderVolume {
public:
	int32_t id = 0;
	Vector3 position;
	Vector3 outward; // unit, XZ
	Vector3 side; // unit, XZ
	double ladder_height = 3.0;
	double ladder_half_width = 0.45;
	double ladder_attach_depth = 0.55;

	scrap::Vec3 outward_normal() const override;
	scrap::Vec3 climb_normal(const scrap::Vec3 &p_world_point) const override;
	scrap::Vec3 climb_side(const scrap::Vec3 &p_world_point) const override;
	scrap::Vec3 climb_side_for_normal(const scrap::Vec3 &p_normal) const override;
	scrap::Vec3 local_point(const scrap::Vec3 &p_world_point) const override;
	bool accepts_position(const scrap::Vec3 &p_world_point, scrap::Scalar p_margin) const override;
	scrap::Scalar height() const override { return ladder_height; }
	scrap::Scalar attach_depth() const override { return ladder_attach_depth; }
	scrap::Vec3 global_position() const override;

	// Pawn-side probe helper (LadderVolume.accepts_top_descent) -- not part of
	// the motor-facing ILadderVolume; check_ladder_opportunity calls it.
	bool accepts_top_descent(const Vector3 &p_world_point, double p_lateral_margin, double p_top_band, double p_back_reach) const;

	// Node-typed variants of the geometry math (the GDScript computes these
	// with Vector3s; the ILadderVolume overrides above wrap them).
	Vector3 climb_normal_gd(const Vector3 &p_world_point) const;
	Vector3 climb_side_for_normal_gd(const Vector3 &p_normal) const;
	Vector3 local_point_gd(const Vector3 &p_world_point) const;
};

class Box3DPawnBody final : public scrap::IPawnBody {
public:
	// Mirror of PlayerCollisionResult: the cached contact state every
	// is_on_floor / is_on_wall / get_wall_normal read serves from.
	// step_delta_y is double like the GDScript's bare float fields
	// (player_collision_result.gd / the pawn's mailbox accumulator).
	struct CollisionResult {
		Vector3 position;
		Vector3 velocity;
		bool on_floor = false;
		bool on_wall = false;
		Vector3 floor_normal;
		Vector3 wall_normal;
		double step_delta_y = 0.0;
	};

private:
	static constexpr double FLOOR_OVERLAP_PROBE = 0.005;
	static constexpr double SNAP_UPWARD_LIMIT = 0.05;

	CharacterBody3D *pawn = nullptr;
	ObjectID pawn_id;
	CollisionShape3D *pawn_capsule = nullptr;
	ObjectID pawn_capsule_id;
	Ref<Box3DCharacterMover> mover;
	scrap::MovementParams cfg; // _config: refreshed at the calls that pass params
	double collision_height = 1.8;
	double mover_height = 1.8;
	double capsule_radius = 0.35;
	CollisionResult last_result;
	double pending_step_delta_y = 0.0; // presentation mailbox (unused by the motor)

	const LocalVector<ScrapLadderVolume> *ladders = nullptr; // owned by ScrapCoreMotor

	// --- backend internals (box3d_character_collision_backend.gd) ---
	void _prepare_mover(const scrap::MovementParams &p_params);
	void _sync_mover_settings(const scrap::MovementParams &p_params);
	void _update_exclusions();
	TypedArray<RID> _pawn_body_exclusions() const; // PlayerPawn.pawn_body_exclusions
	HashSet<RID> _pawn_ray_exclusion_set() const; // pawn_ray_exclusions as a ray-exclude set
	Vector3 _node_origin_to_feet_offset() const;
	Vector3 _node_position_to_feet(const Vector3 &p_node_position) const;
	Vector3 _feet_position_to_node(const Vector3 &p_feet_position) const;
	bool _snap_down(const Vector3 &p_feet_position, double p_distance, Vector3 &r_position, Vector3 &r_normal) const;
	Vector3 _floor_normal_at(const Vector3 &p_feet_position) const;
	Vector3 _wall_normal_from_planes(const Array &p_planes, const Vector3 &p_pre_move_velocity, double p_floor_max_angle) const;
	double _apply_capsule_dimensions(double p_height, double p_radius); // PlayerPawn.apply_capsule_dimensions
	void _set_capsule_height(double p_height, double p_radius);
	bool _wall_jump_face_reaches_min_height(const Vector3 &p_direction, const scrap::MovementParams &p_params) const;
	static void _append_unique_wall_probe_dir(LocalVector<Vector3> &r_directions, Vector3 p_direction);
	int32_t _ladder_runtime_id(const ScrapLadderVolume &p_ladder) const { return p_ladder.id; }

public:
	// Mirrors Box3DCharacterCollisionBackend.configure(): binds the body node,
	// finds its capsule child, sets up the mover, and seeds the initial cached
	// contact (floor probe when not moving upward).
	void configure(CharacterBody3D *p_pawn, const scrap::MovementParams &p_params);
	void set_ladders(const LocalVector<ScrapLadderVolume> *p_ladders) { ladders = p_ladders; }
	// A null collider is tolerated like the reference (feet offset 0, clamped
	// fallback dims), so configuration requires only the body.
	bool is_configured() const { return pawn != nullptr; }
	CharacterBody3D *get_pawn() const { return pawn; }
	// Lifetime guard for the caller's entry points: the cached raw pointers are
	// only dereferenced behind this (ObjectDB validity + in-tree; a freed
	// capsule degrades to the tolerated-null path rather than dangling).
	bool body_valid();
	double consume_step_delta_y(); // presentation mailbox drain (PlayerPawn.consume_collision_step_delta_y)
	// Backend.refresh_ground_contact_after_teleport: rebuild floor/wall contact
	// at the current (teleported) position -- probe, snap-down fallback, plane
	// collide -- mirroring the result onto the live body. The reconcile path's
	// replay entry (player_prediction_runner.gd:91) calls this.
	bool refresh_ground_contact_after_teleport(const scrap::MovementParams &p_params);
	// PlayerPawn.teleport_to_state: a replay/teleport starts a new collision
	// solve timeline -- do not carry an unpresented stair delta into it.
	void teleport_to_state(const scrap::MovementState &p_state, const scrap::MovementParams &p_params);

	// --- scrap::IPawnBody ---
	void apply_movement_state(const scrap::MovementState &p_state, const scrap::MovementParams &p_params) override;
	void move_body(const scrap::Vec3 &p_next_velocity, const scrap::MovementParams &p_params) override;
	scrap::Vec3 get_position() const override;
	scrap::Vec3 get_velocity() const override;
	void set_velocity(const scrap::Vec3 &p_velocity) override;
	bool is_grounded() const override;
	bool is_on_floor() const override;
	bool is_on_wall() const override;
	scrap::Vec3 get_wall_normal() const override;
	bool can_stand_up(scrap::Scalar p_current_height, const scrap::MovementParams &p_params) const override;
	void set_capsule_for_pose(scrap::Scalar p_height, int32_t p_pose, const scrap::MovementParams &p_params) override;
	bool raycast_blocked(const scrap::Vec3 &p_from, const scrap::Vec3 &p_to, uint32_t p_mask) const override;
	scrap::MantleProbe check_mantle_opportunity(const scrap::Vec2 &p_input, const scrap::MovementState &p_state, const scrap::MovementParams &p_params) const override;
	scrap::MantleProbe swept_mantle_landing(const scrap::Vec3 &p_ledge_point, const scrap::Vec3 &p_target_landing) const override;
	scrap::LadderProbe check_ladder_opportunity(const scrap::Vec2 &p_input, const scrap::MovementState &p_state, const scrap::MovementParams &p_params) const override;
	scrap::WallJumpProbe check_wall_jump(const scrap::MovementState &p_state, const scrap::Vec3 &p_wish, const scrap::MovementParams &p_params) const override;
	const scrap::ILadderVolume *get_ladder_by_id(int32_t p_ladder_id) const override;
};
