/**************************************************************************/
/*  test_editor_grid_3d.cpp                                              */
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

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_editor_grid_3d)

#ifdef TOOLS_ENABLED

#include "core/math/math_funcs.h"
#include "editor/scene/3d/editor_grid_3d.h"

namespace TestEditorGrid3D {

// ------------------------------------------------------------------------- //
// Helpers.
// ------------------------------------------------------------------------- //

static bool lod_is_finite(const EditorGridLodResult &p_lod) {
	return Math::is_finite(p_lod.projected_pixels_per_base_step) &&
			Math::is_finite(p_lod.level_raw) &&
			Math::is_finite(p_lod.level_clamped) &&
			Math::is_finite(p_lod.fade) &&
			Math::is_finite(p_lod.minor_step) &&
			Math::is_finite(p_lod.major_step) &&
			Math::is_finite(p_lod.fade_distance);
}

// An orthographic camera whose projected density is exact and controllable for
// the horizontal frame: pixels/base-step == step * viewport / ortho_size.
static EditorGridCameraSample make_ortho_camera(real_t p_ortho_size, real_t p_viewport = 1000.0, const Vector3 &p_origin = Vector3(0, 10, 0)) {
	EditorGridCameraSample cam;
	cam.orthogonal = true;
	cam.ortho_size = p_ortho_size;
	cam.viewport_width = p_viewport;
	cam.viewport_height = p_viewport;
	cam.projection = Projection::create_orthogonal(-p_ortho_size * 0.5, p_ortho_size * 0.5, -p_ortho_size * 0.5, p_ortho_size * 0.5, 0.05, 4000.0);
	cam.camera_transform.set_look_at(p_origin, p_origin + Vector3(0, -1, 0), Vector3(0, 0, -1));
	return cam;
}

// A perspective camera looking straight down onto the world XZ plane.
static EditorGridCameraSample make_perspective_camera_above(real_t p_height, real_t p_viewport = 1000.0) {
	EditorGridCameraSample cam;
	cam.orthogonal = false;
	cam.viewport_width = p_viewport;
	cam.viewport_height = p_viewport;
	cam.projection = Projection::create_perspective(70.0, 1.0, 0.05, 4000.0);
	// Looking straight down; up is -Z so the basis is well-defined.
	cam.camera_transform.set_look_at(Vector3(0, p_height, 0), Vector3(0, 0, 0), Vector3(0, 0, -1));
	return cam;
}

static EditorGridLodSettings make_settings() {
	EditorGridLodSettings s;
	s.base_translate_snap = 1.0;
	s.primary_grid_steps = 8;
	s.division_level_bias = 0.0; // No bias for exact boundary math in tests.
	s.division_level_min = -10; // Wide clamp so tests exercise the free level.
	s.division_level_max = 10;
	s.grid_half_extent_cells = 200;
	s.reference_pixels = 64.0;
	return s;
}

// ------------------------------------------------------------------------- //
// Frame construction and invariants (plan sections 5.3, 11).
// ------------------------------------------------------------------------- //

TEST_CASE("[EditorGrid3D] World XZ frame is canonical, orthonormal and right-handed") {
	const EditorGridFrame3D frame = EditorGridFrame3D::world_xz();
	CHECK(frame.is_valid());
	CHECK(frame.u.is_equal_approx(Vector3(1, 0, 0)));
	CHECK(frame.n.is_equal_approx(Vector3(0, 1, 0)));
	CHECK(frame.v.is_equal_approx(Vector3(0, 0, -1)));
	CHECK(frame.anchor.is_equal_approx(Vector3()));
	CHECK(frame.plane_coordinate == doctest::Approx(0.0));

	// The plane is the world ground plane.
	const Plane p = frame.plane();
	CHECK(p.normal.is_equal_approx(Vector3(0, 1, 0)));
	CHECK(p.d == doctest::Approx(0.0));
}

TEST_CASE("[EditorGrid3D] Rotated, translated and arbitrary-plane frames stay orthonormal") {
	// Arbitrary tilted plane through an arbitrary point, oriented by a rotated
	// and translated space.
	const Vector3 normal = Vector3(0.3, 0.7, -0.5).normalized();
	const Vector3 point = Vector3(4, -2, 9);
	Transform3D space;
	space.basis = Basis::from_euler(Vector3(0.5, -1.1, 0.3));
	space.origin = Vector3(-3, 5, 2);
	const Vector3 hint = Vector3(1, 0, 0);

	EditorGridFrame3D frame;
	REQUIRE(EditorGridFrame3D::from_plane_in_space(point, normal, space, hint, frame));
	CHECK(frame.is_valid());

	// Anchor is the space origin; U lies in the plane.
	CHECK(frame.anchor.is_equal_approx(space.origin));
	CHECK(Math::is_zero_approx(frame.u.dot(frame.n)));
	CHECK(Math::is_zero_approx(frame.v.dot(frame.n)));
}

TEST_CASE("[EditorGrid3D] Anchor and plane coordinate reconstruct the exact source plane") {
	const Vector3 normal = Vector3(-0.2, 0.4, 0.9).normalized();
	const Vector3 point = Vector3(7, 3, -1);
	Transform3D space;
	space.basis = Basis::from_euler(Vector3(-0.9, 0.2, 1.3));
	space.origin = Vector3(11, -4, 6);

	EditorGridFrame3D frame;
	REQUIRE(EditorGridFrame3D::from_plane_in_space(point, normal, space, Vector3(0, 0, 1), frame));

	const Plane source(normal, point);
	const Plane rebuilt = frame.plane();
	CHECK(rebuilt.normal.is_equal_approx(source.normal));
	CHECK(rebuilt.d == doctest::Approx(source.d));

	// plane_origin lies on the source plane; the source point does too.
	CHECK(Math::is_zero_approx(source.distance_to(frame.plane_origin())));
	CHECK(Math::is_zero_approx(rebuilt.distance_to(point)));
}

TEST_CASE("[EditorGrid3D] Coordinate transforms are exact inverses") {
	const Vector3 normal = Vector3(0.1, 0.9, 0.2).normalized();
	Transform3D space;
	space.basis = Basis::from_euler(Vector3(0.2, 0.4, -0.6));
	space.origin = Vector3(2, 2, 2);
	EditorGridFrame3D frame;
	REQUIRE(EditorGridFrame3D::from_plane_in_space(Vector3(1, 1, 1), normal, space, Vector3(1, 0, 0), frame));

	const Vector3 samples[] = { Vector3(0, 0, 0), Vector3(5, -3, 8), Vector3(-12, 4, -7) };
	for (const Vector3 &w : samples) {
		const Vector3 coords = frame.to_coordinates(w);
		CHECK(frame.to_world(coords).is_equal_approx(w));
	}
}

TEST_CASE("[EditorGrid3D] Nonuniform and mirrored transforms do not skew or scale cells") {
	const Vector3 normal = Vector3(0, 1, 0);
	const Vector3 point = Vector3(0, 3, 0);

	// Nonuniform scale in the space must not leak into the lattice.
	Transform3D nonuniform;
	nonuniform.basis.set_column(0, Vector3(4, 0, 0));
	nonuniform.basis.set_column(1, Vector3(0, 0.25, 0));
	nonuniform.basis.set_column(2, Vector3(0, 0, 9));
	nonuniform.origin = Vector3(1, 0, 1);
	EditorGridFrame3D f_nonuniform;
	REQUIRE(EditorGridFrame3D::from_plane_in_space(point, normal, nonuniform, Vector3(), f_nonuniform));
	CHECK(f_nonuniform.is_valid());
	CHECK(f_nonuniform.u.length() == doctest::Approx(1.0));
	CHECK(f_nonuniform.v.length() == doctest::Approx(1.0));

	// A mirrored (negative determinant) space must still yield a right-handed,
	// unit lattice -- never left-handed or scaled.
	Transform3D mirrored;
	mirrored.basis.set_column(0, Vector3(-1, 0, 0)); // Mirror X.
	mirrored.basis.set_column(1, Vector3(0, 1, 0));
	mirrored.basis.set_column(2, Vector3(0, 0, 1));
	EditorGridFrame3D f_mirrored;
	REQUIRE(EditorGridFrame3D::from_plane_in_space(point, normal, mirrored, Vector3(1, 0, 0), f_mirrored));
	CHECK(f_mirrored.is_valid()); // is_valid() includes the right-handedness check.

	// Cell squareness: stepping one unit along U and along V covers equal world
	// distance (== base step), so cells are not skewed.
	const real_t du = f_nonuniform.to_world(Vector3(1, 0, 0)).distance_to(f_nonuniform.to_world(Vector3(0, 0, 0)));
	const real_t dv = f_nonuniform.to_world(Vector3(0, 1, 0)).distance_to(f_nonuniform.to_world(Vector3(0, 0, 0)));
	CHECK(du == doctest::Approx(dv));
	CHECK(du == doctest::Approx(1.0));
}

TEST_CASE("[EditorGrid3D] Degenerate construction returns false and never emits NaN") {
	EditorGridFrame3D frame;

	// Zero-length normal.
	CHECK_FALSE(EditorGridFrame3D::from_plane_in_space(Vector3(0, 0, 0), Vector3(0, 0, 0), Transform3D(), Vector3(1, 0, 0), frame));
	// Non-finite normal.
	const real_t nan = Math::NaN;
	CHECK_FALSE(EditorGridFrame3D::from_plane_in_space(Vector3(0, 0, 0), Vector3(nan, 0, 0), Transform3D(), Vector3(1, 0, 0), frame));
	// Non-finite point.
	CHECK_FALSE(EditorGridFrame3D::from_plane_in_space(Vector3(nan, 0, 0), Vector3(0, 1, 0), Transform3D(), Vector3(1, 0, 0), frame));
	// Singular source space cannot define an orientation.
	Transform3D singular;
	singular.basis = Basis(Vector3(), Vector3(), Vector3());
	CHECK_FALSE(EditorGridFrame3D::from_plane_in_space(Vector3(), Vector3(0, 1, 0), singular, Vector3(1, 0, 0), frame));
}

TEST_CASE("[EditorGrid3D] Deterministic tangent fallback picks the least-parallel axis") {
	// A hint parallel to N is degenerate; U must fall back deterministically.
	const Vector3 normal = Vector3(0, 1, 0);
	EditorGridFrame3D a;
	EditorGridFrame3D b;
	REQUIRE(EditorGridFrame3D::from_plane_in_space(Vector3(0, 0, 0), normal, Transform3D(), Vector3(0, 1, 0), a));
	REQUIRE(EditorGridFrame3D::from_plane_in_space(Vector3(0, 0, 0), normal, Transform3D(), Vector3(), b));

	// Identical inputs -> identical frames (determinism).
	CHECK(a.u.is_equal_approx(b.u));
	CHECK(a.v.is_equal_approx(b.v));
	CHECK(a.is_valid());

	// The fallback axis must lie in the plane (least parallel to N == +Y is +X,
	// the first world axis by deterministic tie-break).
	CHECK(Math::is_zero_approx(a.u.dot(normal)));
	CHECK(a.u.is_equal_approx(Vector3(1, 0, 0)));
}

// ------------------------------------------------------------------------- //
// Absolute snapping (plan sections 5.3, 11, invariant 12).
// ------------------------------------------------------------------------- //

TEST_CASE("[EditorGrid3D] Absolute U/V/N snapping is stable across repeated samples") {
	const EditorGridFrame3D frame = EditorGridFrame3D::world_xz();
	const real_t step = 0.25;

	// A cloud of jittered points around one lattice node all snap to that node,
	// and repeated sampling of the same point is identical (no accumulation).
	const Vector3 node_world = frame.to_world(Vector3(2.0, -3.0, 0.0)); // A lattice node (U=2 m, V=-3 m, both multiples of the step).
	const Vector2 expected = frame.snap_uv(node_world, step);
	for (int i = -3; i <= 3; i++) {
		for (int j = -3; j <= 3; j++) {
			const Vector3 jitter = node_world + frame.u * (0.1 * i * step * 0.4) + frame.v * (0.1 * j * step * 0.4);
			const Vector2 s1 = frame.snap_uv(jitter, step);
			const Vector2 s2 = frame.snap_uv(jitter, step);
			CHECK(s1.is_equal_approx(s2)); // Stable / deterministic.
			CHECK(s1.is_equal_approx(expected)); // Snaps to the same node.
		}
	}

	// N snapping is absolute about the anchor.
	const Vector3 above = frame.to_world(Vector3(0, 0, 1.24));
	CHECK(frame.snap_n(above, 0.5) == doctest::Approx(1.0));
	const Vector3 above2 = frame.to_world(Vector3(0, 0, 1.26));
	CHECK(frame.snap_n(above2, 0.5) == doctest::Approx(1.5));
}

TEST_CASE("[EditorGrid3D] Snapping is absolute about a translated anchor, not the origin") {
	// Anchor away from the world origin; snapping is measured from the anchor.
	Transform3D space;
	space.origin = Vector3(0.13, 0, 0.07); // Non-lattice-aligned anchor.
	EditorGridFrame3D frame;
	REQUIRE(EditorGridFrame3D::from_plane_in_space(space.origin, Vector3(0, 1, 0), space, Vector3(1, 0, 0), frame));

	const real_t step = 1.0;
	// A point exactly on the anchor snaps to (0,0) in frame coordinates.
	CHECK(frame.snap_uv(frame.anchor, step).is_equal_approx(Vector2(0, 0)));
	// A point one step along U from the anchor snaps to (1,0).
	CHECK(frame.snap_uv(frame.anchor + frame.u * 1.01, step).is_equal_approx(Vector2(1, 0)));
}

// ------------------------------------------------------------------------- //
// LOD / layout policy (plan sections 5.4, 11).
// ------------------------------------------------------------------------- //

TEST_CASE("[EditorGrid3D] Minor step is base_snap times a power of primary_grid_steps") {
	EditorGridLodSettings s = make_settings();
	const EditorGridFrame3D frame = EditorGridFrame3D::world_xz();

	for (real_t base : { (real_t)0.25, (real_t)1.0, (real_t)2.0 }) {
		s.base_translate_snap = base;
		for (real_t ortho : { (real_t)1.0, (real_t)10.0, (real_t)100.0, (real_t)1000.0 }) {
			const EditorGridLodResult lod = editor_grid_compute_lod(s, make_ortho_camera(ortho), frame);
			CHECK(lod_is_finite(lod));
			// minor == base * steps^floor, major == minor * steps.
			const real_t expected_minor = base * Math::pow(8.0, (double)lod.level_floor);
			CHECK(lod.minor_step == doctest::Approx(expected_minor));
			CHECK(lod.major_step == doctest::Approx(lod.minor_step * 8.0));
			// The ratio minor/base is an exact integer power of steps.
			const real_t ratio = lod.minor_step / base;
			const real_t log_ratio = Math::log(ratio) / Math::log(8.0);
			CHECK(log_ratio == doctest::Approx(Math::round(log_ratio)));
		}
	}
}

TEST_CASE("[EditorGrid3D] Orthographic and perspective layouts are valid and finite") {
	const EditorGridLodSettings s = make_settings();
	const EditorGridFrame3D frame = EditorGridFrame3D::world_xz();

	const EditorGridLodResult ortho = editor_grid_compute_lod(s, make_ortho_camera(50.0), frame);
	CHECK(ortho.valid);
	CHECK(lod_is_finite(ortho));
	CHECK(ortho.projected_pixels_per_base_step > 0);

	const EditorGridLodResult persp = editor_grid_compute_lod(s, make_perspective_camera_above(20.0), frame);
	CHECK(persp.valid);
	CHECK(lod_is_finite(persp));
	CHECK(persp.projected_pixels_per_base_step > 0);

	// Zooming out (camera farther / larger ortho extent) never refines the grid.
	const EditorGridLodResult ortho_far = editor_grid_compute_lod(s, make_ortho_camera(500.0), frame);
	CHECK(ortho_far.level_raw > ortho.level_raw);
	const EditorGridLodResult persp_far = editor_grid_compute_lod(s, make_perspective_camera_above(200.0), frame);
	CHECK(persp_far.level_raw > persp.level_raw);
}

TEST_CASE("[EditorGrid3D] Orthographic density accounts for arbitrary-plane foreshortening") {
	const EditorGridCameraSample cam = make_ortho_camera(20.0);
	const EditorGridFrame3D horizontal = EditorGridFrame3D::world_xz();
	EditorGridFrame3D tilted;
	REQUIRE(EditorGridFrame3D::from_plane_in_space(Vector3(), Vector3(0, 1, 1).normalized(), Transform3D(), Vector3(1, 0, 0), tilted));

	const real_t horizontal_pixels = editor_grid_projected_pixels_per_base_step(cam, horizontal, 1.0);
	const real_t tilted_pixels = editor_grid_projected_pixels_per_base_step(cam, tilted, 1.0);
	CHECK(horizontal_pixels > 0.0);
	CHECK(tilted_pixels > 0.0);
	CHECK(tilted_pixels < horizontal_pixels);
}

TEST_CASE("[EditorGrid3D] Physical division envelope is rounded outward onto every snap lattice") {
	EditorGridLodSettings s;
	s.primary_grid_steps = 8;
	s.division_level_min = 0; // Absolute 1 m legacy minimum.
	s.division_level_max = 2; // Absolute 64 m legacy maximum.
	const EditorGridFrame3D frame = EditorGridFrame3D::world_xz();

	s.base_translate_snap = 0.001;
	const EditorGridLodResult tiny = editor_grid_compute_lod(s, make_ortho_camera(1000000.0), frame);
	CHECK(tiny.level_min == 0);
	CHECK(tiny.level_max >= 6);
	CHECK(tiny.level_min <= 0);
	CHECK(tiny.level_max >= 0);
	CHECK(0.001 * Math::pow(8.0, tiny.level_max) >= 64.0);

	s.base_translate_snap = 10.0;
	const EditorGridLodResult coarse = editor_grid_compute_lod(s, make_ortho_camera(0.1), frame);
	CHECK(coarse.level_min <= -2);
	CHECK(coarse.level_max >= 1);
	CHECK(coarse.level_min <= 0);
	CHECK(coarse.level_max >= 0);
}

TEST_CASE("[EditorGrid3D] primary_grid_steps == 1 uses a guarded fallback (no divide by zero)") {
	EditorGridLodSettings s = make_settings();
	s.primary_grid_steps = 1;
	const EditorGridFrame3D frame = EditorGridFrame3D::world_xz();

	const EditorGridLodResult lod = editor_grid_compute_lod(s, make_ortho_camera(37.0), frame);
	CHECK(lod_is_finite(lod));
	CHECK(lod.minor_step > 0);
	// Guarded factor of 2: major == minor * 2.
	CHECK(lod.major_step == doctest::Approx(lod.minor_step * 2.0));

	// Also robust at the pathological step == 0.
	s.primary_grid_steps = 0;
	const EditorGridLodResult lod0 = editor_grid_compute_lod(s, make_ortho_camera(37.0), frame);
	CHECK(lod_is_finite(lod0));
	CHECK(lod0.minor_step > 0);
}

// ------------------------------------------------------------------------- //
// Semantic rebuild key (plan sections 5.4, 11).
// ------------------------------------------------------------------------- //

static EditorGridRebuildKey key_for(const EditorGridLodSettings &p_s, const EditorGridCameraSample &p_cam, const EditorGridFrame3D &p_frame) {
	const EditorGridLodResult lod = editor_grid_compute_lod(p_s, p_cam, p_frame);
	return editor_grid_make_rebuild_key(p_s, p_cam, p_frame, lod, 0x7, Color(1, 1, 1), Color(0.5, 0.5, 0.5));
}

// Sweep t across [0, 1] with `p_camera_for_t` supplying the camera per sample,
// and count how many times the rebuild key changes along the way.
template <typename F>
static int count_key_changes(const EditorGridLodSettings &p_s, const EditorGridFrame3D &p_frame, F p_camera_for_t) {
	const int samples = 400;
	int changes = 0;
	EditorGridRebuildKey prev = key_for(p_s, p_camera_for_t((real_t)0.0), p_frame);
	for (int i = 1; i <= samples; i++) {
		const EditorGridRebuildKey cur = key_for(p_s, p_camera_for_t((real_t)i / samples), p_frame);
		if (cur != prev) {
			changes++;
			prev = cur;
		}
	}
	return changes;
}

TEST_CASE("[EditorGrid3D] Rebuild key changes exactly once across an LOD boundary") {
	const EditorGridLodSettings s = make_settings();
	const EditorGridFrame3D frame = EditorGridFrame3D::world_xz();

	// With base=1, vp=1000, ref=64, steps=8, bias=0:
	//   level = log2_8(64 * ortho_size / 1000).
	// Sweeping ortho_size so the continuous level runs ~0.5 -> ~1.5 crosses the
	// single integer boundary at level 1.0 exactly once.
	const real_t os_lo = 44.2; // level ~= 0.5
	const real_t os_hi = 353.0; // level ~= 1.5

	const int changes = count_key_changes(s, frame, [&](real_t t) {
		return make_ortho_camera(os_lo + (os_hi - os_lo) * t);
	});
	CHECK(changes == 1);
}

TEST_CASE("[EditorGrid3D] Rebuild key changes exactly once across a center-cell boundary") {
	const EditorGridLodSettings s = make_settings();
	const EditorGridFrame3D frame = EditorGridFrame3D::world_xz();

	// Fixed ortho_size keeps the LOD level (and therefore major_step) constant
	// while the camera pans in-plane along U across one major-cell boundary.
	const real_t ortho = 180.0;
	const EditorGridLodResult probe = editor_grid_compute_lod(s, make_ortho_camera(ortho), frame);
	const real_t major = probe.major_step;
	REQUIRE(major > 0);

	// Pan U from 0.2*major to 1.8*major: crosses exactly one integer cell.
	const int changes = count_key_changes(s, frame, [&](real_t t) {
		return make_ortho_camera(ortho, 1000.0, Vector3(major * (0.2 + 1.6 * t), 10, 0));
	});
	CHECK(changes == 1);
}

TEST_CASE("[EditorGrid3D] Focus point overrides camera origin for center-cell selection") {
	const EditorGridLodSettings s = make_settings();
	const EditorGridFrame3D frame = EditorGridFrame3D::world_xz();

	// Same camera; only the optional focus point differs. Centering must follow
	// the focus point (e.g. an ortho view-ray/plane intersection supplied by the
	// renderer) while the LOD itself is unchanged.
	EditorGridCameraSample cam = make_ortho_camera(180.0);
	const EditorGridLodResult without = editor_grid_compute_lod(s, cam, frame);
	REQUIRE(without.major_step > 0);

	cam.has_focus_point = true;
	cam.focus_point = frame.to_world(Vector3(without.major_step * 5.5, 0, 0));
	const EditorGridLodResult with_focus = editor_grid_compute_lod(s, cam, frame);

	CHECK(with_focus.center_cell_u == 5); // Truncation toward zero, as legacy.
	CHECK(with_focus.center_cell_u != without.center_cell_u);
	CHECK(with_focus.level_floor == without.level_floor); // Focus affects centering only.

	// A non-finite focus point is ignored, not propagated.
	cam.focus_point = Vector3(Math::NaN, 0, 0);
	const EditorGridLodResult with_bad_focus = editor_grid_compute_lod(s, cam, frame);
	CHECK(with_bad_focus.center_cell_u == without.center_cell_u);
	CHECK(lod_is_finite(with_bad_focus));
}

TEST_CASE("[EditorGrid3D] Fade-only change does not change the rebuild key") {
	const EditorGridLodSettings s = make_settings();
	const EditorGridFrame3D frame = EditorGridFrame3D::world_xz();

	// Two ortho sizes that keep floor LOD and center cell identical but produce
	// different continuous fade fractions.
	const EditorGridCameraSample cam_a = make_ortho_camera(150.0);
	const EditorGridCameraSample cam_b = make_ortho_camera(165.0);
	const EditorGridLodResult lod_a = editor_grid_compute_lod(s, cam_a, frame);
	const EditorGridLodResult lod_b = editor_grid_compute_lod(s, cam_b, frame);

	REQUIRE(lod_a.level_floor == lod_b.level_floor);
	REQUIRE(lod_a.center_cell_u == lod_b.center_cell_u);
	REQUIRE(lod_a.center_cell_v == lod_b.center_cell_v);
	CHECK(lod_a.fade != doctest::Approx(lod_b.fade)); // Fade genuinely differs.

	const EditorGridRebuildKey key_a = key_for(s, cam_a, frame);
	const EditorGridRebuildKey key_b = key_for(s, cam_b, frame);
	CHECK(key_a == key_b); // No geometry rebuild for a material-only fade change.
}

TEST_CASE("[EditorGrid3D] Same-LOD resize and sub-tolerance plane jitter do not rebuild") {
	const EditorGridLodSettings s = make_settings();
	const EditorGridFrame3D frame = EditorGridFrame3D::world_xz();
	const EditorGridRebuildKey base_key = key_for(s, make_ortho_camera(150.0, 1000.0), frame);

	const EditorGridRebuildKey resized_key = key_for(s, make_ortho_camera(150.0, 1001.0), frame);
	CHECK(resized_key == base_key);

	EditorGridFrame3D jittered = frame;
	jittered.anchor += frame.u * (base_key.frame_tolerance * 0.25);
	jittered.plane_coordinate += base_key.frame_tolerance * 0.25;
	const EditorGridRebuildKey jittered_key = key_for(s, make_ortho_camera(150.0, 1000.0), jittered);
	CHECK(jittered_key == base_key);

	EditorGridFrame3D moved = frame;
	moved.plane_coordinate += base_key.frame_tolerance * 4.0;
	CHECK(key_for(s, make_ortho_camera(150.0, 1000.0), moved) != base_key);
}

TEST_CASE("[EditorGrid3D] Rebuild key reacts to frame, snap step and settings changes") {
	const EditorGridLodSettings s = make_settings();
	const EditorGridFrame3D frame = EditorGridFrame3D::world_xz();
	const EditorGridCameraSample cam = make_ortho_camera(150.0);
	const EditorGridRebuildKey base_key = key_for(s, cam, frame);

	// Base snap step change.
	EditorGridLodSettings s_snap = s;
	s_snap.base_translate_snap = 0.5;
	CHECK(key_for(s_snap, cam, frame) != base_key);

	// Plane coordinate change (same basis/anchor).
	EditorGridFrame3D frame_moved = frame;
	frame_moved.plane_coordinate = 3.0;
	CHECK(key_for(s, cam, frame_moved) != base_key);

	// Grid extent change.
	EditorGridLodSettings s_ext = s;
	s_ext.grid_half_extent_cells = 50;
	CHECK(key_for(s_ext, cam, frame) != base_key);

	// Color / plane-mask changes are carried by make_rebuild_key arguments.
	const EditorGridLodResult lod = editor_grid_compute_lod(s, cam, frame);
	const EditorGridRebuildKey k_color = editor_grid_make_rebuild_key(s, cam, frame, lod, 0x7, Color(0, 0, 0), Color(0.5, 0.5, 0.5));
	CHECK(k_color != base_key);
	const EditorGridRebuildKey k_mask = editor_grid_make_rebuild_key(s, cam, frame, lod, 0x1, Color(1, 1, 1), Color(0.5, 0.5, 0.5));
	CHECK(k_mask != base_key);
}

// ------------------------------------------------------------------------- //
// Failure / edge behavior (plan sections 11, 12).
// ------------------------------------------------------------------------- //

TEST_CASE("[EditorGrid3D] Invalid camera or frame inputs produce no NaNs") {
	const EditorGridLodSettings s = make_settings();
	const EditorGridFrame3D good = EditorGridFrame3D::world_xz();

	// Invalid (default, zero-basis) frame.
	EditorGridFrame3D bad_frame;
	CHECK_FALSE(bad_frame.is_valid());
	const EditorGridLodResult r_bad_frame = editor_grid_compute_lod(s, make_ortho_camera(50.0), bad_frame);
	CHECK_FALSE(r_bad_frame.valid);
	CHECK(lod_is_finite(r_bad_frame));

	// Zero-height viewport.
	EditorGridCameraSample zero_vp = make_ortho_camera(50.0);
	zero_vp.viewport_height = 0.0;
	zero_vp.viewport_width = 0.0;
	CHECK(lod_is_finite(editor_grid_compute_lod(s, zero_vp, good)));

	// Zero ortho size.
	EditorGridCameraSample zero_size = make_ortho_camera(0.0);
	CHECK(lod_is_finite(editor_grid_compute_lod(s, zero_size, good)));

	// Camera exactly on the plane (perspective, height 0): sample degenerates.
	CHECK(lod_is_finite(editor_grid_compute_lod(s, make_perspective_camera_above(0.0), good)));

	// Camera nearly parallel to the plane (grazing / horizon).
	EditorGridCameraSample grazing;
	grazing.orthogonal = false;
	grazing.viewport_width = 1000.0;
	grazing.viewport_height = 1000.0;
	grazing.projection = Projection::create_perspective(70.0, 1.0, 0.05, 4000.0);
	grazing.camera_transform.set_look_at(Vector3(0, 0.001, 100), Vector3(0, 0.0, 0), Vector3(0, 1, 0));
	const EditorGridLodResult r_grazing = editor_grid_compute_lod(s, grazing, good);
	CHECK(lod_is_finite(r_grazing));
	// Never explodes: level stays within the clamp bounds.
	CHECK(r_grazing.level_clamped <= r_grazing.level_raw + 1e-3 + 20.0);
}

TEST_CASE("[EditorGrid3D] Invalid settings stay finite and fade radius never becomes negative") {
	EditorGridLodSettings s = make_settings();
	s.division_level_bias = Math::NaN;
	s.reference_pixels = Math::NaN;
	s.division_level_min = 3;
	s.division_level_max = -2;
	s.grid_half_extent_cells = 1;
	s.primary_grid_steps = 1;

	const EditorGridLodResult lod = editor_grid_compute_lod(s, make_ortho_camera(50.0), EditorGridFrame3D::world_xz());
	CHECK(lod_is_finite(lod));
	CHECK(lod.fade_distance >= 0.0);
	CHECK(lod.level_min <= 0);
	CHECK(lod.level_max >= 0);
}

TEST_CASE("[EditorGrid3D] Translate-snap normalization keeps the base step positive") {
	CHECK(editor_grid_normalize_translate_snap(0.0) == doctest::Approx((double)EDITOR_GRID_MIN_TRANSLATE_SNAP));
	CHECK(editor_grid_normalize_translate_snap(-5.0) == doctest::Approx((double)EDITOR_GRID_MIN_TRANSLATE_SNAP));
	CHECK(editor_grid_normalize_translate_snap(Math::NaN) == doctest::Approx((double)EDITOR_GRID_MIN_TRANSLATE_SNAP));
	CHECK(editor_grid_normalize_translate_snap(2.5) == doctest::Approx(2.5));
}

TEST_CASE("[EditorGrid3D] Snap modifier effects distinguish native and domain policy") {
	CHECK(editor_snap_is_enabled(false, true, EditorSnapModifierEffect::NATIVE));
	CHECK_FALSE(editor_snap_is_enabled(true, true, EditorSnapModifierEffect::NATIVE));
	CHECK_FALSE(editor_snap_is_enabled(false, true, EditorSnapModifierEffect::NONE));
	CHECK(editor_snap_is_enabled(true, true, EditorSnapModifierEffect::NONE));

	CHECK(editor_snap_apply_fine_step(1.0, true, 10.0, EditorSnapModifierEffect::NATIVE) == doctest::Approx(0.1));
	CHECK(editor_snap_apply_fine_step(1.0, true, 10.0, EditorSnapModifierEffect::NONE) == doctest::Approx(1.0));
	CHECK(editor_snap_apply_fine_step(1.0, false, 10.0, EditorSnapModifierEffect::NATIVE) == doctest::Approx(1.0));
}

TEST_CASE("[EditorGrid3D] One-shot position snapping removes the absolute world-grid offset") {
	const Vector3 original(1.24, -1.26, 2.74);
	CHECK(editor_grid_snap_position(original, 0.5).is_equal_approx(Vector3(1.0, -1.5, 2.5)));

	// Unusable steps pass the position through instead of collapsing it to zero.
	CHECK(editor_grid_snap_position(original, 0.0).is_equal_approx(original));
	CHECK(editor_grid_snap_position(original, -1.0).is_equal_approx(original));
	CHECK(editor_grid_snap_position(original, Math::NaN).is_equal_approx(original));
}

TEST_CASE("[EditorGrid3D] Private viewport layers are unique per scenario and fail closed") {
	Editor3DViewportLayerPool pool;
	Editor3DViewportLayerLease leases[Editor3DViewportLayerPool::LAYER_COUNT];
	uint32_t layer_mask = 0;
	for (int i = 0; i < Editor3DViewportLayerPool::LAYER_COUNT; i++) {
		leases[i] = pool.acquire(101);
		REQUIRE(leases[i].is_valid());
		const uint32_t bit = uint32_t(1) << leases[i].layer;
		CHECK((layer_mask & bit) == 0);
		layer_mask |= bit;
	}
	CHECK(leases[Editor3DViewportLayerPool::LAYER_COUNT - 1].layer == 31);
	CHECK((layer_mask & (uint32_t(1) << 31)) != 0);
	CHECK(pool.get_active_count(101) == 9);
	CHECK_FALSE(pool.acquire(101).is_valid());

	// A different scenario has its own independent slots.
	const Editor3DViewportLayerLease other = pool.acquire(202);
	REQUIRE(other.is_valid());
	CHECK(other.layer == 20);

	// Release is balanced and idempotent; the freed slot is reused.
	pool.release(leases[3]);
	pool.release(leases[3]);
	CHECK(pool.get_active_count(101) == 8);
	const Editor3DViewportLayerLease reused = pool.acquire(101);
	REQUIRE(reused.is_valid());
	CHECK(reused.layer == leases[3].layer);
	CHECK(pool.get_active_count(101) == 9);
}

} // namespace TestEditorGrid3D

#endif // TOOLS_ENABLED
