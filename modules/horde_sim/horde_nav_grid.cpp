/**************************************************************************/
/*  horde_nav_grid.cpp                                                     */
/**************************************************************************/

#include "horde_nav_grid.h"

#include "core/object/class_db.h"

Vector3i HordeNavGrid::index_to_cell(int32_t p_index) const {
	if (unlikely(width <= 0 || height <= 0 || p_index < 0)) {
		return Vector3i();
	}
	const int32_t layer = width * height;
	const int32_t f = p_index / layer;
	const int32_t rem = p_index - f * layer;
	return Vector3i(rem % width, rem / width, f);
}

void HordeNavGrid::resize(int32_t p_width, int32_t p_height, int32_t p_floors) {
	ERR_FAIL_COND_MSG(p_width <= 0 || p_height <= 0 || p_floors <= 0, "Grid dimensions must be positive.");
	width = p_width;
	height = p_height;
	floors = p_floors;

	const uint32_t count = (uint32_t)(width * height * floors);
	walkable.resize(count);
	blocked.resize(count);
	base_cost.resize(count);
	dynamic_cost.resize(count);

	for (uint32_t i = 0; i < count; i++) {
		walkable[i] = 1;
		blocked[i] = 0;
		base_cost[i] = DEFAULT_BASE_COST;
		dynamic_cost[i] = 0;
	}
	links.clear();
	_touch();
}

void HordeNavGrid::configure(float p_cell_size, const Vector2 &p_origin, float p_floor_height) {
	ERR_FAIL_COND_MSG(p_cell_size <= 0.0f, "cell_size must be positive.");
	ERR_FAIL_COND_MSG(p_floor_height <= 0.0f, "floor_height must be positive.");
	cell_size = p_cell_size;
	origin = p_origin;
	floor_height = p_floor_height;
	_touch();
}

void HordeNavGrid::fill_walkable(bool p_walkable) {
	const uint8_t v = p_walkable ? 1 : 0;
	for (uint32_t i = 0; i < walkable.size(); i++) {
		walkable[i] = v;
	}
	_touch();
}

void HordeNavGrid::clear_dynamic_cost() {
	for (uint32_t i = 0; i < dynamic_cost.size(); i++) {
		dynamic_cost[i] = 0;
		blocked[i] = 0;
	}
	_touch();
}

void HordeNavGrid::set_walkable(int32_t p_x, int32_t p_y, int32_t p_floor, bool p_walkable) {
	ERR_FAIL_COND(!in_bounds(p_x, p_y, p_floor));
	walkable[cell_index(p_x, p_y, p_floor)] = p_walkable ? 1 : 0;
	_touch();
}

bool HordeNavGrid::is_walkable(int32_t p_x, int32_t p_y, int32_t p_floor) const {
	ERR_FAIL_COND_V(!in_bounds(p_x, p_y, p_floor), false);
	return walkable[cell_index(p_x, p_y, p_floor)] != 0;
}

void HordeNavGrid::set_base_cost(int32_t p_x, int32_t p_y, int32_t p_floor, int32_t p_cost) {
	ERR_FAIL_COND(!in_bounds(p_x, p_y, p_floor));
	base_cost[cell_index(p_x, p_y, p_floor)] = (uint16_t)CLAMP(p_cost, 0, 65535);
	_touch();
}

int32_t HordeNavGrid::get_base_cost(int32_t p_x, int32_t p_y, int32_t p_floor) const {
	ERR_FAIL_COND_V(!in_bounds(p_x, p_y, p_floor), 0);
	return base_cost[cell_index(p_x, p_y, p_floor)];
}

void HordeNavGrid::set_blocked(int32_t p_x, int32_t p_y, int32_t p_floor, bool p_blocked) {
	ERR_FAIL_COND(!in_bounds(p_x, p_y, p_floor));
	blocked[cell_index(p_x, p_y, p_floor)] = p_blocked ? 1 : 0;
	_touch();
}

bool HordeNavGrid::is_blocked(int32_t p_x, int32_t p_y, int32_t p_floor) const {
	ERR_FAIL_COND_V(!in_bounds(p_x, p_y, p_floor), true);
	return blocked[cell_index(p_x, p_y, p_floor)] != 0;
}

void HordeNavGrid::set_dynamic_cost(int32_t p_x, int32_t p_y, int32_t p_floor, int32_t p_cost) {
	ERR_FAIL_COND(!in_bounds(p_x, p_y, p_floor));
	dynamic_cost[cell_index(p_x, p_y, p_floor)] = (uint16_t)CLAMP(p_cost, 0, 65535);
	_touch();
}

int32_t HordeNavGrid::get_dynamic_cost(int32_t p_x, int32_t p_y, int32_t p_floor) const {
	ERR_FAIL_COND_V(!in_bounds(p_x, p_y, p_floor), 0);
	return dynamic_cost[cell_index(p_x, p_y, p_floor)];
}

