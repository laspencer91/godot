/**************************************************************************/
/*  level_mesh_data.cpp                                                   */
/**************************************************************************/

#include "level_mesh_data.h"

#include "level_mesh_diff.h"

#include "core/object/class_db.h"

#define LEVEL_MESH_DATA_ACCESSORS(m_type, m_name, m_member) \
	void LevelMeshData::set_##m_name(const m_type &p_values) { \
		m_member = p_values; \
		emit_changed(); \
	} \
	m_type LevelMeshData::get_##m_name() const { \
		return m_member; \
	}

void LevelMeshData::_emit_mesh_diff_applied(const Ref<LevelMeshDiff> &p_diff, bool p_reverted) {
	emit_signal(SNAME("mesh_diff_applied"), p_diff, p_reverted);
}

void LevelMeshData::_emit_mesh_preview_changed() {
	emit_signal(SNAME("mesh_preview_changed"));
}

LEVEL_MESH_DATA_ACCESSORS(PackedVector3Array, vertex_positions, vertex_positions)
LEVEL_MESH_DATA_ACCESSORS(PackedInt32Array, free_vertex_ids, free_vertex_ids)

LEVEL_MESH_DATA_ACCESSORS(PackedInt32Array, edge_vertices, edge_vertices)
LEVEL_MESH_DATA_ACCESSORS(PackedInt32Array, free_edge_ids, free_edge_ids)

LEVEL_MESH_DATA_ACCESSORS(PackedInt32Array, face_loop_starts, face_loop_starts)
LEVEL_MESH_DATA_ACCESSORS(PackedInt32Array, face_loop_counts, face_loop_counts)
LEVEL_MESH_DATA_ACCESSORS(PackedInt32Array, face_material_indices, face_material_indices)
LEVEL_MESH_DATA_ACCESSORS(PackedInt32Array, face_uv_modes, face_uv_modes)
LEVEL_MESH_DATA_ACCESSORS(PackedVector3Array, face_uv_origins, face_uv_origins)
LEVEL_MESH_DATA_ACCESSORS(PackedVector3Array, face_uv_tangents, face_uv_tangents)
LEVEL_MESH_DATA_ACCESSORS(PackedFloat32Array, face_uv_transforms, face_uv_transforms)
LEVEL_MESH_DATA_ACCESSORS(PackedInt32Array, face_polygroup_ids, face_polygroup_ids)
LEVEL_MESH_DATA_ACCESSORS(PackedInt32Array, face_flags, face_flags)
LEVEL_MESH_DATA_ACCESSORS(PackedInt32Array, free_face_ids, free_face_ids)

LEVEL_MESH_DATA_ACCESSORS(PackedInt32Array, loop_vertex_indices, loop_vertex_indices)
LEVEL_MESH_DATA_ACCESSORS(PackedVector2Array, loop_uv0, loop_uv0)
LEVEL_MESH_DATA_ACCESSORS(PackedColorArray, loop_colors, loop_colors)
LEVEL_MESH_DATA_ACCESSORS(PackedVector3Array, loop_normals, loop_normals)
LEVEL_MESH_DATA_ACCESSORS(PackedByteArray, loop_alive, loop_alive)
LEVEL_MESH_DATA_ACCESSORS(PackedInt32Array, free_loop_ids, free_loop_ids)

#undef LEVEL_MESH_DATA_ACCESSORS

void LevelMeshData::set_vertex_alive(const PackedByteArray &p_values) {
	vertex_alive = p_values;
	_ensure_generation_columns();
	emit_changed();
}

PackedByteArray LevelMeshData::get_vertex_alive() const {
	return vertex_alive;
}

void LevelMeshData::set_vertex_generations(const PackedInt32Array &p_values) {
	vertex_generations = p_values;
	for (int i = 0; i < vertex_generations.size(); i++) {
		_advance_generation_counter(_generation_at(vertex_generations, i), next_vertex_generation);
	}
	_ensure_generation_columns();
	emit_changed();
}

PackedInt32Array LevelMeshData::get_vertex_generations() const {
	return vertex_generations;
}

void LevelMeshData::set_edge_alive(const PackedByteArray &p_values) {
	edge_alive = p_values;
	_ensure_generation_columns();
	emit_changed();
}

