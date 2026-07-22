/**************************************************************************/
/*  csg_evaluation_scheduler.h                                            */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#pragma once

#include "csg_evaluation.h"

#include "core/object/worker_thread_pool.h"
#include "core/os/mutex.h"

// Per-root owner for detached CSG evaluation. All methods except _run() are
// called on the main thread; the worker receives only its Job value.
class CSGEvaluationScheduler {
	struct Job {
		CSGEvaluationInputs inputs;
		CSGEvaluationSnapshot result;
		SafeFlag done;

		explicit Job(CSGEvaluationInputs &&p_inputs) :
				inputs(std::move(p_inputs)) {}
	};

	uint64_t requested_generation = 0;
	uint64_t published_generation = 0;
	WorkerThreadPool::TaskID running_task = WorkerThreadPool::INVALID_TASK_ID;
	Job *running_job = nullptr;

	bool has_pending = false;
	CSGEvaluationInputs pending_inputs;
	CSGEvalQuality pending_quality = CSGEvalQuality::INTERACTIVE;

	mutable Mutex mutex;

	static SafeFlag force_synchronous;

	static void _run(void *p_job);
	static void _apply_quality(CSGEvaluationInputs &r_inputs, CSGEvalQuality p_quality);
	void _launch_locked(CSGEvaluationInputs &&p_inputs);
	void _advance_requested_generation_locked();

public:
	void request(CSGEvaluationInputs &&p_inputs, CSGEvalQuality p_quality);
	bool try_take_completed(CSGEvaluationSnapshot &r_snapshot);
	void launch_pending();
	bool has_work() const;
	void cancel_and_flush();

	// A synchronous update must finish any worker first, then advances the
	// request epoch so its completed snapshot cannot overwrite the sync result.
	void prepare_for_synchronous_evaluation();
	void invalidate_requests();

	uint64_t get_requested_generation() const;
	uint64_t get_published_generation() const;
	bool is_requested_generation(uint64_t p_generation) const;
	void mark_published(uint64_t p_generation);
	void mark_stale_drop();

	static void set_force_synchronous(bool p_force);
	static bool is_force_synchronous();

	~CSGEvaluationScheduler();
};
