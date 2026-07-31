/**************************************************************************/
/*  editor_grid_3d.cpp                                                    */
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

#include "editor_grid_3d.h"

#include "core/error/error_macros.h"
#include "core/math/basis.h"
#include "core/math/math_funcs.h"

#include <climits>

namespace {

// Natural log of 10, for the change-of-base rescale that the legacy grid uses
// so that the division-level min/max settings keep their "in decades" meaning
// regardless of primary_grid_steps.
constexpr double GRID_LN_10 = 2.302585092994045901094;

// A direction/basis shorter than this is treated as degenerate.
constexpr real_t GRID_DIR_EPSILON = (real_t)1e-6;

// Floor for projected density so the logarithms below stay finite and the LOD
// simply clamps to its coarsest level at the horizon instead of exploding.
constexpr real_t GRID_MIN_PIXELS = (real_t)1e-4;

// Editor settings only expose a few decades in either direction. This wider
// guard still covers tiny/large snap profiles while keeping pow() finite in
// both float and double precision builds, including the maximum step count.
constexpr int GRID_MAX_ABS_LEVEL = 16;

static real_t grid_safe_pow(real_t p_steps, real_t p_level, real_t p_fallback = 1.0) {
	const real_t value = (real_t)Math::pow((double)p_steps, (double)CLAMP(p_level, (real_t)-GRID_MAX_ABS_LEVEL, (real_t)GRID_MAX_ABS_LEVEL));
	return Math::is_finite(value) && value > 0.0 ? value : p_fallback;
}

static int grid_safe_cell(real_t p_coordinate, real_t p_cell_size) {
	if (!Math::is_finite(p_coordinate) || !Math::is_finite(p_cell_size) || p_cell_size <= 0.0) {
		return 0;
	}
	const double cell = (double)p_coordinate / (double)p_cell_size;
	if (!Math::is_finite(cell)) {
		return 0;
	}
	return (int)CLAMP(cell, (double)INT_MIN, (double)INT_MAX);
}

static bool grid_vector_equal_tolerance(const Vector3 &p_a, const Vector3 &p_b, real_t p_tolerance) {
	return Math::abs(p_a.x - p_b.x) <= p_tolerance &&
			Math::abs(p_a.y - p_b.y) <= p_tolerance &&
			Math::abs(p_a.z - p_b.z) <= p_tolerance;
}

} // namespace

int Editor3DViewportLayerPool::get_layer_for_slot(int p_slot) {
	static constexpr int PRIVATE_LAYERS[LAYER_COUNT] = { 20, 21, 22, 23, 27, 28, 29, 30, 31 };
	ERR_FAIL_INDEX_V(p_slot, LAYER_COUNT, -1);
	return PRIVATE_LAYERS[p_slot];
}

Editor3DViewportLayerLease Editor3DViewportLayerPool::acquire(uint64_t p_scenario_key) {
	Editor3DViewportLayerLease lease;
	if (p_scenario_key == 0) {
		return lease;
	}
	uint32_t &used = used_masks[p_scenario_key];
	for (int slot = 0; slot < LAYER_COUNT; slot++) {
		const uint32_t slot_mask = uint32_t(1) << slot;
		if (!(used & slot_mask)) {
			used |= slot_mask;
			lease.scenario_key = p_scenario_key;
			lease.layer = get_layer_for_slot(slot);
			return lease;
		}
	}
	return lease;
}

void Editor3DViewportLayerPool::release(const Editor3DViewportLayerLease &p_lease) {
	if (!p_lease.is_valid()) {
		return;
	}
	int slot = -1;
	for (int i = 0; i < LAYER_COUNT; i++) {
		if (get_layer_for_slot(i) == p_lease.layer) {
			slot = i;
			break;
		}
	}
	if (slot < 0) {
		return;
	}
	HashMap<uint64_t, uint32_t>::Iterator it = used_masks.find(p_lease.scenario_key);
	if (!it) {
		return;
	}
	it->value &= ~(uint32_t(1) << slot);
	if (it->value == 0) {
		used_masks.remove(it);
	}
}

