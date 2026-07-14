/**************************************************************************/
/*  level_mesh_diff.cpp                                                   */
/**************************************************************************/

#include "level_mesh_diff.h"

#include "level_mesh_data.h"

#include "core/object/class_db.h"

namespace {

bool slot_is_alive(const PackedByteArray &p_alive, int p_slot) {
	return p_slot >= 0 && p_slot < p_alive.size() && p_alive[p_slot] != 0;
}

uint32_t slot_generation(const PackedInt32Array &p_generations, int p_slot) {
	return p_slot >= 0 && p_slot < p_generations.size() ? (uint32_t)p_generations[p_slot] : 0;
}

} // namespace

void LevelMeshDiff::_compute_removed_handles(
		const PackedByteArray &p_from_alive,
		const PackedInt32Array &p_from_generations,
		const PackedByteArray &p_to_alive,
		const PackedInt32Array &p_to_generations,
		PackedInt64Array &r_removed) {
	r_removed.clear();
	for (int slot = 0; slot < p_from_alive.size(); slot++) {
		if (!slot_is_alive(p_from_alive, slot)) {
			continue;
		}
		const uint32_t from_generation = slot_generation(p_from_generations, slot);
		if (!slot_is_alive(p_to_alive, slot) || slot_generation(p_to_generations, slot) != from_generation) {
			r_removed.push_back(LevelMeshData::_pack_handle(slot, from_generation));
		}
	}
}

void LevelMeshDiff::_set_states(const Ref<LevelMeshData> &p_before, const Ref<LevelMeshData> &p_after, bool p_empty) {
	before_data = p_before;
	after_data = p_after;
	empty = p_empty;
	removed_vertex_handles.clear();
	removed_edge_handles.clear();
	removed_face_handles.clear();
	revert_removed_vertex_handles.clear();
	revert_removed_edge_handles.clear();
	revert_removed_face_handles.clear();
	topology_changed = false;
	geometry_changed = false;

	if (empty || before_data.is_null() || after_data.is_null()) {
		return;
	}

	_compute_removed_handles(before_data->vertex_alive, before_data->vertex_generations,
			after_data->vertex_alive, after_data->vertex_generations, removed_vertex_handles);
	_compute_removed_handles(before_data->edge_alive, before_data->edge_generations,
			after_data->edge_alive, after_data->edge_generations, removed_edge_handles);
	_compute_removed_handles(before_data->face_alive, before_data->face_generations,
			after_data->face_alive, after_data->face_generations, removed_face_handles);
	_compute_removed_handles(after_data->vertex_alive, after_data->vertex_generations,
			before_data->vertex_alive, before_data->vertex_generations, revert_removed_vertex_handles);
	_compute_removed_handles(after_data->edge_alive, after_data->edge_generations,
			before_data->edge_alive, before_data->edge_generations, revert_removed_edge_handles);
	_compute_removed_handles(after_data->face_alive, after_data->face_generations,
			before_data->face_alive, before_data->face_generations, revert_removed_face_handles);

	topology_changed =
			before_data->vertex_alive != after_data->vertex_alive ||
			before_data->vertex_generations != after_data->vertex_generations ||
			before_data->free_vertex_ids != after_data->free_vertex_ids ||
			before_data->edge_vertices != after_data->edge_vertices ||
			before_data->edge_alive != after_data->edge_alive ||
			before_data->edge_generations != after_data->edge_generations ||
			before_data->free_edge_ids != after_data->free_edge_ids ||
			before_data->face_loop_starts != after_data->face_loop_starts ||
			before_data->face_loop_counts != after_data->face_loop_counts ||
			before_data->face_alive != after_data->face_alive ||
			before_data->face_generations != after_data->face_generations ||
			before_data->free_face_ids != after_data->free_face_ids ||
			before_data->loop_vertex_indices != after_data->loop_vertex_indices ||
			before_data->loop_alive != after_data->loop_alive ||
			before_data->free_loop_ids != after_data->free_loop_ids;
	geometry_changed = topology_changed || before_data->vertex_positions != after_data->vertex_positions;
}

