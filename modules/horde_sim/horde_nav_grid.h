/**************************************************************************/
/*  horde_nav_grid.h                                                      */
/**************************************************************************/

#pragma once

#include "core/math/vector2.h"
#include "core/math/vector3.h"
#include "core/math/vector3i.h"
#include "core/object/ref_counted.h"
#include "core/templates/local_vector.h"

// Walkable grid for the horde flow-field system (DES G6.4).
//
// The grid is owned by the horde system, NOT Godot's NavigationServer: flow
// fields want a dense grid, and dynamic cost (boards/doors) wants direct O(1)
// cell writes rather than a navmesh re-bake. Layout is struct-of-arrays with
// flat, cache-friendly index math and no per-cell heap objects.
//
// Coordinates: a stack of `floors` 2D layers. Planar axes (x, y) map to world
// (X, Z); `floor` maps to world Y. Cell index is flat:
//   index = (floor * height + y) * width + x
//
// Dynamic edits bump a version counter that flow fields watch to debounce
// recomputes. Grid geometry (dimensions/cell_size) must be fixed via resize()
// before any field samples it; per-cell walkability/cost may change at runtime.
class HordeFlowField;

class HordeNavGrid : public RefCounted {
	GDCLASS(HordeNavGrid, RefCounted);

	friend class HordeFlowField;

public:
	// Cost is stored as a small additive weight per cell. Effective traversal
	// cost of entering a cell is (base_cost + dynamic_cost), clamped to >= 1 so
	// every passable cell costs something (keeps Dijkstra well-ordered).
	static constexpr uint16_t DEFAULT_BASE_COST = 1;
	static constexpr float DEFAULT_CELL_SIZE = 0.5f; // Doorway resolution.
	static constexpr float DEFAULT_FLOOR_HEIGHT = 3.0f;

private:
	int32_t width = 0;
	int32_t height = 0;
	int32_t floors = 1;
	float cell_size = DEFAULT_CELL_SIZE;
	float floor_height = DEFAULT_FLOOR_HEIGHT;
	Vector2 origin; // World (X, Z) of the min corner of cell (0, 0, 0).

	// Struct-of-arrays, all sized width*height*floors.
	LocalVector<uint8_t> walkable; // Static geometry passability.
	LocalVector<uint8_t> blocked; // Dynamic block (boards/doors); cost layer.
	LocalVector<uint16_t> base_cost; // Static terrain weight.
	LocalVector<uint16_t> dynamic_cost; // Dynamic additive weight; cost layer.

	// Inter-floor connectivity (ladders/stairs). Stored as an explicit link
	// list; the flow field folds these into Dijkstra via a CSR built at
	// dispatch. Kept out of the planar inner loop so the common case is fast.
	struct GridLink {
		int32_t from = -1;
		int32_t to = -1;
		uint16_t cost = 1;
	};
	LocalVector<GridLink> links;

	uint32_t version = 1; // Bumped on any cell/link mutation.

	_FORCE_INLINE_ void _touch() { version++; }

	static void _bind_methods();

public:
	_FORCE_INLINE_ int32_t get_width() const { return width; }
	_FORCE_INLINE_ int32_t get_height() const { return height; }
	_FORCE_INLINE_ int32_t get_floors() const { return floors; }
	_FORCE_INLINE_ int32_t get_cell_count() const { return width * height * floors; }
	_FORCE_INLINE_ float get_cell_size() const { return cell_size; }
	_FORCE_INLINE_ float get_floor_height() const { return floor_height; }
	Vector2 get_origin() const { return origin; }
	uint32_t get_version() const { return version; }

	_FORCE_INLINE_ bool in_bounds(int32_t p_x, int32_t p_y, int32_t p_floor) const {
		return p_x >= 0 && p_x < width && p_y >= 0 && p_y < height && p_floor >= 0 && p_floor < floors;
	}
	_FORCE_INLINE_ int32_t cell_index(int32_t p_x, int32_t p_y, int32_t p_floor) const {
		return (p_floor * height + p_y) * width + p_x;
	}
	Vector3i index_to_cell(int32_t p_index) const;
	int32_t cell_index_v(const Vector3i &p_cell) const { return cell_index(p_cell.x, p_cell.y, p_cell.z); }

	// Allocation / geometry. Resets all cells to a walkable default.
	void resize(int32_t p_width, int32_t p_height, int32_t p_floors);
	void configure(float p_cell_size, const Vector2 &p_origin, float p_floor_height);
	void fill_walkable(bool p_walkable);
	void clear_dynamic_cost(); // Zeroes dynamic cost + unblocks all cells.

	// Per-cell authoring.
	void set_walkable(int32_t p_x, int32_t p_y, int32_t p_floor, bool p_walkable);
	bool is_walkable(int32_t p_x, int32_t p_y, int32_t p_floor) const;
	void set_base_cost(int32_t p_x, int32_t p_y, int32_t p_floor, int32_t p_cost);
	int32_t get_base_cost(int32_t p_x, int32_t p_y, int32_t p_floor) const;

	// Cost layer (board/door anchors register here).
	void set_blocked(int32_t p_x, int32_t p_y, int32_t p_floor, bool p_blocked);
	bool is_blocked(int32_t p_x, int32_t p_y, int32_t p_floor) const;
	void set_dynamic_cost(int32_t p_x, int32_t p_y, int32_t p_floor, int32_t p_cost);
	int32_t get_dynamic_cost(int32_t p_x, int32_t p_y, int32_t p_floor) const;

	// Effective passability/cost used by the flow field.
	bool is_passable(int32_t p_x, int32_t p_y, int32_t p_floor) const;

	// Rectangle authoring on a single floor (rows [y0,y1], cols [x0,x1]).
	void set_walkable_rect(int32_t p_x0, int32_t p_y0, int32_t p_x1, int32_t p_y1, int32_t p_floor, bool p_walkable);
	void set_base_cost_rect(int32_t p_x0, int32_t p_y0, int32_t p_x1, int32_t p_y1, int32_t p_floor, int32_t p_cost);
	void set_blocked_rect(int32_t p_x0, int32_t p_y0, int32_t p_x1, int32_t p_y1, int32_t p_floor, bool p_blocked);
	void set_dynamic_cost_rect(int32_t p_x0, int32_t p_y0, int32_t p_x1, int32_t p_y1, int32_t p_floor, int32_t p_cost);

	// Inter-floor links (bidirectional ladder/stair between two cells).
	void link_cells(const Vector3i &p_a, const Vector3i &p_b, int32_t p_cost);
	void clear_links();

	// World <-> cell transforms.
	Vector3i world_to_cell(const Vector3 &p_world) const;
	Vector3 cell_to_world(int32_t p_x, int32_t p_y, int32_t p_floor) const; // Cell center.
	Vector3 cell_to_world_v(const Vector3i &p_cell) const { return cell_to_world(p_cell.x, p_cell.y, p_cell.z); }

	HordeNavGrid() {}
};