int Editor3DViewportLayerPool::get_active_count(uint64_t p_scenario_key) const {
	HashMap<uint64_t, uint32_t>::ConstIterator it = used_masks.find(p_scenario_key);
	if (!it) {
		return 0;
	}
	int count = 0;
	for (int slot = 0; slot < LAYER_COUNT; slot++) {
		if (it->value & (uint32_t(1) << slot)) {
			count++;
		}
	}
	return count;
}

EditorGridFrame3D EditorGridFrame3D::world_xz() {
	EditorGridFrame3D frame;
	frame.u = Vector3(1, 0, 0); // +X
	frame.n = Vector3(0, 1, 0); // +Y (up)
	frame.v = frame.n.cross(frame.u); // N x U = -Z, keeping the frame right-handed.
	frame.anchor = Vector3();
	frame.plane_coordinate = 0.0;
	return frame;
}

bool EditorGridFrame3D::from_plane_in_space(
		const Vector3 &p_point_on_plane,
		const Vector3 &p_outward_normal,
		const Transform3D &p_space_to_world,
		const Vector3 &p_tangent_hint,
		EditorGridFrame3D &r_frame) {
	// The anchor is the space's coordinate origin.
	const Vector3 anchor = p_space_to_world.origin;
	if (!p_space_to_world.is_finite() || Math::abs(p_space_to_world.basis.determinant()) < GRID_DIR_EPSILON ||
			!anchor.is_finite() || !p_point_on_plane.is_finite() || !p_outward_normal.is_finite()) {
		return false;
	}

	// Normalize the semantic outward normal. Scale is never a grid unit.
	const real_t normal_len = p_outward_normal.length();
	if (!Math::is_finite(normal_len) || normal_len < GRID_DIR_EPSILON) {
		return false;
	}
	const Vector3 n = p_outward_normal / normal_len;

	// Choose the in-plane tangent U. Prefer the caller's hint, projected onto
	// the plane (Gram-Schmidt against N). If the hint is degenerate (missing or
	// nearly parallel to N), fall back to the space-basis axis least parallel to
	// N, and finally to a world axis if the space basis is unusable.
	Vector3 u;
	bool have_u = false;

	if (p_tangent_hint.is_finite()) {
		const Vector3 hint_in_plane = p_tangent_hint.slide(n);
		const real_t hint_len = hint_in_plane.length();
		if (Math::is_finite(hint_len) && hint_len > GRID_DIR_EPSILON) {
			u = hint_in_plane / hint_len;
			have_u = true;
		}
	}

	if (!have_u) {
		// Gather candidate axes: the orthonormalized space basis if it is finite,
		// otherwise the world axes. Orthonormalizing removes scale/shear; a mirror
		// is preserved, which only flips a candidate's sign -- deterministic given
		// the input and harmless because V = N x U restores right-handedness.
		Vector3 axes[3];
		const Basis space_basis = p_space_to_world.basis.orthonormalized();
		const Vector3 c0 = space_basis.get_column(0);
		const Vector3 c1 = space_basis.get_column(1);
		const Vector3 c2 = space_basis.get_column(2);
		if (c0.is_finite() && c1.is_finite() && c2.is_finite() &&
				c0.length() > GRID_DIR_EPSILON && c1.length() > GRID_DIR_EPSILON && c2.length() > GRID_DIR_EPSILON) {
			axes[0] = c0;
			axes[1] = c1;
			axes[2] = c2;
		} else {
			axes[0] = Vector3(1, 0, 0);
			axes[1] = Vector3(0, 1, 0);
			axes[2] = Vector3(0, 0, 1);
		}

		// Pick the axis least parallel to N (smallest |dot|; the axes are unit
		// length by construction). Deterministic tie break: the earliest axis
		// index wins.
		int best = 0;
		real_t best_parallel = Math::abs(n.dot(axes[0]));
		for (int i = 1; i < 3; i++) {
			const real_t parallel = Math::abs(n.dot(axes[i]));
			if (parallel < best_parallel) {
				best_parallel = parallel;
				best = i;
			}
		}

		const Vector3 fallback_in_plane = axes[best].slide(n);
		const real_t fallback_len = fallback_in_plane.length();
		if (!Math::is_finite(fallback_len) || fallback_len < GRID_DIR_EPSILON) {
			return false; // Should not happen for a non-degenerate N, but never manufacture NaN.
		}
		u = fallback_in_plane / fallback_len;
	}

	Vector3 v = n.cross(u);
	const real_t v_len = v.length();
	if (!Math::is_finite(v_len) || v_len < GRID_DIR_EPSILON) {
		return false;
	}
	v /= v_len;

	r_frame.u = u;
	r_frame.v = v;
	r_frame.n = n;
	r_frame.anchor = anchor;
	// Absolute N coordinate of the plane, measured from the anchor.
	r_frame.plane_coordinate = n.dot(p_point_on_plane - anchor);

	if (!Math::is_finite(r_frame.plane_coordinate)) {
		return false;
	}
	return true;
}

