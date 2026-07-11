/**************************************************************************/
/*  test_horde_nav_grid.h                                                */
/**************************************************************************/

#pragma once

#include "horde_test_helpers.h"

namespace TestHordeSim {

// ---------------------------------------------------------------------------
// Grid transform sanity.
// ---------------------------------------------------------------------------
TEST_CASE("[HordeSim][NavGrid] World<->cell transforms round-trip") {
	Ref<HordeNavGrid> grid = make_grid(10, 10, 3);
	grid->configure(0.5f, Vector2(-2.0f, -3.0f), 3.0f);

	const Vector3 w = grid->cell_to_world(4, 6, 2);
	const Vector3i c = grid->world_to_cell(w);
	CHECK(c == Vector3i(4, 6, 2));

	// Cell center of (0,0,0) sits half a cell in from the origin corner.
	const Vector3 c0 = grid->cell_to_world(0, 0, 0);
	CHECK(c0.x == doctest::Approx(-2.0f + 0.25f));
	CHECK(c0.z == doctest::Approx(-3.0f + 0.25f));
	CHECK(c0.y == doctest::Approx(0.0f));
}

} // namespace TestHordeSim
