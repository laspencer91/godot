/**************************************************************************/
/*  editor_grid_3d.h                                                      */
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

// Pure, headless grid policy shared by the 3D editor grid renderer, the CSG
// grid-space integration, the edit-domain seam, and their tests.
//
// Nothing in this header depends on RenderingServer, Node3DEditor, viewports,
// or EditorSettings. Callers gather setting values and camera state, hand them
// in as plain parameters, and receive plain values back. This is deliberate:
// it keeps the grid math testable without a renderer or a live editor (see
// GRID-AND-SNAP-IMPLEMENTATION-PLAN.md, sections 5.3, 5.4 and 11).

#include "core/math/color.h"
#include "core/math/plane.h"
#include "core/math/projection.h"
#include "core/math/transform_3d.h"
#include "core/math/vector2.h"
#include "core/math/vector3.h"
#include "core/typedefs.h"

// The centralized translate-snap minimum from the plan (section 5.2). Values
// loaded as zero or negative are normalized to this, because snap-off already
// has an explicit toggle and a zero base step would break the LOD logarithms.
#define EDITOR_GRID_MIN_TRANSLATE_SNAP ((real_t)0.001)

// Target on-screen width (in pixels) of one *base* snap cell at which the
// continuous LOD level (before bias) crosses zero, i.e. the minor mesh step
// equals the base snap step. Below this the grid coarsens; above it, it
// refines (bounded by the division-level min/max settings).
//
// The plan (section 5.4) fixes the *structure* of the LOD -- log base
// primary_grid_steps, division bias, min/max clamp, floor->minor, fraction->
// fade -- but is silent on the exact pixel reference, since the pre-existing
// behavior keyed LOD off raw camera distance (viewport-independent) and the
// plan intentionally re-bases it on projected on-screen density (section 13:
// "an intentional visible behavior change"). The bias setting remains the
// user-facing tuning knob; this constant only sets the neutral point.
#define EDITOR_GRID_DEFAULT_REFERENCE_PIXELS ((real_t)64.0)

// An orthonormal, right-handed work frame plus a separate plane coordinate.
//
// `anchor` is the coordinate origin of the chosen grid space (Local / CSG Root
// / World). `plane_coordinate` is the absolute N coordinate of the plane that
// is currently displayed or edited; an anchor alone cannot both fix an
// absolute lattice and lie on an arbitrary hovered face, so the two are stored
// separately (plan section 2, correction 7).
//
// Invariants (plan section 4, item 11 and section 5.3): u, v, n are unit
// length, mutually orthogonal, right-handed (u x v == n), and finite. Any
// scale, shear, or mirror in a source transform is removed. Units are world
// meters in every grid space.
struct EditorGridFrame3D {
	Vector3 u; // In-plane tangent.
	Vector3 v; // In-plane bitangent (n x u).
	Vector3 n; // Plane normal (semantic outward direction).
	Vector3 anchor; // Coordinate zero of the grid space.
	real_t plane_coordinate = 0.0; // Absolute N coordinate of the drawn/edited plane.

	// The canonical world ground frame: U = +X, N = +Y, V = N x U = -Z, anchored
	// at the world origin on the plane through the origin. Callers use this as
	// the deterministic fallback whenever `from_plane_in_space` fails.
	static EditorGridFrame3D world_xz();

	// Build a frame from a plane hit expressed in world space, oriented by the
	// given space (Local operand / CSG root / world identity). Returns false on
	// degenerate/invalid input; the caller must then fall back to `world_xz()`
	// rather than use a partially built frame.
	//
	// - `p_point_on_plane`   : any world point on the plane (e.g. the hit point).
	// - `p_outward_normal`   : semantic outward normal in world space.
	// - `p_space_to_world`   : transform whose origin is the anchor and whose
	//                          basis supplies deterministic fallback axes.
	// - `p_tangent_hint`     : preferred U direction in world space; projected
	//                          onto the plane. If degenerate, U falls back to
	//                          the space-basis axis least parallel to N.
	static bool from_plane_in_space(
			const Vector3 &p_point_on_plane,
			const Vector3 &p_outward_normal,
			const Transform3D &p_space_to_world,
			const Vector3 &p_tangent_hint,
			EditorGridFrame3D &r_frame);

