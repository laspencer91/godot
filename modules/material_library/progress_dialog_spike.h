/**************************************************************************/
/*  progress_dialog_spike.h                                               */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/

#pragma once

#include "core/object/class_db.h"
#include "core/object/object.h"
#include "core/variant/dictionary.h"

// Go/no-go SPIKE for the native progress dialog primitive (the DragOutSpike
// precedent): proves — or disproves — that a dialog created through
// DisplayServer.create_progress_dialog keeps painting and advancing while the
// engine's main loop is deliberately blocked, which is the load-bearing
// assumption of the whole feature (native-progress-dialog.md section 0).
//
// run_blocking_test():
//  1. Creates a dialog (AUTOTIME on, marquee off).
//  2. Starts a worker thread that ticks set_progress / set_lines every 100 ms
//     and logs every is_cancelled() transition it observes.
//  3. BLOCKS the main thread with OS::delay_msec for p_block_seconds — no
//     Main::iteration, no MessageQueue, exactly like the post-drop freeze.
//  4. Joins the worker, destroys the dialog, returns a report Dictionary.
//
// PASS: the dialog's numbers and "time remaining" visibly advance during the
// block; clicking Cancel flips is_cancelled within ~100 ms in the log.
// FAIL: the dialog freezes with the app — the marshalling design is wrong.
//
// Everything is throwaway: single run at a time, no error recovery beyond
// what a verdict needs. No-op / empty report on non-Windows or headless.
class ProgressDialogSpike : public Object {
	GDCLASS(ProgressDialogSpike, Object);

	Dictionary last_report;

protected:
	static void _bind_methods();

public:
	// Blocks the calling (main) thread for p_block_seconds while a worker
	// drives the dialog through p_total_steps. Returns the report.
	Dictionary run_blocking_test(int p_total_steps = 150, int p_block_seconds = 15);

	Dictionary get_report() const;
};