PackedByteArray LevelMeshData::get_edge_alive() const {
	return edge_alive;
}

void LevelMeshData::set_edge_generations(const PackedInt32Array &p_values) {
	edge_generations = p_values;
	for (int i = 0; i < edge_generations.size(); i++) {
		_advance_generation_counter(_generation_at(edge_generations, i), next_edge_generation);
	}
	_ensure_generation_columns();
	emit_changed();
}

PackedInt32Array LevelMeshData::get_edge_generations() const {
	return edge_generations;
}

void LevelMeshData::set_face_alive(const PackedByteArray &p_values) {
	face_alive = p_values;
	_ensure_generation_columns();
	emit_changed();
}

PackedByteArray LevelMeshData::get_face_alive() const {
	return face_alive;
}

void LevelMeshData::set_face_generations(const PackedInt32Array &p_values) {
	face_generations = p_values;
	for (int i = 0; i < face_generations.size(); i++) {
		_advance_generation_counter(_generation_at(face_generations, i), next_face_generation);
	}
	_ensure_generation_columns();
	emit_changed();
}

PackedInt32Array LevelMeshData::get_face_generations() const {
	return face_generations;
}

uint32_t LevelMeshData::_bump_generation(uint32_t p_generation) {
	uint32_t next = p_generation + 1;
	if (next == 0) {
		next = 1;
	}
	return next;
}

bool LevelMeshData::_append_free_id(PackedInt32Array &r_free_ids, int p_slot) {
	if (r_free_ids.has(p_slot)) {
		return false;
	}
	r_free_ids.push_back(p_slot);
	return true;
}

uint32_t LevelMeshData::_generation_at(const PackedInt32Array &p_generations, int p_slot) {
	if (p_slot < 0 || p_slot >= p_generations.size()) {
		return 0;
	}
	return (uint32_t)p_generations[p_slot];
}

void LevelMeshData::_advance_generation_counter(uint32_t p_generation, uint32_t &r_next_generation) {
	if (p_generation == 0) {
		return;
	}
	const uint32_t candidate = _bump_generation(p_generation);
	if (candidate > r_next_generation) {
		r_next_generation = candidate;
	}
}

uint32_t LevelMeshData::_claim_vertex_generation() {
	const uint32_t generation = next_vertex_generation;
	next_vertex_generation = _bump_generation(next_vertex_generation);
	return generation;
}

uint32_t LevelMeshData::_claim_edge_generation() {
	const uint32_t generation = next_edge_generation;
	next_edge_generation = _bump_generation(next_edge_generation);
	return generation;
}

uint32_t LevelMeshData::_claim_face_generation() {
	const uint32_t generation = next_face_generation;
	next_face_generation = _bump_generation(next_face_generation);
	return generation;
}

void LevelMeshData::_ensure_generation_columns() {
	while (vertex_generations.size() < vertex_alive.size()) {
		vertex_generations.push_back((int32_t)_claim_vertex_generation());
	}
	while (edge_generations.size() < edge_alive.size()) {
		edge_generations.push_back((int32_t)_claim_edge_generation());
	}
	while (face_generations.size() < face_alive.size()) {
		face_generations.push_back((int32_t)_claim_face_generation());
	}
}

int LevelMeshData::_count_alive(const PackedByteArray &p_alive) {
	int count = 0;
	for (int i = 0; i < p_alive.size(); i++) {
		if (p_alive[i] != 0) {
			count++;
		}
	}
	return count;
}

