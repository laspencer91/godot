/**************************************************************************/
/*  scrap_core_motor.h                                                    */
/**************************************************************************/

#pragma once

#include "box3d_pawn_body.h"
#include "core/object/ref_counted.h"

#include <scrapcore/movement_event.h>
#include <scrapcore/movement_motor.h>

// Script-visible handle for the native ScrapCore movement motor
// (docs/SCRAPCORE_ENGINE_MODULE_PLAN-2026-07-28.md in the game repo).
//
// D2 -- ONE boundary crossing per tick, byte-exact where it crosses:
// MovementState/MovementParams live native-side; per tick GDScript hands in the
// 32-byte packed InputCommand (Phase-1 codec) and reads back only what script
// needs. Snapshots cross as the 196-byte v18 packed form. No per-field
// Dictionary traffic in the tick path (setup()'s params Dictionary and
// drain_events() are the deliberate cold-path exceptions).
class ScrapCoreMotor : public RefCounted {
	GDCLASS(ScrapCoreMotor, RefCounted);

	scrap::MovementState state;
	scrap::MovementParams params;
	scrap::MovementMotor motor;
	scrap::MovementResult result;
	Box3DPawnBody body;
	LocalVector<ScrapLadderVolume> ladders;
	LocalVector<scrap::MovementEvent> pending_events;
	bool ready = false;

	bool _apply_params_dict(const Dictionary &p_params);

protected:
	static void _bind_methods();

public:
	// Once per body (re-callable for live tuning): binds the CharacterBody3D
	// this motor drives (must be inside the tree) and fills MovementParams from
	// a name->value Dictionary (MovementConfig.to_param_dict(), game side).
	// Unknown keys AND missing keys are HARD errors: the boundary demands the
	// complete param set, because silence is how a config field drifts out of
	// the native motor. Seeds state from the live pawn exactly like the
	// controller's _ready (position/velocity/yaw, stand height/eye, on-floor),
	// so a simulate() before any reset_state() starts where the pawn stands.
	void setup(Object *p_body, const Dictionary &p_params);

	// Map-load ladder wiring. Axes arrive pre-resolved from LadderVolume
	// (outward_normal()/side_dir()); extents stay double end to end. CALL ORDER
	// IS CONTRACT: register in node-path-sorted order (the GDScript's
	// _sorted_ladders walk) -- probe iteration preserves registration order.
	// id follows PlayerPawn._ladder_runtime_id: an authored id > 0 is kept
	// verbatim; id <= 0 maps to registration index + 1.
	void register_ladder(int p_id, const Vector3 &p_position, const Vector3 &p_outward_normal, const Vector3 &p_side_dir, double p_height, double p_half_width, double p_attach_depth);
	void clear_ladders();

	// --- Out-of-motor state inputs (the controller-side writes) --------------
	// Not packed; injected before sim from the authoritative equipped weapon
	// (player_controller.gd _prepare_movement_state_for_sim). Call on change.
	void set_ads_move_speed_mult(double p_mult);
	// The per-shot kick (player_controller.gd _apply_recoil_kick_to): offset
	// AND debt take the kick; recovery rate re-stamped; fire window re-armed.
	void add_recoil_kick(double p_pitch, double p_yaw, double p_recovery_rate);
	// Snapshot-time slot stamp (player_controller.gd snapshot_state).
	void set_active_slot(int p_slot);

	// The hot call: unpack the 32-byte wire command (rejecting a NaN/Inf
	// payload exactly like the wire ingest guard), run one fixed tick.
	void simulate(int p_tick, double p_delta, const PackedByteArray &p_command);

	Vector3 get_position() const;
	Vector3 get_velocity() const;

	// Reconcile path: the v18 packed MovementState both ways. reset_state
	// implements PlayerPawn.teleport_to_state (mailbox cleared, physics
	// interpolation reset) and, when p_refresh_ground_contact is true, the
	// replay entry's contact rebuild -- snapped position/velocity are copied
	// back into state like player_prediction_runner.gd:91 does. It also clears
	// undrained events: a state reset discards the prediction they belonged to.
	PackedByteArray state_packed() const;
	void reset_state(const PackedByteArray &p_packed, bool p_refresh_ground_contact = false);

	// Presentation mailbox: exact step/snap feet pops since last drained
	// (PlayerPawn.consume_collision_step_delta_y; never simulation state).
	double consume_collision_step_delta_y();

	// Rare; empty most ticks. Drains every event since the last call, each as
	// {kind, tick, input_seq, data, replay_safe, network_relevant} with data
	// keyed like player_movement_event.gd's per-kind payloads.
	Array drain_events();

	// scrap::MovementState::PACK_VERSION from the compiled core (18).
	int pack_version() const;
	// One simulate() tick against a local no-op stub; true if state stays
	// finite. Gate-A leftover, kept as a link-level self-test.
	bool motor_smoke() const;

	ScrapCoreMotor() = default;
};
