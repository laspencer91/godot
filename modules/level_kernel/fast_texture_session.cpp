/**************************************************************************/
/*  fast_texture_session.cpp                                              */
/**************************************************************************/

#include "fast_texture_session.h"

#include "level_mesh.h"
#include "level_mesh_data.h"
#include "level_mesh_diff.h"

#include "core/object/class_db.h"

void FastTextureSession::_set_error(const String &p_error) {
	last_error = p_error;
	emit_signal(SNAME("changed"));
}

String FastTextureSession::_unwrap_error_text(int p_error) {
	switch (LevelMesh::UnwrapError(p_error)) {
		case LevelMesh::UNWRAP_ERROR_NONE:
			return String();
		case LevelMesh::UNWRAP_ERROR_BUSY:
			return "The mesh is busy with another operation.";
		case LevelMesh::UNWRAP_ERROR_EMPTY_SELECTION:
			return "Fast Texture needs at least one face.";
		case LevelMesh::UNWRAP_ERROR_INVALID_FACE:
			return "The selection contains an invalid face.";
		case LevelMesh::UNWRAP_ERROR_INVALID_TOPOLOGY:
			return "The selected face topology is invalid.";
		case LevelMesh::UNWRAP_ERROR_NON_MANIFOLD_EDGE:
			return "The selection crosses a non-manifold edge.";
		case LevelMesh::UNWRAP_ERROR_INVALID_SEED:
			return "Follow Quads needs a quad seed face.";
		case LevelMesh::UNWRAP_ERROR_INVALID_SPACING_MODE:
			return "The Follow Quads spacing mode is invalid.";
		case LevelMesh::UNWRAP_ERROR_INVALID_THRESHOLD:
			return "The conforming distortion threshold is invalid.";
		case LevelMesh::UNWRAP_ERROR_UNFOLD_FAILED:
			return "The selected faces could not be unfolded.";
	}
	return "The unwrap operation was rejected.";
}

bool FastTextureSession::_source_matches_snapshot() const {
	if (source_mesh.is_null() || original_data.is_null()) {
		return false;
	}
	const Ref<LevelMeshData> current = source_mesh->get_data();
	if (current.is_null() ||
			current->get_vertex_positions() != original_data->get_vertex_positions() ||
			current->get_vertex_alive() != original_data->get_vertex_alive() ||
			current->get_vertex_generations() != original_data->get_vertex_generations() ||
			current->get_face_loop_starts() != original_data->get_face_loop_starts() ||
			current->get_face_loop_counts() != original_data->get_face_loop_counts() ||
			current->get_face_alive() != original_data->get_face_alive() ||
			current->get_face_generations() != original_data->get_face_generations() ||
			current->get_loop_vertex_indices() != original_data->get_loop_vertex_indices() ||
			current->get_loop_alive() != original_data->get_loop_alive()) {
		return false;
	}

	const PackedInt32Array loop_starts = original_data->get_face_loop_starts();
	const PackedInt32Array loop_counts = original_data->get_face_loop_counts();
	for (const int face_id : face_ids) {
		if (current->get_face_uv_mode(face_id) != original_data->get_face_uv_mode(face_id) ||
				current->get_face_uv_origin(face_id) != original_data->get_face_uv_origin(face_id) ||
				current->get_face_uv_tangent(face_id) != original_data->get_face_uv_tangent(face_id) ||
				current->get_face_uv_transform(face_id) != original_data->get_face_uv_transform(face_id)) {
			return false;
		}
		const int loop_start = loop_starts[face_id];
		const int loop_count = loop_counts[face_id];
		for (int corner = 0; corner < loop_count; corner++) {
			const int loop_id = loop_start + corner;
			if (current->get_loop_uv(loop_id) != original_data->get_loop_uv(loop_id)) {
				return false;
			}
		}
	}
	return true;
}

