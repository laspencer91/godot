/**************************************************************************/
/*  csg_debug_counters.h                                                  */
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

#ifdef DEV_ENABLED

#include "core/typedefs.h"

struct CSGDebugCounters {
	uint64_t local_primitive_brush_packs = 0;
	uint64_t leaf_manifold_repacks = 0;
	uint64_t transformed_wrapper_constructions = 0;
	uint64_t expression_node_reconstructions = 0;
	uint64_t batch_boolean_calls = 0;
	uint64_t operation_switch_flushes = 0;
	uint64_t root_materializations = 0;
	uint64_t non_root_materializations = 0;
	uint64_t uv_finalizations = 0;
	uint64_t tangent_finalizations = 0;
	uint64_t collision_rebuilds = 0;

	static void reset();
	static CSGDebugCounters get();

	static void count_local_primitive_brush_pack();
	static void count_leaf_manifold_repack();
	static void count_transformed_wrapper_construction();
	static void count_expression_node_reconstruction();
	static void count_batch_boolean_call();
	static void count_operation_switch_flush();
	static void count_root_materialization();
	static void count_non_root_materialization();
	static void count_uv_finalization();
	static void count_tangent_finalization();
	static void count_collision_rebuild();
};

#endif // DEV_ENABLED