Vector3 EditorGridFrame3D::to_coordinates(const Vector3 &p_world) const {
	const Vector3 rel = p_world - anchor;
	return Vector3(u.dot(rel), v.dot(rel), n.dot(rel));
}

Vector3 EditorGridFrame3D::to_world(const Vector3 &p_coordinates) const {
	return anchor + u * p_coordinates.x + v * p_coordinates.y + n * p_coordinates.z;
}

Vector3 EditorGridFrame3D::plane_origin() const {
	// Foot of the perpendicular from the anchor onto the displayed plane.
	return anchor + n * plane_coordinate;
}

Plane EditorGridFrame3D::plane() const {
	return Plane(n, plane_origin());
}

// Both snap functions: `p_step <= 0` passes the coordinate through, matching
// Math::snapped(x, 0) -- whether to snap at all is the caller's decision (plan
// invariant 10); see the header for the full convention.
Vector2 EditorGridFrame3D::snap_uv(const Vector3 &p_world, real_t p_step) const {
	const Vector3 coords = to_coordinates(p_world);
	if (!Math::is_finite(p_step) || p_step <= 0) {
		return Vector2(coords.x, coords.y);
	}
	return Vector2(Math::snapped(coords.x, p_step), Math::snapped(coords.y, p_step));
}

real_t EditorGridFrame3D::snap_n(const Vector3 &p_world, real_t p_step) const {
	const real_t coord = n.dot(p_world - anchor); // == to_coordinates(p_world).z, without the unused U/V dots.
	if (!Math::is_finite(p_step) || p_step <= 0) {
		return coord;
	}
	return Math::snapped(coord, p_step);
}

bool EditorGridFrame3D::is_valid() const {
	if (!u.is_finite() || !v.is_finite() || !n.is_finite() || !anchor.is_finite() || !Math::is_finite(plane_coordinate)) {
		return false;
	}
	// Unit length and mutually orthogonal (as columns of a basis), then
	// right-handed: u x v == n.
	return Basis(u, v, n).is_orthonormal() && u.cross(v).is_equal_approx(n);
}

real_t editor_grid_normalize_translate_snap(real_t p_step) {
	if (!Math::is_finite(p_step) || p_step <= 0) {
		return EDITOR_GRID_MIN_TRANSLATE_SNAP;
	}
	return MAX(p_step, EDITOR_GRID_MIN_TRANSLATE_SNAP);
}

EditorGridLodSettings EditorGridLodSettings::normalized() const {
	EditorGridLodSettings s = *this;
	s.base_translate_snap = editor_grid_normalize_translate_snap(base_translate_snap);
	// primary_grid_steps <= 1 would make log(steps) zero/negative. Use a guarded
	// fallback factor of 2 so no logarithm divides by zero (plan section 5.4).
	s.primary_grid_steps = MAX(2, primary_grid_steps);
	s.division_level_bias = Math::is_finite(division_level_bias) ? division_level_bias : 0.0;
	s.grid_half_extent_cells = MAX(0, grid_half_extent_cells);
	s.reference_pixels = Math::is_finite(reference_pixels) ? MAX(reference_pixels, (real_t)1e-3) : EDITOR_GRID_DEFAULT_REFERENCE_PIXELS;
	return s;
}

