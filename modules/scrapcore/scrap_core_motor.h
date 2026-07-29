/**************************************************************************/
/*  scrap_core_motor.h                                                    */
/**************************************************************************/

#pragma once

#include "core/object/ref_counted.h"

// Script-visible handle for the native ScrapCore movement motor
// (docs/SCRAPCORE_ENGINE_MODULE_PLAN-2026-07-28.md in the game repo).
//
// GATE-A STUB: proves the module skeleton + the external-path core compile
// (D1) link into the engine -- pack_version() reads a real core symbol and
// motor_smoke() runs one real simulate() tick against a local no-op pawn stub.
// The D2 tick surface (setup / simulate(packed command) / state_packed / ...)
// and the Box3DPawnBody adapter are Gate B; nothing here is that surface yet.
class ScrapCoreMotor : public RefCounted {
	GDCLASS(ScrapCoreMotor, RefCounted);

protected:
	static void _bind_methods();

public:
	// scrap::MovementState::PACK_VERSION straight from the core headers -- the
	// game-side smoke asserts 18, proving the engine compiled the SAME contract
	// the game repo's traces certify.
	int pack_version() const;

	// Constructs core state/params, runs one MovementMotor::simulate() tick
	// against a trivial local IPawnBody stub, and reports whether the state
	// came back finite. Exercises the linked motor object code end to end.
	bool motor_smoke() const;

	ScrapCoreMotor() = default;
};