bool HordeNavGrid::is_passable(int32_t p_x, int32_t p_y, int32_t p_floor) const {
	if (!in_bounds(p_x, p_y, p_floor)) {
		return false;
	}
	const int32_t i = cell_index(p_x, p_y, p_floor);
	return walkable[i] != 0 && blocked[i] == 0;
}

void HordeNavGrid::set_walkable_rect(int32_t p_x0, int32_t p_y0, int32_t p_x1, int32_t p_y1, int32_t p_floor, bool p_walkable) {
	ERR_FAIL_INDEX(p_floor, floors);
	const int32_t x0 = MAX(0, MIN(p_x0, p_x1));
	const int32_t x1 = MIN(width - 1, MAX(p_x0, p_x1));
	const int32_t y0 = MAX(0, MIN(p_y0, p_y1));
	const int32_t y1 = MIN(height - 1, MAX(p_y0, p_y1));
	const uint8_t v = p_walkable ? 1 : 0;
	for (int32_t y = y0; y <= y1; y++) {
		for (int32_t x = x0; x <= x1; x++) {
			walkable[cell_index(x, y, p_floor)] = v;
		}
	}
	_touch();
}

void HordeNavGrid::set_base_cost_rect(int32_t p_x0, int32_t p_y0, int32_t p_x1, int32_t p_y1, int32_t p_floor, int32_t p_cost) {
	ERR_FAIL_INDEX(p_floor, floors);
	const int32_t x0 = MAX(0, MIN(p_x0, p_x1));
	const int32_t x1 = MIN(width - 1, MAX(p_x0, p_x1));
	const int32_t y0 = MAX(0, MIN(p_y0, p_y1));
	const int32_t y1 = MIN(height - 1, MAX(p_y0, p_y1));
	const uint16_t v = (uint16_t)CLAMP(p_cost, 0, 65535);
	for (int32_t y = y0; y <= y1; y++) {
		for (int32_t x = x0; x <= x1; x++) {
			base_cost[cell_index(x, y, p_floor)] = v;
		}
	}
	_touch();
}

void HordeNavGrid::set_blocked_rect(int32_t p_x0, int32_t p_y0, int32_t p_x1, int32_t p_y1, int32_t p_floor, bool p_blocked) {
	ERR_FAIL_INDEX(p_floor, floors);
	const int32_t x0 = MAX(0, MIN(p_x0, p_x1));
	const int32_t x1 = MIN(width - 1, MAX(p_x0, p_x1));
	const int32_t y0 = MAX(0, MIN(p_y0, p_y1));
	const int32_t y1 = MIN(height - 1, MAX(p_y0, p_y1));
	const uint8_t v = p_blocked ? 1 : 0;
	for (int32_t y = y0; y <= y1; y++) {
		for (int32_t x = x0; x <= x1; x++) {
			blocked[cell_index(x, y, p_floor)] = v;
		}
	}
	_touch();
}

void HordeNavGrid::set_dynamic_cost_rect(int32_t p_x0, int32_t p_y0, int32_t p_x1, int32_t p_y1, int32_t p_floor, int32_t p_cost) {
	ERR_FAIL_INDEX(p_floor, floors);
	const int32_t x0 = MAX(0, MIN(p_x0, p_x1));
	const int32_t x1 = MIN(width - 1, MAX(p_x0, p_x1));
	const int32_t y0 = MAX(0, MIN(p_y0, p_y1));
	const int32_t y1 = MIN(height - 1, MAX(p_y0, p_y1));
	const uint16_t v = (uint16_t)CLAMP(p_cost, 0, 65535);
	for (int32_t y = y0; y <= y1; y++) {
		for (int32_t x = x0; x <= x1; x++) {
			dynamic_cost[cell_index(x, y, p_floor)] = v;
		}
	}
	_touch();
}

void HordeNavGrid::link_cells(const Vector3i &p_a, const Vector3i &p_b, int32_t p_cost) {
	ERR_FAIL_COND(!in_bounds(p_a.x, p_a.y, p_a.z));
	ERR_FAIL_COND(!in_bounds(p_b.x, p_b.y, p_b.z));
	const uint16_t c = (uint16_t)CLAMP(p_cost, 1, 65535);
	GridLink ab;
	ab.from = cell_index_v(p_a);
	ab.to = cell_index_v(p_b);
	ab.cost = c;
	links.push_back(ab);
	GridLink ba;
	ba.from = cell_index_v(p_b);
	ba.to = cell_index_v(p_a);
	ba.cost = c;
	links.push_back(ba);
	_touch();
}

void HordeNavGrid::clear_links() {
	links.clear();
	_touch();
}

Vector3i HordeNavGrid::world_to_cell(const Vector3 &p_world) const {
	const int32_t x = (int32_t)Math::floor((p_world.x - origin.x) / cell_size);
	const int32_t y = (int32_t)Math::floor((p_world.z - origin.y) / cell_size);
	int32_t f = (int32_t)Math::round(p_world.y / floor_height);
	f = CLAMP(f, 0, floors - 1);
	return Vector3i(CLAMP(x, 0, width - 1), CLAMP(y, 0, height - 1), f);
}