// Shared body of `editor_grid_projected_pixels_per_base_step`; assumes the
// frame was already validated and the base step normalized, so the hot LOD
// path validates each exactly once.
static real_t grid_projected_pixels(
		const EditorGridCameraSample &p_camera,
		const EditorGridFrame3D &p_frame,
		real_t p_base_step) {
	if (!Math::is_finite(p_camera.viewport_width) || !Math::is_finite(p_camera.viewport_height) ||
			p_camera.viewport_width <= 0 || p_camera.viewport_height <= 0 || !p_camera.camera_transform.is_finite()) {
		return GRID_MIN_PIXELS;
	}

	// Project a stable sample on the plane, plus a base step along each of U and
	// V, and measure the on-screen separation. The same full projection path is
	// used for orthographic and perspective cameras so tilted work planes retain
	// their foreshortening.
	const Transform3D view = p_camera.camera_transform.affine_inverse();
	const Vector3 cam = p_camera.camera_transform.origin;
	Vector3 sample;
	const Vector3 view_direction = -p_camera.camera_transform.basis.get_column(2).normalized();
	bool have_sample = view_direction.is_finite() && p_frame.plane().intersects_ray(cam, view_direction, &sample) && sample.is_finite();
	if (!have_sample && p_camera.has_focus_point && p_camera.focus_point.is_finite()) {
		sample = p_frame.plane().project(p_camera.focus_point);
		have_sample = sample.is_finite();
	}
	if (!have_sample) {
		sample = p_frame.plane().project(cam);
		if (!sample.is_finite()) {
			return GRID_MIN_PIXELS;
		}
	}

	// Project a world point to pixel coordinates; returns false if it is at or
	// behind the camera or otherwise not finite.
	auto project = [&](const Vector3 &p_world, Vector2 &r_px) -> bool {
		const Vector3 view_pos = view.xform(p_world);
		if (!view_pos.is_finite() || !(view_pos.z < 0)) { // Camera looks down local -Z.
			return false;
		}
		const Vector3 ndc = p_camera.projection.xform(view_pos); // Includes the perspective divide.
		if (!ndc.is_finite()) {
			return false;
		}
		r_px = Vector2(
				(ndc.x * (real_t)0.5 + (real_t)0.5) * p_camera.viewport_width,
				((real_t)0.5 - ndc.y * (real_t)0.5) * p_camera.viewport_height);
		return r_px.is_finite();
	};

	Vector2 px_center;
	Vector2 px_u;
	Vector2 px_v;
	if (!project(sample, px_center) ||
			!project(sample + p_frame.u * p_base_step, px_u) ||
			!project(sample + p_frame.v * p_base_step, px_v)) {
		return GRID_MIN_PIXELS; // Horizon / behind-camera: clamp to coarsest.
	}

	const real_t pu = px_center.distance_to(px_u);
	const real_t pv = px_center.distance_to(px_v);
	if (!Math::is_finite(pu) || !Math::is_finite(pv)) {
		return GRID_MIN_PIXELS;
	}

	// Isotropic base-step density: geometric mean of the two in-plane rates.
	// Edge-on (one rate near zero) collapses toward the floor, so the grid
	// coarsens rather than exploding.
	const real_t pixels = Math::sqrt(MAX(pu, (real_t)0.0) * MAX(pv, (real_t)0.0));
	if (!Math::is_finite(pixels) || pixels <= 0) {
		return GRID_MIN_PIXELS;
	}
	return pixels;
}

real_t editor_grid_projected_pixels_per_base_step(
		const EditorGridCameraSample &p_camera,
		const EditorGridFrame3D &p_frame,
		real_t p_base_step) {
	if (!p_frame.is_valid()) {
		return GRID_MIN_PIXELS;
	}
	return grid_projected_pixels(p_camera, p_frame, editor_grid_normalize_translate_snap(p_base_step));
}