void LevelMeshData::_copy_from(const LevelMeshData &p_other, bool p_emit_changed) {
	vertex_positions = p_other.vertex_positions;
	vertex_alive = p_other.vertex_alive;
	vertex_generations = p_other.vertex_generations;
	free_vertex_ids = p_other.free_vertex_ids;

	edge_vertices = p_other.edge_vertices;
	edge_alive = p_other.edge_alive;
	edge_generations = p_other.edge_generations;
	free_edge_ids = p_other.free_edge_ids;

	face_loop_starts = p_other.face_loop_starts;
	face_loop_counts = p_other.face_loop_counts;
	face_material_indices = p_other.face_material_indices;
	face_uv_modes = p_other.face_uv_modes;
	face_uv_origins = p_other.face_uv_origins;
	face_uv_tangents = p_other.face_uv_tangents;
	face_uv_transforms = p_other.face_uv_transforms;
	face_polygroup_ids = p_other.face_polygroup_ids;
	face_flags = p_other.face_flags;
	face_alive = p_other.face_alive;
	face_generations = p_other.face_generations;
	free_face_ids = p_other.free_face_ids;

	loop_vertex_indices = p_other.loop_vertex_indices;
	loop_uv0 = p_other.loop_uv0;
	loop_colors = p_other.loop_colors;
	loop_normals = p_other.loop_normals;
	loop_alive = p_other.loop_alive;
	free_loop_ids = p_other.free_loop_ids;

	// Snapshot restore must never move allocation epochs backwards. This is
	// what prevents a branch made after undo from aliasing an old handle.
	if (p_other.next_vertex_generation > next_vertex_generation) {
		next_vertex_generation = p_other.next_vertex_generation;
	}
	if (p_other.next_edge_generation > next_edge_generation) {
		next_edge_generation = p_other.next_edge_generation;
	}
	if (p_other.next_face_generation > next_face_generation) {
		next_face_generation = p_other.next_face_generation;
	}
	for (int i = 0; i < vertex_generations.size(); i++) {
		_advance_generation_counter(_generation_at(vertex_generations, i), next_vertex_generation);
	}
	for (int i = 0; i < edge_generations.size(); i++) {
		_advance_generation_counter(_generation_at(edge_generations, i), next_edge_generation);
	}
	for (int i = 0; i < face_generations.size(); i++) {
		_advance_generation_counter(_generation_at(face_generations, i), next_face_generation);
	}
	_ensure_generation_columns();

	if (p_emit_changed) {
		emit_changed();
	}
}

Transform2D LevelMeshData::_read_uv_transform(const LevelMeshData &p_data, int p_face_id) {
	const int offset = p_face_id * 6;
	if (p_face_id < 0 || offset + 5 >= p_data.face_uv_transforms.size()) {
		return Transform2D();
	}
	return Transform2D(
			p_data.face_uv_transforms[offset + 0], p_data.face_uv_transforms[offset + 1],
			p_data.face_uv_transforms[offset + 2], p_data.face_uv_transforms[offset + 3],
			p_data.face_uv_transforms[offset + 4], p_data.face_uv_transforms[offset + 5]);
}

void LevelMeshData::_write_uv_transform(LevelMeshData &r_data, int p_face_id, const Transform2D &p_transform) {
	const int offset = p_face_id * 6;
	if (p_face_id < 0 || offset + 5 >= r_data.face_uv_transforms.size()) {
		return;
	}
	r_data.face_uv_transforms.set(offset + 0, (float)p_transform[0].x);
	r_data.face_uv_transforms.set(offset + 1, (float)p_transform[0].y);
	r_data.face_uv_transforms.set(offset + 2, (float)p_transform[1].x);
	r_data.face_uv_transforms.set(offset + 3, (float)p_transform[1].y);
	r_data.face_uv_transforms.set(offset + 4, (float)p_transform[2].x);
	r_data.face_uv_transforms.set(offset + 5, (float)p_transform[2].y);
}

Transform2D LevelMeshData::get_face_uv_transform(int p_face_id) const {
	const int offset = p_face_id * 6;
	ERR_FAIL_COND_V(p_face_id < 0 || offset + 5 >= face_uv_transforms.size(), Transform2D());
	return _read_uv_transform(*this, p_face_id);
}

void LevelMeshData::set_face_uv_transform(int p_face_id, const Transform2D &p_transform) {
	const int offset = p_face_id * 6;
	ERR_FAIL_COND(p_face_id < 0 || offset + 5 >= face_uv_transforms.size());
	_write_uv_transform(*this, p_face_id, p_transform);
	emit_changed();
}