Ref<LevelMeshData> LevelMeshDiff::get_before_data() const {
	return before_data;
}

Ref<LevelMeshData> LevelMeshDiff::get_after_data() const {
	return after_data;
}

PackedInt64Array LevelMeshDiff::get_removed_vertex_handles() const {
	return removed_vertex_handles;
}

PackedInt64Array LevelMeshDiff::get_removed_edge_handles() const {
	return removed_edge_handles;
}

PackedInt64Array LevelMeshDiff::get_removed_face_handles() const {
	return removed_face_handles;
}

PackedInt64Array LevelMeshDiff::get_revert_removed_vertex_handles() const {
	return revert_removed_vertex_handles;
}

PackedInt64Array LevelMeshDiff::get_revert_removed_edge_handles() const {
	return revert_removed_edge_handles;
}

PackedInt64Array LevelMeshDiff::get_revert_removed_face_handles() const {
	return revert_removed_face_handles;
}

// The current full-snapshot representation identifies created elements as
// elements removed by reverting. This accessor is the stable contract even if
// the diff representation narrows to explicit changed ranges later.
PackedInt64Array LevelMeshDiff::get_created_vertex_handles() const {
	return revert_removed_vertex_handles;
}

PackedInt64Array LevelMeshDiff::get_created_edge_handles() const {
	return revert_removed_edge_handles;
}

PackedInt64Array LevelMeshDiff::get_created_face_handles() const {
	return revert_removed_face_handles;
}

bool LevelMeshDiff::touches_topology() const {
	return topology_changed;
}

bool LevelMeshDiff::touches_geometry() const {
	return geometry_changed;
}

bool LevelMeshDiff::is_empty() const {
	return empty;
}

void LevelMeshDiff::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_before_data"), &LevelMeshDiff::get_before_data);
	ClassDB::bind_method(D_METHOD("get_after_data"), &LevelMeshDiff::get_after_data);
	ClassDB::bind_method(D_METHOD("get_removed_vertex_handles"), &LevelMeshDiff::get_removed_vertex_handles);
	ClassDB::bind_method(D_METHOD("get_removed_edge_handles"), &LevelMeshDiff::get_removed_edge_handles);
	ClassDB::bind_method(D_METHOD("get_removed_face_handles"), &LevelMeshDiff::get_removed_face_handles);
	ClassDB::bind_method(D_METHOD("get_revert_removed_vertex_handles"), &LevelMeshDiff::get_revert_removed_vertex_handles);
	ClassDB::bind_method(D_METHOD("get_revert_removed_edge_handles"), &LevelMeshDiff::get_revert_removed_edge_handles);
	ClassDB::bind_method(D_METHOD("get_revert_removed_face_handles"), &LevelMeshDiff::get_revert_removed_face_handles);
	ClassDB::bind_method(D_METHOD("get_created_vertex_handles"), &LevelMeshDiff::get_created_vertex_handles);
	ClassDB::bind_method(D_METHOD("get_created_edge_handles"), &LevelMeshDiff::get_created_edge_handles);
	ClassDB::bind_method(D_METHOD("get_created_face_handles"), &LevelMeshDiff::get_created_face_handles);
	ClassDB::bind_method(D_METHOD("touches_topology"), &LevelMeshDiff::touches_topology);
	ClassDB::bind_method(D_METHOD("touches_geometry"), &LevelMeshDiff::touches_geometry);
	ClassDB::bind_method(D_METHOD("is_empty"), &LevelMeshDiff::is_empty);

	ADD_PROPERTY(PropertyInfo(Variant::PACKED_INT64_ARRAY, "removed_vertex_handles", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_READ_ONLY), "", "get_removed_vertex_handles");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_INT64_ARRAY, "removed_edge_handles", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_READ_ONLY), "", "get_removed_edge_handles");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_INT64_ARRAY, "removed_face_handles", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_READ_ONLY), "", "get_removed_face_handles");
}