EditorGridLodResult editor_grid_compute_lod(
		const EditorGridLodSettings &p_settings,
		const EditorGridCameraSample &p_camera,
		const EditorGridFrame3D &p_frame) {
	EditorGridLodResult r;

	const EditorGridLodSettings s = p_settings.normalized();
	const real_t base = s.base_translate_snap;
	const real_t steps = (real_t)s.primary_grid_steps;
	const double log_steps = Math::log((double)s.primary_grid_steps);

	// First recover the absolute physical spacing envelope represented by the
	// legacy division settings. Their change-of-base/truncation behavior remains
	// compatible; only then is that envelope mapped outward onto the snap lattice.
	int legacy_min = s.division_level_min;
	int legacy_max = s.division_level_max;
	if (s.primary_grid_steps != 10) {
		const real_t div = (real_t)(log_steps / GRID_LN_10);
		if (Math::is_finite(div) && Math::abs(div) > GRID_DIR_EPSILON) {
			legacy_min = (int)(s.division_level_min / div);
			legacy_max = (int)(s.division_level_max / div);
		}
	}
	if (legacy_max < legacy_min) {
		SWAP(legacy_min, legacy_max);
	}
	legacy_min = CLAMP(legacy_min, -GRID_MAX_ABS_LEVEL, GRID_MAX_ABS_LEVEL);
	legacy_max = CLAMP(legacy_max, -GRID_MAX_ABS_LEVEL, GRID_MAX_ABS_LEVEL);

	const real_t configured_min_spacing = grid_safe_pow(steps, (real_t)legacy_min);
	const real_t configured_max_spacing = grid_safe_pow(steps, (real_t)legacy_max);
	const real_t relative_min = configured_min_spacing / base;
	const real_t relative_max = configured_max_spacing / base;
	const real_t lattice_min = Math::is_finite(relative_min) && relative_min > 0.0 ? (real_t)(Math::log((double)relative_min) / log_steps) : 0.0;
	const real_t lattice_max = Math::is_finite(relative_max) && relative_max > 0.0 ? (real_t)(Math::log((double)relative_max) / log_steps) : 0.0;
	r.level_min = CLAMP(MIN(0, (int)Math::floor(lattice_min)), -GRID_MAX_ABS_LEVEL, GRID_MAX_ABS_LEVEL);
	r.level_max = CLAMP(MAX(0, (int)Math::ceil(lattice_max)), -GRID_MAX_ABS_LEVEL, GRID_MAX_ABS_LEVEL);
	if (r.level_max < r.level_min) {
		SWAP(r.level_min, r.level_max);
	}

	// The frame is validated exactly once per evaluation; an invalid frame keeps
	// every output finite by degrading to the density floor.
	r.valid = p_frame.is_valid();

	// Continuous level from projected on-screen density. `reference_pixels` is
	// the base-cell pixel width at which the level (before bias) crosses zero.
	const real_t pixels = r.valid ? grid_projected_pixels(p_camera, p_frame, base) : GRID_MIN_PIXELS;
	r.projected_pixels_per_base_step = pixels;

	real_t level = (real_t)(Math::log((double)(s.reference_pixels / pixels)) / log_steps) + s.division_level_bias;
	if (!Math::is_finite(level)) {
		level = 0.0;
	}
	r.level_raw = level;

	const real_t clamped = CLAMP(level, (real_t)r.level_min, (real_t)r.level_max);
	r.level_clamped = clamped;
	const real_t floored = Math::floor(clamped);
	r.level_floor = (int)floored;
	r.fade = clamped - floored; // Drives the small/large color fade only.

	r.minor_step = base * grid_safe_pow(steps, floored);
	if (!Math::is_finite(r.minor_step) || r.minor_step <= 0.0) {
		r.minor_step = base;
	}
	r.major_step = r.minor_step * steps;
	if (!Math::is_finite(r.major_step) || r.major_step <= 0.0) {
		r.major_step = r.minor_step;
	}

	// Camera-centered major cell. Center on the caller-provided focus point when
	// present (e.g. the orthographic view-ray/plane intersection supplied by the
	// renderer); otherwise on the camera origin's in-plane U/V, matching the
	// legacy perspective centering. Keeping refined centering an *input* keeps
	// the rebuild key authoritative over the geometry the renderer draws.
	const Vector3 center_source = (p_camera.has_focus_point && p_camera.focus_point.is_finite())
			? p_camera.focus_point
			: p_camera.camera_transform.origin;
	const Vector3 center_coords = p_frame.to_coordinates(center_source);
	const real_t large = r.major_step;
	if (Math::is_finite(large) && large > 0) {
		r.center_cell_u = grid_safe_cell(center_coords.x, large); // Truncation toward zero, as legacy.
		r.center_cell_v = grid_safe_cell(center_coords.y, large);
	}

	// Physical fade radius for the distance-fade shader, re-based on the snap
	// lattice so it tracks the grid's real world extent (uses the unclamped
	// level, as the legacy code does).
	real_t fade_size = base * grid_safe_pow(steps, level - (real_t)1.0);
	const real_t min_fade = base * grid_safe_pow(steps, (real_t)r.level_min);
	const real_t max_fade = base * grid_safe_pow(steps, (real_t)r.level_max);
	fade_size = CLAMP(fade_size, min_fade, max_fade);
	r.fade_distance = MAX((real_t)0.0, ((real_t)s.grid_half_extent_cells - steps) * fade_size);
	if (!Math::is_finite(r.fade_distance)) {
		r.fade_distance = 0.0;
	}

	return r;
}