	// World point -> frame coordinates (U, V, N) measured from `anchor`.
	Vector3 to_coordinates(const Vector3 &p_world) const;
	// Frame coordinates (U, V, N) -> world point. Exact inverse of `to_coordinates`.
	Vector3 to_world(const Vector3 &p_coordinates) const;

	// The point on the displayed plane at U = V = 0 (foot of the perpendicular
	// from `anchor` to the plane).
	Vector3 plane_origin() const;
	// The displayed/edited plane; exactly reconstructs the source plane passed
	// to `from_plane_in_space`.
	Plane plane() const;

	// Absolute in-plane snap of a world point to the U/V lattice about `anchor`.
	// Returns the snapped (U, V) coordinates. Stable across repeated samples and
	// never accumulated from a previous sample (plan invariant 12).
	//
	// Step convention (both snap functions): `p_step <= 0` returns the unsnapped
	// coordinate, matching `Math::snapped(x, 0)`. Deciding *whether* to snap is
	// the caller's job (plan invariant 10); persisted step values are normalized
	// at the mutation boundary by `editor_grid_normalize_translate_snap`.
	Vector2 snap_uv(const Vector3 &p_world, real_t p_step) const;
	// Absolute snap of a world point's N coordinate to the lattice about `anchor`.
	real_t snap_n(const Vector3 &p_world, real_t p_step) const;

	bool is_valid() const; // Finite, orthonormal, right-handed.
};

// Setting-derived inputs to the LOD/layout policy. These mirror the existing
// `editors/3d/*` editor settings, plus the snap-lattice base step. Kept as a
// plain struct so tests supply values directly.
struct EditorGridLodSettings {
	real_t base_translate_snap = 1.0; // The configured base snap step, in meters.
	int primary_grid_steps = 8; // editors/3d/primary_grid_steps.
	real_t division_level_bias = -0.2; // editors/3d/grid_division_level_bias.
	int division_level_min = 0; // editors/3d/grid_division_level_min.
	int division_level_max = 2; // editors/3d/grid_division_level_max.
	int grid_half_extent_cells = 200; // editors/3d/grid_size (renamed local; half-extent in cells).
	real_t reference_pixels = EDITOR_GRID_DEFAULT_REFERENCE_PIXELS; // Neutral base-cell pixel width.

	// A copy with every degenerate field clamped to its guarded fallback:
	// base snap to the supported minimum, primary_grid_steps to >= 2 (so no
	// logarithm divides by zero, plan section 5.4), reference_pixels positive.
	// The LOD policy and the rebuild key normalize through this one place, so a
	// degenerate raw setting can never make the key disagree with the geometry.
	EditorGridLodSettings normalized() const;
};

// Camera/viewport state needed to compute projected on-screen density. All
// pure core-math types, so this can be built without a live Camera3D.
struct EditorGridCameraSample {
	bool orthogonal = false;
	Transform3D camera_transform; // Camera-to-world.
	Projection projection; // Camera projection matrix for either projection type.
	real_t ortho_size = 1.0; // Retained as source telemetry; projection is authoritative.
	real_t viewport_width = 0.0; // In pixels.
	real_t viewport_height = 0.0; // In pixels.

	// Optional world point the grid should center its major cell on instead of
	// the camera origin's in-plane projection -- e.g. the orthographic view-ray/
	// plane intersection. Centering is part of the semantic rebuild key, so a
	// renderer wanting refined centering must supply it HERE as an input; it must
	// not re-center on its own, or the key would stop describing the drawn
	// geometry.
	bool has_focus_point = false;
	Vector3 focus_point;
};

// Result of the continuous LOD selection plus the derived layout quantities.
struct EditorGridLodResult {
	bool valid = false; // False when inputs were unusable; fields are still finite.

