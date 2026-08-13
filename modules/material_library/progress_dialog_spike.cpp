/**************************************************************************/
/*  progress_dialog_spike.cpp                                             */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/

#include "progress_dialog_spike.h"

#include "core/os/os.h"
#include "core/os/thread.h"
#include "core/string/print_string.h"
#include "core/variant/array.h"
#include "servers/display/display_server.h"

// POD job description handed to the ticker thread. The thread talks to
// DisplayServer's progress mutators only — they are documented safe from any
// thread, and exercising exactly that contract is the point of the spike.
struct SpikeTickerJob {
	int dialog_id = 0;
	int total_steps = 0;
	int step_interval_ms = 100;
	volatile bool stop = false;

	// Written by the ticker, read by the main thread after the join.
	int steps_done = 0;
	int64_t cancel_observed_at_step = -1;
	uint64_t cancel_observed_usec = 0;
	uint64_t started_usec = 0;
};

static void _spike_ticker_func(void *p_ud) {
	SpikeTickerJob *job = (SpikeTickerJob *)p_ud;
	DisplayServer *ds = DisplayServer::get_singleton();
	job->started_usec = OS::get_singleton()->get_ticks_usec();

	bool was_cancelled = false;
	for (int step = 1; step <= job->total_steps && !job->stop; step++) {
		ds->progress_dialog_set_progress(job->dialog_id, (uint64_t)step, (uint64_t)job->total_steps);
		ds->progress_dialog_set_lines(job->dialog_id,
				vformat("Worker step %d of %d (main thread is blocked)", step, job->total_steps),
				vformat("Elapsed: %d ms", (int)((OS::get_singleton()->get_ticks_usec() - job->started_usec) / 1000)));
		job->steps_done = step;

		bool cancelled = ds->progress_dialog_is_cancelled(job->dialog_id);
		if (cancelled != was_cancelled) {
			was_cancelled = cancelled;
			if (cancelled) {
				job->cancel_observed_at_step = step;
				job->cancel_observed_usec = OS::get_singleton()->get_ticks_usec();
				print_line(vformat("ProgressDialogSpike: is_cancelled flipped TRUE at step %d (%d ms in) — observed from the worker thread.",
						step, (int)((job->cancel_observed_usec - job->started_usec) / 1000)));
				// Keep ticking anyway: the spike wants to show updates continue
				// to land after Cancel until the job actually winds down.
			}
		}
		OS::get_singleton()->delay_usec((uint64_t)job->step_interval_ms * 1000);
	}
}

Dictionary ProgressDialogSpike::run_blocking_test(int p_total_steps, int p_block_seconds) {
	Dictionary report;
	DisplayServer *ds = DisplayServer::get_singleton();

	report["feature"] = ds->has_feature(DisplayServerEnums::FEATURE_NATIVE_PROGRESS_DIALOG);
	if (!ds->has_feature(DisplayServerEnums::FEATURE_NATIVE_PROGRESS_DIALOG)) {
		print_line("ProgressDialogSpike: FEATURE_NATIVE_PROGRESS_DIALOG not available (headless or stock binary); nothing to test.");
		last_report = report;
		return report;
	}

	const int total_steps = MAX(1, p_total_steps);
	const int block_seconds = CLAMP(p_block_seconds, 1, 120);

	int id = ds->create_progress_dialog(
			"ProgressDialogSpike — go/no-go",
			"Starting...",
			"",
			DisplayServerEnums::PROGRESS_DIALOG_FLAG_AUTO_TIME,
			Callable());
	report["dialog_id"] = id;
	if (id == DisplayServerEnums::INVALID_PROGRESS_DIALOG_ID) {
		print_line("ProgressDialogSpike: create_progress_dialog returned 0 — FAIL.");
		last_report = report;
		return report;
	}

	SpikeTickerJob job;
	job.dialog_id = id;
	job.total_steps = total_steps;

	print_line(vformat("ProgressDialogSpike: dialog %d up; blocking the MAIN thread for %d s while a worker ticks %d steps.", id, block_seconds, total_steps));
	print_line("ProgressDialogSpike: PASS = the dialog's numbers and time-remaining keep advancing while this app is unresponsive. Click Cancel to test the poll.");

	Thread ticker;
	ticker.start(_spike_ticker_func, &job);

	// The deliberate freeze: no Main::iteration, no MessageQueue flush — the
	// same starvation the post-drop streaming phase produces.
	uint64_t block_start = OS::get_singleton()->get_ticks_usec();
	OS::get_singleton()->delay_usec((uint64_t)block_seconds * 1000000);
	uint64_t block_end = OS::get_singleton()->get_ticks_usec();

	job.stop = true;
	ticker.wait_to_finish();

	bool cancelled = ds->progress_dialog_is_cancelled(id);
	ds->delete_progress_dialog(id);
	// Idempotence check (the delete-on-finish / delete-on-drain race must be harmless).
	ds->delete_progress_dialog(id);

	report["blocked_ms"] = (int64_t)((block_end - block_start) / 1000);
	report["steps_done"] = job.steps_done;
	report["cancelled"] = cancelled;
	report["cancel_observed_at_step"] = job.cancel_observed_at_step;
	if (job.cancel_observed_at_step >= 0) {
		report["cancel_observed_ms"] = (int64_t)((job.cancel_observed_usec - job.started_usec) / 1000);
	}

	print_line(vformat("ProgressDialogSpike: done. steps_done=%d blocked_ms=%d cancelled=%s%s",
			job.steps_done, (int)((block_end - block_start) / 1000), cancelled ? "true" : "false",
			job.cancel_observed_at_step >= 0 ? vformat(" (observed at step %d)", job.cancel_observed_at_step) : String()));

	last_report = report;
	return report;
}

Dictionary ProgressDialogSpike::get_report() const {
	return last_report;
}

void ProgressDialogSpike::_bind_methods() {
	ClassDB::bind_method(D_METHOD("run_blocking_test", "total_steps", "block_seconds"), &ProgressDialogSpike::run_blocking_test, DEFVAL(150), DEFVAL(15));
	ClassDB::bind_method(D_METHOD("get_report"), &ProgressDialogSpike::get_report);
}
