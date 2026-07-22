/**************************************************************************/
/*  csg_manifold_cache.h                                                  */
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

#include "csg.h"
#include "csg_evaluation.h"
#include "csg_shape.h"

#include <manifold/manifold.h>

struct CSGManifoldSurfaceRecord {
	CSGOriginToken origin_token = 0;
	CSGSurfaceKey surface;
	Ref<Material> source_material;
};

struct CSGShape3D::ManifoldCache {
	CSGBrush *local_brush = nullptr;
	// Clean Manifold values keep their exact CSG-node handles. In particular,
	// subtree_manifold must only be evaluated through a copy.
	manifold::Manifold local_manifold;
	manifold::Manifold transformed_manifold;
	manifold::Manifold subtree_manifold;
	// One contiguous origin-token range is retained for the whole schema
	// generation, including across geometry-only leaf rebuilds.
	CSGOriginToken origin_base = 0;
	uint32_t origin_count = 0;
	uint32_t origin_schema_generation = 0;
	Vector<CSGManifoldSurfaceRecord> surface_records;

	// Root materialization snapshot. One compact pair is retained per output
	// triangle; keys are stored once per origin token.
	HashMap<CSGOriginToken, CSGSurfaceKey> result_surface_keys;
	Vector<CSGManifoldResultTriangle> result_triangles;

	bool local_manifold_dirty = true;
	bool transformed_manifold_dirty = true;
	bool subtree_manifold_dirty = true;
	bool materialization_dirty = true;
	bool subtree_empty = true;
	bool subtree_empty_valid = false;

	~ManifoldCache() {
		if (local_brush) {
			memdelete(local_brush);
		}
	}
};