void FastTextureSession::set_mesh(const Ref<LevelMesh> &p_mesh) {
	ERR_FAIL_COND_MSG(active, "Cannot replace the Fast Texture mesh while a session is active.");
	source_mesh = p_mesh;
}

Ref<LevelMesh> FastTextureSession::get_mesh() const {
	return source_mesh;
}

bool FastTextureSession::open(const PackedInt32Array &p_face_ids) {
	if (active || source_mesh.is_null() || source_mesh->is_transaction_active() || p_face_ids.is_empty()) {
		_set_error(p_face_ids.is_empty() ? "Fast Texture needs at least one face." : "Fast Texture could not open on this mesh.");
		return false;
	}
	const Ref<LevelMeshData> source_data = source_mesh->get_data();
	if (source_data.is_null()) {
		_set_error("Fast Texture could not read the mesh data.");
		return false;
	}

	PackedInt32Array normalized;
	for (const int face_id : p_face_ids) {
		if (normalized.has(face_id)) {
			continue;
		}
		if (!source_data->face_is_bakeable(face_id)) {
			_set_error("Fast Texture received an invalid face selection.");
			return false;
		}
		normalized.push_back(face_id);
	}
	if (normalized.is_empty()) {
		_set_error("Fast Texture needs at least one face.");
		return false;
	}
	normalized.sort();

	original_data = source_data->duplicate_data();
	working_mesh.instantiate();
	working_mesh->set_data(original_data->duplicate_data());
	face_ids = normalized;
	nudge = Transform2D();
	mode = MODE_USE_EXISTING;
	spacing = SPACING_LENGTH;
	last_diff.unref();
	last_error.clear();
	active = true;
	emit_signal(SNAME("changed"));
	return true;
}

bool FastTextureSession::set_mode(int p_mode, int p_spacing) {
	if (!active || original_data.is_null() || p_mode < MODE_USE_EXISTING || p_mode > MODE_PLANAR ||
			p_spacing < SPACING_LENGTH || p_spacing > SPACING_LENGTH_AVERAGE) {
		_set_error("Fast Texture received an invalid mode or spacing value.");
		return false;
	}

	Ref<LevelMesh> candidate;
	candidate.instantiate();
	candidate->set_data(original_data->duplicate_data());
	Ref<LevelMeshDiff> mode_diff;
	switch (Mode(p_mode)) {
		case MODE_USE_EXISTING:
			break;
		case MODE_CONFORMING:
			mode_diff = candidate->unwrap_conforming(face_ids);
			break;
		case MODE_SQUARE:
			mode_diff = candidate->unwrap_square(face_ids);
			break;
		case MODE_FOLLOW_QUADS:
			mode_diff = candidate->unwrap_follow_quads(face_ids, p_spacing);
			break;
		case MODE_PLANAR:
			mode_diff = candidate->unwrap_planar(face_ids);
			break;
	}
	if (p_mode != MODE_USE_EXISTING && mode_diff.is_null()) {
		_set_error(_unwrap_error_text(candidate->get_last_unwrap_error()));
		return false;
	}

	working_mesh = candidate;
	mode = Mode(p_mode);
	spacing = SpacingMode(p_spacing);
	last_error.clear();
	emit_signal(SNAME("changed"));
	return true;
}

bool FastTextureSession::set_nudge(const Transform2D &p_nudge) {
	if (!active || !p_nudge.is_finite()) {
		_set_error("Fast Texture received a non-finite box transform.");
		return false;
	}
	if (nudge == p_nudge) {
		return true;
	}
	nudge = p_nudge;
	last_error.clear();
	emit_signal(SNAME("changed"));
	return true;
}

bool FastTextureSession::can_accept() const {
	return active && source_mesh.is_valid() && working_mesh.is_valid() &&
			!source_mesh->is_transaction_active() && _source_matches_snapshot();
}

