/**************************************************************************/
/*  csg_debug_counters.cpp                                                */
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

#include "csg_debug_counters.h"

#ifdef DEV_ENABLED

#include "core/templates/safe_refcount.h"

namespace {

struct CSGDebugCounterStorage {
	SafeNumeric<uint64_t> local_primitive_brush_packs;
	SafeNumeric<uint64_t> leaf_manifold_repacks;
	SafeNumeric<uint64_t> transformed_wrapper_constructions;
	SafeNumeric<uint64_t> expression_node_reconstructions;
	SafeNumeric<uint64_t> batch_boolean_calls;
	SafeNumeric<uint64_t> operation_switch_flushes;
	SafeNumeric<uint64_t> root_materializations;
	SafeNumeric<uint64_t> non_root_materializations;
	SafeNumeric<uint64_t> uv_finalizations;
	SafeNumeric<uint64_t> tangent_finalizations;
	SafeNumeric<uint64_t> collision_rebuilds;
};

CSGDebugCounterStorage counters;

} // namespace

void CSGDebugCounters::reset() {
	counters.local_primitive_brush_packs.set(0);
	counters.leaf_manifold_repacks.set(0);
	counters.transformed_wrapper_constructions.set(0);
	counters.expression_node_reconstructions.set(0);
	counters.batch_boolean_calls.set(0);
	counters.operation_switch_flushes.set(0);
	counters.root_materializations.set(0);
	counters.non_root_materializations.set(0);
	counters.uv_finalizations.set(0);
	counters.tangent_finalizations.set(0);
	counters.collision_rebuilds.set(0);
}

CSGDebugCounters CSGDebugCounters::get() {
	CSGDebugCounters result;
	result.local_primitive_brush_packs = counters.local_primitive_brush_packs.get();
	result.leaf_manifold_repacks = counters.leaf_manifold_repacks.get();
	result.transformed_wrapper_constructions = counters.transformed_wrapper_constructions.get();
	result.expression_node_reconstructions = counters.expression_node_reconstructions.get();
	result.batch_boolean_calls = counters.batch_boolean_calls.get();
	result.operation_switch_flushes = counters.operation_switch_flushes.get();
	result.root_materializations = counters.root_materializations.get();
	result.non_root_materializations = counters.non_root_materializations.get();
	result.uv_finalizations = counters.uv_finalizations.get();
	result.tangent_finalizations = counters.tangent_finalizations.get();
	result.collision_rebuilds = counters.collision_rebuilds.get();
	return result;
}

void CSGDebugCounters::count_local_primitive_brush_pack() {
	counters.local_primitive_brush_packs.increment();
}

void CSGDebugCounters::count_leaf_manifold_repack() {
	counters.leaf_manifold_repacks.increment();
}

void CSGDebugCounters::count_transformed_wrapper_construction() {
	counters.transformed_wrapper_constructions.increment();
}

void CSGDebugCounters::count_expression_node_reconstruction() {
	counters.expression_node_reconstructions.increment();
}

void CSGDebugCounters::count_batch_boolean_call() {
	counters.batch_boolean_calls.increment();
}

void CSGDebugCounters::count_operation_switch_flush() {
	counters.operation_switch_flushes.increment();
}

void CSGDebugCounters::count_root_materialization() {
	counters.root_materializations.increment();
}

void CSGDebugCounters::count_non_root_materialization() {
	counters.non_root_materializations.increment();
}

void CSGDebugCounters::count_uv_finalization() {
	counters.uv_finalizations.increment();
}

void CSGDebugCounters::count_tangent_finalization() {
	counters.tangent_finalizations.increment();
}

void CSGDebugCounters::count_collision_rebuild() {
	counters.collision_rebuilds.increment();
}

#endif // DEV_ENABLED
