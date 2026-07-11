/**************************************************************************/
/*  horde_test_helpers.h                                                 */
/**************************************************************************/

#pragma once

// Shared fixtures for the [HordeSim] suites (test_horde_flow_field.h,
// test_horde_nav_grid.h, test_horde_agents.h). Pulled in once per file via
// #include; also auto-discovered directly by the modules test glob
// (modules/modules_builders.py globs `tests/*.h`), which is harmless since
// this header only declares helpers -- no TEST_CASEs of its own -- and
// #pragma once dedupes it in the single generated test translation unit.

#include "../horde_agents.h"
#include "../horde_flow_field.h"
#include "../horde_fsm_config.h"
#include "../horde_nav_grid.h"

#include "core/os/os.h"
#include "core/os/thread.h"
#include "core/string/print_string.h"
#include "core/templates/local_vector.h"

#include "tests/test_macros.h"

// Full-path includes (see box3d_ragdoll.h): the tests library env does not carry
// the Box3D include dir, but the project root is always on the include path.
#include <thirdparty/box3d/include/box3d/box3d.h>
#include <thirdparty/box3d/include/box3d/collision.h>
#include <thirdparty/box3d/include/box3d/id.h>

namespace TestHordeSim {

static Ref<HordeNavGrid> make_grid(int w, int h, int floors = 1) {
	Ref<HordeNavGrid> grid;
	grid.instantiate();
	grid->resize(w, h, floors);
	grid->configure(HordeNavGrid::DEFAULT_CELL_SIZE, Vector2(), HordeNavGrid::DEFAULT_FLOOR_HEIGHT);
	return grid;
}

// A self-contained Box3D world with static wall boxes, for the native mover
// sweep tests. Owns its hulls (kept alive for the world's lifetime) and world.
struct TestWorld {
	b3WorldId world = b3_nullWorldId;
	LocalVector<b3BoxHull> hulls;

	TestWorld() {
		b3WorldDef wd = b3DefaultWorldDef();
		world = b3CreateWorld(&wd);
	}
	~TestWorld() {
		if (B3_IS_NON_NULL(world)) {
			b3DestroyWorld(world);
		}
	}
	// Axis-aligned static box centered at (cx,cy,cz) with half extents (hx,hy,hz).
	void add_wall(float cx, float cy, float cz, float hx, float hy, float hz) {
		b3BodyDef bd = b3DefaultBodyDef();
		bd.type = b3_staticBody;
		bd.position = b3Vec3{ cx, cy, cz };
		b3BodyId body = b3CreateBody(world, &bd);
		hulls.push_back(b3MakeBoxHull(hx, hy, hz));
		b3ShapeDef sd = b3DefaultShapeDef();
		b3CreateHullShape(body, &sd, &hulls[hulls.size() - 1].base);
	}
	void finalize() {
		// Populate the broadphase so queries see the static geometry.
		b3World_Step(world, 1.0f / 128.0f, 1);
	}
	uint32_t packed() const { return b3StoreWorldId(world); }
};

static Ref<HordeAgents> make_agents(int capacity = 250) {
	Ref<HordeAgents> a;
	a.instantiate();
	a->set_capacity(capacity);
	return a;
}

} // namespace TestHordeSim
