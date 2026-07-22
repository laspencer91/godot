/**************************************************************************/
/*  csg_evaluation_scheduler.cpp                                          */
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

#include "csg_evaluation_scheduler.h"

#ifdef DEV_ENABLED
#include "csg_debug_counters.h"
#endif // DEV_ENABLED

SafeFlag CSGEvaluationScheduler::force_synchronous;

void CSGEvaluationScheduler::_run(void *p_job) {
	Job *job = static_cast<Job *>(p_job);
	// The worker boundary is deliberately narrow: only the detached input copy
	// and the resulting snapshot are touched here.
	job->result = csg_build_snapshot(job->inputs);
	job->done.set();
}

void CSGEvaluationScheduler::_apply_quality(CSGEvaluationInputs &r_inputs, CSGEvalQuality p_quality) {
	r_inputs.settings.want_render = true;
	if (p_quality == CSGEvalQuality::INTERACTIVE) {
		r_inputs.settings.want_collision = false;
		r_inputs.settings.calculate_tangents = false;
	}
}

void CSGEvaluationScheduler::_advance_requested_generation_locked() {
	requested_generation++;
	if (requested_generation == 0) {
		requested_generation = 1;
	}
}

void CSGEvaluationScheduler::_launch_locked(CSGEvaluationInputs &&p_inputs) {
	DEV_ASSERT(running_job == nullptr);
	running_job = memnew(Job(std::move(p_inputs)));
	running_task = WorkerThreadPool::INVALID_TASK_ID;

	WorkerThreadPool *pool = WorkerThreadPool::get_singleton();
	if (force_synchronous.is_set() || !pool) {
		_run(running_job);
		return;
	}

	running_task = pool->add_native_task(&_run, running_job, true, SNAME("csg_eval"));
	if (running_task == WorkerThreadPool::INVALID_TASK_ID) {
		_run(running_job);
	}
}

void CSGEvaluationScheduler::request(CSGEvaluationInputs &&p_inputs, CSGEvalQuality p_quality) {
#ifdef DEV_ENABLED
	CSGDebugCounters::count_scheduler_request();
#endif // DEV_ENABLED

	MutexLock lock(mutex);
	_advance_requested_generation_locked();
	p_inputs.request_generation = requested_generation;

	if (running_job || has_pending) {
		// Latest inputs always win, but once FINAL is pending an interactive
		// request may not downgrade its quality.
		if (has_pending && pending_quality == CSGEvalQuality::FINAL && p_quality == CSGEvalQuality::INTERACTIVE) {
			p_quality = CSGEvalQuality::FINAL;
		}
		_apply_quality(p_inputs, p_quality);
		pending_inputs = std::move(p_inputs);
		pending_quality = p_quality;
		has_pending = true;
#ifdef DEV_ENABLED
		CSGDebugCounters::count_scheduler_coalesce();
#endif // DEV_ENABLED
		return;
	}

	_apply_quality(p_inputs, p_quality);
	_launch_locked(std::move(p_inputs));
}

bool CSGEvaluationScheduler::try_take_completed(CSGEvaluationSnapshot &r_snapshot) {
	MutexLock lock(mutex);
	if (!running_job || !running_job->done.is_set()) {
		return false;
	}

	if (running_task != WorkerThreadPool::INVALID_TASK_ID) {
		WorkerThreadPool *pool = WorkerThreadPool::get_singleton();
		ERR_FAIL_NULL_V(pool, false);
		const Error err = pool->wait_for_task_completion(running_task);
		ERR_FAIL_COND_V(err != OK, false);
	}

	r_snapshot = std::move(running_job->result);
	memdelete(running_job);
	running_job = nullptr;
	running_task = WorkerThreadPool::INVALID_TASK_ID;
#ifdef DEV_ENABLED
	CSGDebugCounters::count_scheduler_completion();
#endif // DEV_ENABLED

	return true;
}

void CSGEvaluationScheduler::launch_pending() {
	MutexLock lock(mutex);
	if (running_job || !has_pending) {
		return;
	}
	CSGEvaluationInputs next_inputs = std::move(pending_inputs);
	has_pending = false;
	_launch_locked(std::move(next_inputs));
}

bool CSGEvaluationScheduler::has_work() const {
	MutexLock lock(mutex);
	return running_job || has_pending;
}

void CSGEvaluationScheduler::cancel_and_flush() {
	Job *job = nullptr;
	WorkerThreadPool::TaskID task = WorkerThreadPool::INVALID_TASK_ID;
	{
		MutexLock lock(mutex);
		has_pending = false;
		pending_inputs = CSGEvaluationInputs();
		job = running_job;
		task = running_task;
		running_job = nullptr;
		running_task = WorkerThreadPool::INVALID_TASK_ID;
	}

	if (!job) {
		return;
	}
	if (task != WorkerThreadPool::INVALID_TASK_ID) {
		WorkerThreadPool *pool = WorkerThreadPool::get_singleton();
		if (pool) {
			pool->wait_for_task_completion(task);
		}
	}
	memdelete(job);
}

void CSGEvaluationScheduler::prepare_for_synchronous_evaluation() {
	WorkerThreadPool::TaskID task = WorkerThreadPool::INVALID_TASK_ID;
	{
		MutexLock lock(mutex);
		if (running_job) {
			task = running_task;
		}
	}

	// A copied Manifold can still share the lazy operation cache. Do not let a
	// synchronous materialization evaluate it concurrently with the worker.
	if (task != WorkerThreadPool::INVALID_TASK_ID) {
		WorkerThreadPool *pool = WorkerThreadPool::get_singleton();
		if (pool) {
			pool->wait_for_task_completion(task);
		}
	}

	MutexLock lock(mutex);
	if (running_task == task) {
		running_task = WorkerThreadPool::INVALID_TASK_ID;
	}
	has_pending = false;
	pending_inputs = CSGEvaluationInputs();
	_advance_requested_generation_locked();
	published_generation = requested_generation;
}

void CSGEvaluationScheduler::invalidate_requests() {
	MutexLock lock(mutex);
	has_pending = false;
	pending_inputs = CSGEvaluationInputs();
	_advance_requested_generation_locked();
}

uint64_t CSGEvaluationScheduler::get_requested_generation() const {
	MutexLock lock(mutex);
	return requested_generation;
}

uint64_t CSGEvaluationScheduler::get_published_generation() const {
	MutexLock lock(mutex);
	return published_generation;
}

bool CSGEvaluationScheduler::is_requested_generation(uint64_t p_generation) const {
	MutexLock lock(mutex);
	return p_generation == requested_generation;
}

void CSGEvaluationScheduler::mark_published(uint64_t p_generation) {
	MutexLock lock(mutex);
	if (p_generation == requested_generation) {
		published_generation = p_generation;
	}
}

void CSGEvaluationScheduler::mark_stale_drop() {
#ifdef DEV_ENABLED
	CSGDebugCounters::count_scheduler_stale_drop();
#endif // DEV_ENABLED
}

void CSGEvaluationScheduler::set_force_synchronous(bool p_force) {
	force_synchronous.set_to(p_force);
}

bool CSGEvaluationScheduler::is_force_synchronous() {
	return force_synchronous.is_set();
}

CSGEvaluationScheduler::~CSGEvaluationScheduler() {
	cancel_and_flush();
}
