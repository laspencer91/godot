/**************************************************************************/
/*  level_mesh_hotspot.cpp                                                */
/**************************************************************************/
/*  G-Level LE3: transactional application of pure hotspot-fit results.   */
/**************************************************************************/

#include "level_mesh.h"

#include "level_mesh_data.h"
#include "level_mesh_diff.h"

#include "core/templates/hash_set.h"

namespace {

bool hotspot_result_is_valid(const Dictionary &p_result, const Ref<LevelMeshData> &p_data) {
	const int face_id = p_result.get("face_index", p_result.get("face_id", -1));
	if (face_id < 0 || face_id >= p_data->get_face_alive().size() ||
			p_data->get_face_alive()[face_id] == 0) {
		return false;
	}
	const String patch_name = p_result.get("patch_name", String());
	const int uv_mode = p_result.get("uv_mode", -1);
	if (patch_name.is_empty() || (uv_mode != LevelMeshData::UV_MODE_PROJECTED &&
			uv_mode != LevelMeshData::UV_MODE_EXPLICIT)) {
		return false;
	}
	const PackedInt32Array face_starts = p_data->get_face_loop_starts();
	const PackedInt32Array face_counts = p_data->get_face_loop_counts();
	if (face_id >= face_starts.size() || face_id >= face_counts.size()) {
		return false;
	}
	const int loop_start = face_starts[face_id];
	const int loop_count = face_counts[face_id];
	const PackedInt32Array loop_ids = p_result.get("loop_ids", PackedInt32Array());
	const PackedVector2Array loop_uvs = p_result.get("loop_uvs", PackedVector2Array());
	if (loop_start < 0 || loop_count < 3 || loop_ids.size() != loop_count || loop_uvs.size() != loop_count) {
		return false;
	}
	for (int corner = 0; corner < loop_count; corner++) {
		if (loop_ids[corner] != loop_start + corner || !loop_uvs[corner].is_finite()) {
			return false;
		}
	}
	if (uv_mode == LevelMeshData::UV_MODE_PROJECTED) {
		const Vector3 origin = p_result.get("uv_origin", Vector3());
		const Vector3 tangent = p_result.get("uv_tangent", Vector3());
		const Transform2D transform = p_result.get("uv_transform", Transform2D());
		return origin.is_finite() && tangent.is_finite() && tangent.length_squared() > CMP_EPSILON2 &&
				transform.is_finite();
	}
	return true;
}

} // namespace

Ref<LevelMeshDiff> LevelMesh::apply_hotspot_fit(const Array &p_face_results) {
	if (transaction_active || transform_preview_active || p_face_results.is_empty() || data.is_null()) {
		return Ref<LevelMeshDiff>();
	}
	data->_ensure_generation_columns();
	HashSet<int> seen_faces;
	Array applied_diagnostics;
	for (int i = 0; i < p_face_results.size(); i++) {
		if (p_face_results[i].get_type() != Variant::DICTIONARY) {
			return Ref<LevelMeshDiff>();
		}
		const Dictionary result = p_face_results[i];
		const int face_id = result.get("face_index", result.get("face_id", -1));
		if (seen_faces.has(face_id) || !hotspot_result_is_valid(result, data)) {
			return Ref<LevelMeshDiff>();
		}
		seen_faces.insert(face_id);
		const Variant diagnostic_variant = result.get("fit_diagnostic", Variant());
		if (diagnostic_variant.get_type() == Variant::DICTIONARY) {
			const Dictionary diagnostic = diagnostic_variant;
			if (!applied_diagnostics.has(diagnostic)) {
				applied_diagnostics.push_back(diagnostic);
			}
		}
	}

	begin_transaction();
	for (int i = 0; i < p_face_results.size(); i++) {
		const Dictionary result = p_face_results[i];
		const int face_id = result.get("face_index", result.get("face_id", -1));
		const int uv_mode = result.get("uv_mode", LevelMeshData::UV_MODE_EXPLICIT);
		const int loop_start = data->face_loop_starts[face_id];
		const int loop_count = data->face_loop_counts[face_id];
		data->face_hotspot_patch_names.set(face_id, String(result.get("patch_name", String())));
		data->face_uv_modes.set(face_id, uv_mode);
		if (uv_mode == LevelMeshData::UV_MODE_PROJECTED) {
			data->face_uv_origins.set(face_id, result.get("uv_origin", Vector3()));
			data->face_uv_tangents.set(face_id, result.get("uv_tangent", Vector3()));
			LevelMeshData::_write_uv_transform(**data, face_id, result.get("uv_transform", Transform2D()));
		} else {
			data->face_uv_origins.set(face_id, Vector3());
			data->face_uv_tangents.set(face_id, Vector3());
			LevelMeshData::_write_uv_transform(**data, face_id, Transform2D());
			const PackedVector2Array loop_uvs = result.get("loop_uvs", PackedVector2Array());
			for (int corner = 0; corner < loop_count; corner++) {
				data->loop_uv0.set(loop_start + corner, loop_uvs[corner]);
			}
		}
		if (!_reconcile_face_uv(face_id)) {
			transaction_changed = true;
			rollback();
			return Ref<LevelMeshDiff>();
		}
	}
	transaction_changed = true;
	geometry_change_notification = true;
	data->emit_changed();
	geometry_change_notification = false;
	Ref<LevelMeshDiff> diff = commit();
	if (diff.is_valid()) {
		last_hotspot_fit_diagnostics = applied_diagnostics;
	}
	return diff;
}