PackedVector2Array FastTextureSession::get_mode_loop_uvs() const {
	const Ref<LevelMeshData> working_data = get_working_data();
	return working_data.is_valid() ? working_data->get_loop_uv0() : PackedVector2Array();
}

PackedVector2Array FastTextureSession::get_working_loop_uvs() const {
	const Ref<LevelMeshData> working_data = get_working_data();
	if (working_data.is_null()) {
		return PackedVector2Array();
	}
	PackedVector2Array result = working_data->get_loop_uv0();
	const PackedInt32Array loop_starts = working_data->get_face_loop_starts();
	const PackedInt32Array loop_counts = working_data->get_face_loop_counts();
	for (const int face_id : face_ids) {
		const int loop_start = loop_starts[face_id];
		const int loop_count = loop_counts[face_id];
		for (int corner = 0; corner < loop_count; corner++) {
			const int loop_id = loop_start + corner;
			result.set(loop_id, nudge.xform(result[loop_id]));
		}
	}
	return result;
}

Ref<LevelMeshData> FastTextureSession::get_working_data() const {
	return working_mesh.is_valid() ? working_mesh->get_data() : Ref<LevelMeshData>();
}

Ref<LevelMeshData> FastTextureSession::get_original_data() const {
	return original_data;
}

Ref<LevelMeshDiff> FastTextureSession::get_last_diff() const {
	return last_diff;
}

bool FastTextureSession::accept() {
	if (!active || source_mesh.is_null() || working_mesh.is_null() || source_mesh->is_transaction_active()) {
		_set_error("Fast Texture could not accept while the mesh is busy.");
		return false;
	}
	if (!_source_matches_snapshot()) {
		_set_error("The source mesh changed while Fast Texture was open.");
		return false;
	}

	const Ref<LevelMeshData> target = working_mesh->get_data();
	const Ref<LevelMeshData> current = source_mesh->get_data();
	const PackedVector2Array target_loop_uvs = get_working_loop_uvs();
	const PackedInt32Array loop_starts = target->get_face_loop_starts();
	const PackedInt32Array loop_counts = target->get_face_loop_counts();
	bool changed = false;
	for (const int face_id : face_ids) {
		const int target_mode = target->get_face_uv_mode(face_id);
		const Transform2D target_transform = target_mode == LevelMeshData::UV_MODE_PROJECTED ? nudge * target->get_face_uv_transform(face_id) : target->get_face_uv_transform(face_id);
		changed = changed || current->get_face_uv_mode(face_id) != target_mode ||
				current->get_face_uv_origin(face_id) != target->get_face_uv_origin(face_id) ||
				current->get_face_uv_tangent(face_id) != target->get_face_uv_tangent(face_id) ||
				current->get_face_uv_transform(face_id) != target_transform;
		const int loop_start = loop_starts[face_id];
		const int loop_count = loop_counts[face_id];
		for (int corner = 0; corner < loop_count; corner++) {
			const int loop_id = loop_start + corner;
			changed = changed || current->get_loop_uv(loop_id) != target_loop_uvs[loop_id];
		}
	}

	Ref<LevelMeshDiff> diff;
	if (changed) {
		source_mesh->begin_transaction();
		for (const int face_id : face_ids) {
			const int target_mode = target->get_face_uv_mode(face_id);
			current->set_face_uv_mode(face_id, target_mode);
			current->set_face_uv_origin(face_id, target->get_face_uv_origin(face_id));
			current->set_face_uv_tangent(face_id, target->get_face_uv_tangent(face_id));
			const Transform2D target_transform = target_mode == LevelMeshData::UV_MODE_PROJECTED ? nudge * target->get_face_uv_transform(face_id) : target->get_face_uv_transform(face_id);
			current->set_face_uv_transform(face_id, target_transform);
			if (target_mode == LevelMeshData::UV_MODE_EXPLICIT) {
				const int loop_start = loop_starts[face_id];
				const int loop_count = loop_counts[face_id];
				for (int corner = 0; corner < loop_count; corner++) {
					const int loop_id = loop_start + corner;
					current->set_loop_uv(loop_id, target_loop_uvs[loop_id]);
				}
			}
			if (!source_mesh->reconcile_face_uv(face_id)) {
				source_mesh->rollback();
				_set_error("Fast Texture could not reconcile the accepted UV state.");
				return false;
			}
		}
		diff = source_mesh->commit();
		if (diff.is_null()) {
			_set_error("Fast Texture produced no mesh diff for a changed result.");
			return false;
		}
	}

	last_diff = diff;
	working_mesh.unref();
	original_data.unref();
	face_ids.clear();
	last_error.clear();
	active = false;
	emit_signal(SNAME("changed"));
	return true;
}