	real_t projected_pixels_per_base_step = 0.0; // On-screen width of one base cell.
	real_t level_raw = 0.0; // Continuous level with bias, before min/max clamp.
	int level_min = 0; // Snap-lattice exponent for the configured physical minimum.
	int level_max = 0; // Snap-lattice exponent for the configured physical maximum.
	real_t level_clamped = 0.0; // After clamping to the (rescaled) min/max.
	int level_floor = 0; // Floor of the clamped level -> selects the minor step.
	real_t fade = 0.0; // Fractional part in [0, 1): drives the small/large color fade only.

	real_t minor_step = 0.0; // base_snap * steps^level_floor (world meters).
	real_t major_step = 0.0; // minor_step * steps.

	// Camera-centered major-cell index along U/V. The centered U/V coordinate is
	// simply `major_step * center_cell_u` (resp. `_v`); it is not stored to keep
	// the result free of derivable fields.
	int center_cell_u = 0;
	int center_cell_v = 0;

	real_t fade_distance = 0.0; // Physical fade radius for the distance-fade shader uniform.
};

// The set of values whose change forces a geometry rebuild (plan section 5.4).
// Comparable so a renderer rebuilds exactly when the key changes and never for
// a fade-only (material) change: `fade` is deliberately excluded.
struct EditorGridRebuildKey {
	// Frame basis, anchor and plane coordinate.
	Vector3 u;
	Vector3 v;
	Vector3 n;
	Vector3 anchor;
	real_t plane_coordinate = 0.0;

	// Floor LOD and the camera-centered major-cell coordinate in U/V.
	int level_floor = 0;
	int center_cell_u = 0;
	int center_cell_v = 0;

	// Grid extent, plane settings, colors and primary step count.
	int grid_half_extent_cells = 0;
	uint32_t plane_mask = 0; // Which canonical planes are drawn (world mode).
	Color primary_color;
	Color secondary_color;
	int primary_grid_steps = 0;

	// Base translate snap step.
	real_t base_translate_snap = 0.0;

	// Frame comparisons use a tolerance scaled by the current minor step. Raw
	// ray-hit jitter and one-pixel viewport changes must not rebuild geometry.
	real_t frame_tolerance = 0.0;

	bool operator==(const EditorGridRebuildKey &p_other) const;
	bool operator!=(const EditorGridRebuildKey &p_other) const { return !(*this == p_other); }
};

// Normalize a loaded base snap step: zero/negative/non-finite -> the supported
// minimum. Snap-off is expressed by the enable toggle, never by a zero step.
real_t editor_grid_normalize_translate_snap(real_t p_step);

// Projected on-screen width, in pixels, of one base snap cell at a stable
// sample on the frame's plane. Both orthographic and perspective cameras use
// their full view/projection transforms and project the sample plus U/V steps,
// so arbitrary-plane foreshortening affects density. Invalid or grazing
// ("horizon") cases clamp to a small positive value so the LOD reaches its
// coarsest clamp instead of exploding. Never returns a non-finite value.
real_t editor_grid_projected_pixels_per_base_step(
		const EditorGridCameraSample &p_camera,
		const EditorGridFrame3D &p_frame,
		real_t p_base_step);

// The full LOD/layout policy. Pure: same inputs always yield the same result,
// and every field is finite regardless of input.
EditorGridLodResult editor_grid_compute_lod(
		const EditorGridLodSettings &p_settings,
		const EditorGridCameraSample &p_camera,
		const EditorGridFrame3D &p_frame);

// Assemble the semantic rebuild key from the layout inputs and the computed
// LOD. `p_plane_mask` and the two colors come from the renderer's current
// settings; everything else is derived here.
EditorGridRebuildKey editor_grid_make_rebuild_key(
		const EditorGridLodSettings &p_settings,
		const EditorGridCameraSample &p_camera,
		const EditorGridFrame3D &p_frame,
		const EditorGridLodResult &p_lod,
		uint32_t p_plane_mask,
		const Color &p_primary_color,
		const Color &p_secondary_color);