bool LevelMeshData::face_is_bakeable(int p_face_id) const {
	if (p_face_id < 0 || p_face_id >= face_alive.size() || face_alive[p_face_id] == 0 ||
			p_face_id >= face_loop_starts.size() || p_face_id >= face_loop_counts.size() ||
			p_face_id >= face_material_indices.size()) {
		return false;
	}
	const int loop_start = face_loop_starts[p_face_id];
	const int loop_count = face_loop_counts[p_face_id];
	if (loop_count < 3 || loop_start < 0 || loop_start > loop_vertex_indices.size() - loop_count ||
			loop_start > loop_alive.size() - loop_count || loop_start > loop_uv0.size() - loop_count ||
			loop_start > loop_colors.size() - loop_count || loop_start > loop_normals.size() - loop_count) {
		return false;
	}
	for (int corner = 0; corner < loop_count; corner++) {
		const int loop_id = loop_start + corner;
		const int vertex_id = loop_vertex_indices[loop_id];
		if (loop_alive[loop_id] == 0 || vertex_id < 0 || vertex_id >= vertex_positions.size() ||
				vertex_id >= vertex_alive.size() || vertex_alive[vertex_id] == 0) {
			return false;
		}
	}
	return true;
}

int LevelMeshData::vertex_count() const {
	return _count_alive(vertex_alive);
}

int LevelMeshData::edge_count() const {
	return _count_alive(edge_alive);
}

int LevelMeshData::face_count() const {
	return _count_alive(face_alive);
}

int LevelMeshData::loop_count() const {
	return _count_alive(loop_alive);
}

bool LevelMeshData::free_vertex_slot(int p_vertex_id) {
	_ensure_generation_columns();
	if (p_vertex_id < 0 || p_vertex_id >= vertex_alive.size() || p_vertex_id >= vertex_generations.size() || vertex_alive[p_vertex_id] == 0) {
		return false;
	}
	vertex_alive.set(p_vertex_id, 0);
	const uint32_t generation = _bump_generation(_generation_at(vertex_generations, p_vertex_id));
	vertex_generations.set(p_vertex_id, (int32_t)generation);
	_advance_generation_counter(generation, next_vertex_generation);
	_append_free_id(free_vertex_ids, p_vertex_id);
	emit_changed();
	return true;
}

bool LevelMeshData::free_edge_slot(int p_edge_id) {
	_ensure_generation_columns();
	if (p_edge_id < 0 || p_edge_id >= edge_alive.size() || p_edge_id >= edge_generations.size() ||
			p_edge_id > (edge_vertices.size() - 2) / 2 || edge_alive[p_edge_id] == 0) {
		return false;
	}
	edge_alive.set(p_edge_id, 0);
	const uint32_t generation = _bump_generation(_generation_at(edge_generations, p_edge_id));
	edge_generations.set(p_edge_id, (int32_t)generation);
	_advance_generation_counter(generation, next_edge_generation);
	_append_free_id(free_edge_ids, p_edge_id);
	emit_changed();
	return true;
}

bool LevelMeshData::free_face_slot(int p_face_id) {
	_ensure_generation_columns();
	if (p_face_id < 0 || p_face_id >= face_alive.size() || p_face_id >= face_generations.size() || p_face_id >= face_loop_starts.size() || p_face_id >= face_loop_counts.size() || face_alive[p_face_id] == 0) {
		return false;
	}
	face_alive.set(p_face_id, 0);
	const uint32_t generation = _bump_generation(_generation_at(face_generations, p_face_id));
	face_generations.set(p_face_id, (int32_t)generation);
	_advance_generation_counter(generation, next_face_generation);
	_append_free_id(free_face_ids, p_face_id);
	emit_changed();
	return true;
}

void LevelMeshData::clear() {
	vertex_positions.clear();
	vertex_alive.clear();
	vertex_generations.clear();
	free_vertex_ids.clear();

	edge_vertices.clear();
	edge_alive.clear();
	edge_generations.clear();
	free_edge_ids.clear();

	face_loop_starts.clear();
	face_loop_counts.clear();
	face_material_indices.clear();
	face_uv_modes.clear();
	face_uv_origins.clear();
	face_uv_tangents.clear();
	face_uv_transforms.clear();
	face_polygroup_ids.clear();
	face_flags.clear();
	face_alive.clear();
	face_generations.clear();
	free_face_ids.clear();

	loop_vertex_indices.clear();
	loop_uv0.clear();
	loop_colors.clear();
	loop_normals.clear();
	loop_alive.clear();
	free_loop_ids.clear();

	emit_changed();
}

