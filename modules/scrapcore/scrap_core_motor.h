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
	// this motor drives and fills MovementParams from a name->value Dictionary
	// (MovementConfig.to_param_dict(), game side). Unknown keys are a HARD
	// error: silence is how a config field drifts out of the native motor.
	void setup(Object *p_body, const Dictionary &p_params);

	// Map-load ladder wiring. Axes arrive pre-resolved from LadderVolume
	// (outward_normal()/side_dir()), ids follow the game's authored-or-index
	// runtime-id policy.
	void register_ladder(int p_id, const Vector3 &p_position, const Vector3 &p_outward_normal, const Vector3 &p_side_dir, float p_height, float p_half_width, float p_attach_depth);
	void clear_ladders();

	// The hot call: unpack the 32-byte wire command, run one fixed tick.
	void simulate(int p_tick, double p_delta, const PackedByteArray &p_command);

	Vector3 get_position() const;
	Vector3 get_velocity() const;

	// Reconcile path: the v18 packed MovementState both ways.
	PackedByteArray state_packed() const;
	void reset_state(const PackedByteArray &p_packed);

	// Rare; empty most ticks. Drains every event since the last call.
	Array drain_events();

	// scrap::MovementState::PACK_VERSION from the compiled core (18).
	int pack_version() const;
	// One simulate() tick against a local no-op stub; true if state stays
	// finite. Gate-A leftover, kept as a link-level self-test.
	bool motor_smoke() const;

	ScrapCoreMotor() = default;
};