void FastTextureSession::cancel() {
	if (!active) {
		return;
	}
	working_mesh.unref();
	original_data.unref();
	last_diff.unref();
	face_ids.clear();
	nudge = Transform2D();
	mode = MODE_USE_EXISTING;
	spacing = SPACING_LENGTH;
	last_error.clear();
	active = false;
	emit_signal(SNAME("changed"));
}

void FastTextureSession::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_mesh", "mesh"), &FastTextureSession::set_mesh);
	ClassDB::bind_method(D_METHOD("get_mesh"), &FastTextureSession::get_mesh);
	ClassDB::bind_method(D_METHOD("open", "face_ids"), &FastTextureSession::open);
	ClassDB::bind_method(D_METHOD("set_mode", "mode", "spacing"), &FastTextureSession::set_mode, DEFVAL(SPACING_LENGTH));
	ClassDB::bind_method(D_METHOD("set_nudge", "nudge"), &FastTextureSession::set_nudge);
	ClassDB::bind_method(D_METHOD("can_accept"), &FastTextureSession::can_accept);
	ClassDB::bind_method(D_METHOD("accept"), &FastTextureSession::accept);
	ClassDB::bind_method(D_METHOD("cancel"), &FastTextureSession::cancel);
	ClassDB::bind_method(D_METHOD("is_active"), &FastTextureSession::is_active);
	ClassDB::bind_method(D_METHOD("get_mode"), &FastTextureSession::get_mode);
	ClassDB::bind_method(D_METHOD("get_spacing"), &FastTextureSession::get_spacing);
	ClassDB::bind_method(D_METHOD("get_nudge"), &FastTextureSession::get_nudge);
	ClassDB::bind_method(D_METHOD("get_face_ids"), &FastTextureSession::get_face_ids);
	ClassDB::bind_method(D_METHOD("get_mode_loop_uvs"), &FastTextureSession::get_mode_loop_uvs);
	ClassDB::bind_method(D_METHOD("get_working_loop_uvs"), &FastTextureSession::get_working_loop_uvs);
	ClassDB::bind_method(D_METHOD("get_working_data"), &FastTextureSession::get_working_data);
	ClassDB::bind_method(D_METHOD("get_last_diff"), &FastTextureSession::get_last_diff);
	ClassDB::bind_method(D_METHOD("get_last_error"), &FastTextureSession::get_last_error);

	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "mesh", PROPERTY_HINT_RESOURCE_TYPE, "LevelMesh"), "set_mesh", "get_mesh");
	ADD_SIGNAL(MethodInfo("changed"));

	BIND_ENUM_CONSTANT(MODE_USE_EXISTING);
	BIND_ENUM_CONSTANT(MODE_CONFORMING);
	BIND_ENUM_CONSTANT(MODE_SQUARE);
	BIND_ENUM_CONSTANT(MODE_FOLLOW_QUADS);
	BIND_ENUM_CONSTANT(MODE_PLANAR);
	BIND_ENUM_CONSTANT(SPACING_LENGTH);
	BIND_ENUM_CONSTANT(SPACING_EVEN);
	BIND_ENUM_CONSTANT(SPACING_LENGTH_AVERAGE);
}