Vector3 HordeNavGrid::cell_to_world(int32_t p_x, int32_t p_y, int32_t p_floor) const {
	return Vector3(
			origin.x + (p_x + 0.5f) * cell_size,
			p_floor * floor_height,
			origin.y + (p_y + 0.5f) * cell_size);
}

void HordeNavGrid::_bind_methods() {
	ClassDB::bind_method(D_METHOD("resize", "width", "height", "floors"), &HordeNavGrid::resize, DEFVAL(1));
	ClassDB::bind_method(D_METHOD("configure", "cell_size", "origin", "floor_height"), &HordeNavGrid::configure, DEFVAL(DEFAULT_CELL_SIZE), DEFVAL(Vector2()), DEFVAL(DEFAULT_FLOOR_HEIGHT));
	ClassDB::bind_method(D_METHOD("fill_walkable", "walkable"), &HordeNavGrid::fill_walkable);
	ClassDB::bind_method(D_METHOD("clear_dynamic_cost"), &HordeNavGrid::clear_dynamic_cost);

	ClassDB::bind_method(D_METHOD("get_width"), &HordeNavGrid::get_width);
	ClassDB::bind_method(D_METHOD("get_height"), &HordeNavGrid::get_height);
	ClassDB::bind_method(D_METHOD("get_floors"), &HordeNavGrid::get_floors);
	ClassDB::bind_method(D_METHOD("get_cell_count"), &HordeNavGrid::get_cell_count);
	ClassDB::bind_method(D_METHOD("get_cell_size"), &HordeNavGrid::get_cell_size);
	ClassDB::bind_method(D_METHOD("get_floor_height"), &HordeNavGrid::get_floor_height);
	ClassDB::bind_method(D_METHOD("get_origin"), &HordeNavGrid::get_origin);
	ClassDB::bind_method(D_METHOD("get_version"), &HordeNavGrid::get_version);

	ClassDB::bind_method(D_METHOD("cell_index", "x", "y", "floor"), &HordeNavGrid::cell_index, DEFVAL(0));
	ClassDB::bind_method(D_METHOD("index_to_cell", "index"), &HordeNavGrid::index_to_cell);

	ClassDB::bind_method(D_METHOD("set_walkable", "x", "y", "floor", "walkable"), &HordeNavGrid::set_walkable);
	ClassDB::bind_method(D_METHOD("is_walkable", "x", "y", "floor"), &HordeNavGrid::is_walkable);
	ClassDB::bind_method(D_METHOD("set_base_cost", "x", "y", "floor", "cost"), &HordeNavGrid::set_base_cost);
	ClassDB::bind_method(D_METHOD("get_base_cost", "x", "y", "floor"), &HordeNavGrid::get_base_cost);
	ClassDB::bind_method(D_METHOD("set_blocked", "x", "y", "floor", "blocked"), &HordeNavGrid::set_blocked);
	ClassDB::bind_method(D_METHOD("is_blocked", "x", "y", "floor"), &HordeNavGrid::is_blocked);
	ClassDB::bind_method(D_METHOD("set_dynamic_cost", "x", "y", "floor", "cost"), &HordeNavGrid::set_dynamic_cost);
	ClassDB::bind_method(D_METHOD("get_dynamic_cost", "x", "y", "floor"), &HordeNavGrid::get_dynamic_cost);
	ClassDB::bind_method(D_METHOD("is_passable", "x", "y", "floor"), &HordeNavGrid::is_passable);

	ClassDB::bind_method(D_METHOD("set_walkable_rect", "x0", "y0", "x1", "y1", "floor", "walkable"), &HordeNavGrid::set_walkable_rect);
	ClassDB::bind_method(D_METHOD("set_base_cost_rect", "x0", "y0", "x1", "y1", "floor", "cost"), &HordeNavGrid::set_base_cost_rect);
	ClassDB::bind_method(D_METHOD("set_blocked_rect", "x0", "y0", "x1", "y1", "floor", "blocked"), &HordeNavGrid::set_blocked_rect);
	ClassDB::bind_method(D_METHOD("set_dynamic_cost_rect", "x0", "y0", "x1", "y1", "floor", "cost"), &HordeNavGrid::set_dynamic_cost_rect);

	ClassDB::bind_method(D_METHOD("link_cells", "a", "b", "cost"), &HordeNavGrid::link_cells, DEFVAL(1));
	ClassDB::bind_method(D_METHOD("clear_links"), &HordeNavGrid::clear_links);

	ClassDB::bind_method(D_METHOD("world_to_cell", "world_position"), &HordeNavGrid::world_to_cell);
	ClassDB::bind_method(D_METHOD("cell_to_world", "x", "y", "floor"), &HordeNavGrid::cell_to_world);
}