bool EditorGridRebuildKey::operator==(const EditorGridRebuildKey &p_other) const {
	const real_t tolerance = MAX(frame_tolerance, p_other.frame_tolerance);
	return grid_vector_equal_tolerance(u, p_other.u, tolerance) &&
			grid_vector_equal_tolerance(v, p_other.v, tolerance) &&
			grid_vector_equal_tolerance(n, p_other.n, tolerance) &&
			grid_vector_equal_tolerance(anchor, p_other.anchor, tolerance) &&
			Math::abs(plane_coordinate - p_other.plane_coordinate) <= tolerance &&
			level_floor == p_other.level_floor &&
			center_cell_u == p_other.center_cell_u &&
			center_cell_v == p_other.center_cell_v &&
			grid_half_extent_cells == p_other.grid_half_extent_cells &&
			plane_mask == p_other.plane_mask &&
			primary_color == p_other.primary_color &&
			secondary_color == p_other.secondary_color &&
			primary_grid_steps == p_other.primary_grid_steps &&
			base_translate_snap == p_other.base_translate_snap;
}

EditorGridRebuildKey editor_grid_make_rebuild_key(
		const EditorGridLodSettings &p_settings,
		const EditorGridCameraSample &p_camera,
		const EditorGridFrame3D &p_frame,
		const EditorGridLodResult &p_lod,
		uint32_t p_plane_mask,
		const Color &p_primary_color,
		const Color &p_secondary_color) {
	// Normalized so a degenerate raw setting can never change the key without
	// changing the geometry (the LOD policy normalizes through the same place).
	(void)p_camera; // Projection telemetry affects derived LOD/cell values, not the raw geometry key.
	const EditorGridLodSettings s = p_settings.normalized();

	EditorGridRebuildKey key;
	key.u = p_frame.u;
	key.v = p_frame.v;
	key.n = p_frame.n;
	key.anchor = p_frame.anchor;
	key.plane_coordinate = p_frame.plane_coordinate;

	key.level_floor = p_lod.level_floor;
	key.center_cell_u = p_lod.center_cell_u;
	key.center_cell_v = p_lod.center_cell_v;

	key.grid_half_extent_cells = s.grid_half_extent_cells;
	key.plane_mask = p_plane_mask;
	key.primary_color = p_primary_color;
	key.secondary_color = p_secondary_color;
	key.primary_grid_steps = s.primary_grid_steps;

	key.base_translate_snap = s.base_translate_snap;
	key.frame_tolerance = MAX((real_t)1e-6, p_lod.minor_step * (real_t)1e-5);
	return key;
}