Ref<LevelMeshData> LevelMeshData::duplicate_data() const {
	Ref<LevelMeshData> copy;
	copy.instantiate();
	copy->_copy_from(*this, false);
	return copy;
}

void LevelMeshData::copy_from(const Ref<LevelMeshData> &p_other) {
	ERR_FAIL_COND(p_other.is_null());
	_copy_from(**p_other, true);
}

void LevelMeshData::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_vertex_positions", "values"), &LevelMeshData::set_vertex_positions);
	ClassDB::bind_method(D_METHOD("get_vertex_positions"), &LevelMeshData::get_vertex_positions);
	ClassDB::bind_method(D_METHOD("set_vertex_alive", "values"), &LevelMeshData::set_vertex_alive);
	ClassDB::bind_method(D_METHOD("get_vertex_alive"), &LevelMeshData::get_vertex_alive);
	ClassDB::bind_method(D_METHOD("set_vertex_generations", "values"), &LevelMeshData::set_vertex_generations);
	ClassDB::bind_method(D_METHOD("get_vertex_generations"), &LevelMeshData::get_vertex_generations);
	ClassDB::bind_method(D_METHOD("set_free_vertex_ids", "values"), &LevelMeshData::set_free_vertex_ids);
	ClassDB::bind_method(D_METHOD("get_free_vertex_ids"), &LevelMeshData::get_free_vertex_ids);

	ClassDB::bind_method(D_METHOD("set_edge_vertices", "values"), &LevelMeshData::set_edge_vertices);
	ClassDB::bind_method(D_METHOD("get_edge_vertices"), &LevelMeshData::get_edge_vertices);
	ClassDB::bind_method(D_METHOD("set_edge_alive", "values"), &LevelMeshData::set_edge_alive);
	ClassDB::bind_method(D_METHOD("get_edge_alive"), &LevelMeshData::get_edge_alive);
	ClassDB::bind_method(D_METHOD("set_edge_generations", "values"), &LevelMeshData::set_edge_generations);
	ClassDB::bind_method(D_METHOD("get_edge_generations"), &LevelMeshData::get_edge_generations);
	ClassDB::bind_method(D_METHOD("set_free_edge_ids", "values"), &LevelMeshData::set_free_edge_ids);
	ClassDB::bind_method(D_METHOD("get_free_edge_ids"), &LevelMeshData::get_free_edge_ids);

	ClassDB::bind_method(D_METHOD("set_face_loop_starts", "values"), &LevelMeshData::set_face_loop_starts);
	ClassDB::bind_method(D_METHOD("get_face_loop_starts"), &LevelMeshData::get_face_loop_starts);
	ClassDB::bind_method(D_METHOD("set_face_loop_counts", "values"), &LevelMeshData::set_face_loop_counts);
	ClassDB::bind_method(D_METHOD("get_face_loop_counts"), &LevelMeshData::get_face_loop_counts);
	ClassDB::bind_method(D_METHOD("set_face_material_indices", "values"), &LevelMeshData::set_face_material_indices);
	ClassDB::bind_method(D_METHOD("get_face_material_indices"), &LevelMeshData::get_face_material_indices);
	ClassDB::bind_method(D_METHOD("set_face_uv_modes", "values"), &LevelMeshData::set_face_uv_modes);
	ClassDB::bind_method(D_METHOD("get_face_uv_modes"), &LevelMeshData::get_face_uv_modes);
	ClassDB::bind_method(D_METHOD("set_face_uv_origins", "values"), &LevelMeshData::set_face_uv_origins);
	ClassDB::bind_method(D_METHOD("get_face_uv_origins"), &LevelMeshData::get_face_uv_origins);
	ClassDB::bind_method(D_METHOD("set_face_uv_tangents", "values"), &LevelMeshData::set_face_uv_tangents);
	ClassDB::bind_method(D_METHOD("get_face_uv_tangents"), &LevelMeshData::get_face_uv_tangents);
	ClassDB::bind_method(D_METHOD("set_face_uv_transforms", "values"), &LevelMeshData::set_face_uv_transforms);
	ClassDB::bind_method(D_METHOD("get_face_uv_transforms"), &LevelMeshData::get_face_uv_transforms);
	ClassDB::bind_method(D_METHOD("set_face_polygroup_ids", "values"), &LevelMeshData::set_face_polygroup_ids);
	ClassDB::bind_method(D_METHOD("get_face_polygroup_ids"), &LevelMeshData::get_face_polygroup_ids);
	ClassDB::bind_method(D_METHOD("set_face_flags", "values"), &LevelMeshData::set_face_flags);
	ClassDB::bind_method(D_METHOD("get_face_flags"), &LevelMeshData::get_face_flags);
	ClassDB::bind_method(D_METHOD("set_face_alive", "values"), &LevelMeshData::set_face_alive);
	ClassDB::bind_method(D_METHOD("get_face_alive"), &LevelMeshData::get_face_alive);
	ClassDB::bind_method(D_METHOD("set_face_generations", "values"), &LevelMeshData::set_face_generations);
	ClassDB::bind_method(D_METHOD("get_face_generations"), &LevelMeshData::get_face_generations);
	ClassDB::bind_method(D_METHOD("set_free_face_ids", "values"), &LevelMeshData::set_free_face_ids);
	ClassDB::bind_method(D_METHOD("get_free_face_ids"), &LevelMeshData::get_free_face_ids);

	ClassDB::bind_method(D_METHOD("set_loop_vertex_indices", "values"), &LevelMeshData::set_loop_vertex_indices);
	ClassDB::bind_method(D_METHOD("get_loop_vertex_indices"), &LevelMeshData::get_loop_vertex_indices);
	ClassDB::bind_method(D_METHOD("set_loop_uv0", "values"), &LevelMeshData::set_loop_uv0);
	ClassDB::bind_method(D_METHOD("get_loop_uv0"), &LevelMeshData::get_loop_uv0);
	ClassDB::bind_method(D_METHOD("set_loop_colors", "values"), &LevelMeshData::set_loop_colors);
	ClassDB::bind_method(D_METHOD("get_loop_colors"), &LevelMeshData::get_loop_colors);
	ClassDB::bind_method(D_METHOD("set_loop_normals", "values"), &LevelMeshData::set_loop_normals);
	ClassDB::bind_method(D_METHOD("get_loop_normals"), &LevelMeshData::get_loop_normals);
	ClassDB::bind_method(D_METHOD("set_loop_alive", "values"), &LevelMeshData::set_loop_alive);
	ClassDB::bind_method(D_METHOD("get_loop_alive"), &LevelMeshData::get_loop_alive);
	ClassDB::bind_method(D_METHOD("set_free_loop_ids", "values"), &LevelMeshData::set_free_loop_ids);
	ClassDB::bind_method(D_METHOD("get_free_loop_ids"), &LevelMeshData::get_free_loop_ids);

	ClassDB::bind_method(D_METHOD("get_face_uv_transform", "face_id"), &LevelMeshData::get_face_uv_transform);
	ClassDB::bind_method(D_METHOD("set_face_uv_transform", "face_id", "transform"), &LevelMeshData::set_face_uv_transform);
	ClassDB::bind_method(D_METHOD("face_is_bakeable", "face_id"), &LevelMeshData::face_is_bakeable);
	ClassDB::bind_method(D_METHOD("vertex_count"), &LevelMeshData::vertex_count);
	ClassDB::bind_method(D_METHOD("edge_count"), &LevelMeshData::edge_count);
	ClassDB::bind_method(D_METHOD("face_count"), &LevelMeshData::face_count);
	ClassDB::bind_method(D_METHOD("loop_count"), &LevelMeshData::loop_count);
	ClassDB::bind_method(D_METHOD("free_vertex_slot", "vertex_id"), &LevelMeshData::free_vertex_slot);
	ClassDB::bind_method(D_METHOD("free_edge_slot", "edge_id"), &LevelMeshData::free_edge_slot);
	ClassDB::bind_method(D_METHOD("free_face_slot", "face_id"), &LevelMeshData::free_face_slot);
	ClassDB::bind_method(D_METHOD("clear"), &LevelMeshData::clear);
	ClassDB::bind_method(D_METHOD("duplicate_data"), &LevelMeshData::duplicate_data);
	ClassDB::bind_method(D_METHOD("copy_from", "other"), &LevelMeshData::copy_from);

	ADD_SIGNAL(MethodInfo("mesh_diff_applied",
			PropertyInfo(Variant::OBJECT, "diff", PROPERTY_HINT_RESOURCE_TYPE, "LevelMeshDiff"),
			PropertyInfo(Variant::BOOL, "reverted")));
	ADD_SIGNAL(MethodInfo("mesh_preview_changed"));

	ADD_GROUP("Vertices", "vertex_");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_VECTOR3_ARRAY, "vertex_positions"), "set_vertex_positions", "get_vertex_positions");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_BYTE_ARRAY, "vertex_alive"), "set_vertex_alive", "get_vertex_alive");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_INT32_ARRAY, "vertex_generations"), "set_vertex_generations", "get_vertex_generations");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_INT32_ARRAY, "vertex_free_ids"), "set_free_vertex_ids", "get_free_vertex_ids");

	ADD_GROUP("Edges", "edge_");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_INT32_ARRAY, "edge_vertices"), "set_edge_vertices", "get_edge_vertices");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_BYTE_ARRAY, "edge_alive"), "set_edge_alive", "get_edge_alive");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_INT32_ARRAY, "edge_generations"), "set_edge_generations", "get_edge_generations");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_INT32_ARRAY, "edge_free_ids"), "set_free_edge_ids", "get_free_edge_ids");

	ADD_GROUP("Faces", "face_");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_INT32_ARRAY, "face_loop_starts"), "set_face_loop_starts", "get_face_loop_starts");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_INT32_ARRAY, "face_loop_counts"), "set_face_loop_counts", "get_face_loop_counts");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_INT32_ARRAY, "face_material_indices"), "set_face_material_indices", "get_face_material_indices");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_INT32_ARRAY, "face_uv_modes"), "set_face_uv_modes", "get_face_uv_modes");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_VECTOR3_ARRAY, "face_uv_origins"), "set_face_uv_origins", "get_face_uv_origins");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_VECTOR3_ARRAY, "face_uv_tangents"), "set_face_uv_tangents", "get_face_uv_tangents");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_FLOAT32_ARRAY, "face_uv_transforms"), "set_face_uv_transforms", "get_face_uv_transforms");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_INT32_ARRAY, "face_polygroup_ids"), "set_face_polygroup_ids", "get_face_polygroup_ids");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_INT32_ARRAY, "face_flags"), "set_face_flags", "get_face_flags");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_BYTE_ARRAY, "face_alive"), "set_face_alive", "get_face_alive");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_INT32_ARRAY, "face_generations"), "set_face_generations", "get_face_generations");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_INT32_ARRAY, "face_free_ids"), "set_free_face_ids", "get_free_face_ids");

	ADD_GROUP("Loops", "loop_");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_INT32_ARRAY, "loop_vertex_indices"), "set_loop_vertex_indices", "get_loop_vertex_indices");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_VECTOR2_ARRAY, "loop_uv0"), "set_loop_uv0", "get_loop_uv0");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_COLOR_ARRAY, "loop_colors"), "set_loop_colors", "get_loop_colors");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_VECTOR3_ARRAY, "loop_normals"), "set_loop_normals", "get_loop_normals");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_BYTE_ARRAY, "loop_alive"), "set_loop_alive", "get_loop_alive");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_INT32_ARRAY, "loop_free_ids"), "set_free_loop_ids", "get_free_loop_ids");

	BIND_ENUM_CONSTANT(UV_MODE_PROJECTED);
	BIND_ENUM_CONSTANT(UV_MODE_EXPLICIT);
	BIND_BITFIELD_FLAG(FACE_FLAG_NONE);
	BIND_BITFIELD_FLAG(FACE_FLAG_SMOOTH);
	BIND_BITFIELD_FLAG(FACE_FLAG_TEXTURE_LOCK);
}
