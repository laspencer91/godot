/**************************************************************************/
/*  level_mesh_data.h                                                     */
/**************************************************************************/

#pragma once

#include "core/io/resource.h"
#include "core/math/transform_2d.h"
#include "core/variant/type_info.h"

class LevelMesh;
class LevelMeshAdjacency;
class LevelMeshBaker;
class LevelMeshDiff;
class LevelMeshElementBVH;

class LevelMeshData : public Resource {
	GDCLASS(LevelMeshData, Resource);

public:
	enum UVMode {
		UV_MODE_PROJECTED,
		UV_MODE_EXPLICIT,
	};

	enum FaceFlag {
		FACE_FLAG_NONE = 0,
		FACE_FLAG_SMOOTH = 1 << 0,
		FACE_FLAG_TEXTURE_LOCK = 1 << 1,
	};

private:
	// Physical row ids are stable. Alive columns distinguish active rows from
	// holes, while the free-id columns reserve those holes for later Euler ops.
	PackedVector3Array vertex_positions;
	PackedByteArray vertex_alive;
	// Stored as signed 32-bit words for Resource/Variant compatibility; the
	// bit pattern is interpreted as an unsigned generation stamp.
	PackedInt32Array vertex_generations;
	PackedInt32Array free_vertex_ids;

	// Two consecutive entries per physical edge row: v0, v1.
	PackedInt32Array edge_vertices;
	PackedByteArray edge_alive;
	PackedInt32Array edge_generations;
	PackedInt32Array free_edge_ids;

	PackedInt32Array face_loop_starts;
	PackedInt32Array face_loop_counts;
	PackedInt32Array face_material_indices;
	PackedInt32Array face_uv_modes;
	PackedVector3Array face_uv_origins;
	PackedVector3Array face_uv_tangents;
	// Six consecutive floats per face: Transform2D x, y, then origin columns.
	PackedFloat32Array face_uv_transforms;
	PackedInt32Array face_polygroup_ids;
	PackedInt32Array face_flags;
	PackedByteArray face_alive;
	PackedInt32Array face_generations;
	PackedInt32Array free_face_ids;

	PackedInt32Array loop_vertex_indices;
	PackedVector2Array loop_uv0;
	PackedColorArray loop_colors;
	PackedVector3Array loop_normals;
	PackedByteArray loop_alive;
	PackedInt32Array free_loop_ids;

	// These counters deliberately do not roll back with snapshot copies. If a
	// newly-created tail row is undone and a different edit later reuses that
	// physical slot, it must receive a generation no stale handle has seen.
	uint32_t next_vertex_generation = 1;
	uint32_t next_edge_generation = 1;
	uint32_t next_face_generation = 1;

	static inline int64_t _pack_handle(int p_slot, uint32_t p_generation) {
		if (p_slot < 0) {
			return -1;
		}
		return (int64_t)(((uint64_t)p_generation << 32) | (uint32_t)p_slot);
	}

	static inline int _resolve_handle(int64_t p_handle, const PackedByteArray &p_alive, const PackedInt32Array &p_generations) {
		const uint64_t bits = (uint64_t)p_handle;
		const uint32_t slot_bits = (uint32_t)(bits & UINT32_MAX);
		if (slot_bits > INT32_MAX) {
			return -1;
		}
		const int slot = (int)slot_bits;
		const uint32_t generation = (uint32_t)(bits >> 32);
		if (slot >= p_alive.size() || slot >= p_generations.size() || p_alive[slot] == 0 ||
				(uint32_t)p_generations[slot] != generation) {
			return -1;
		}
		return slot;
	}

	static int _count_alive(const PackedByteArray &p_alive);
	static uint32_t _bump_generation(uint32_t p_generation);
	static bool _append_free_id(PackedInt32Array &r_free_ids, int p_slot);
	static uint32_t _generation_at(const PackedInt32Array &p_generations, int p_slot);
	static Transform2D _read_uv_transform(const LevelMeshData &p_data, int p_face_id);
	static void _write_uv_transform(LevelMeshData &r_data, int p_face_id, const Transform2D &p_transform);
	static void _advance_generation_counter(uint32_t p_generation, uint32_t &r_next_generation);
	uint32_t _claim_vertex_generation();
	uint32_t _claim_edge_generation();
	uint32_t _claim_face_generation();
	void _ensure_generation_columns();
	void _copy_from(const LevelMeshData &p_other, bool p_emit_changed);
	void _emit_mesh_diff_applied(const Ref<LevelMeshDiff> &p_diff, bool p_reverted);
	void _emit_mesh_preview_changed();

	friend class LevelMesh;
	friend class LevelMeshAdjacency;
	friend class LevelMeshBaker;
	friend class LevelMeshDiff;
	friend class LevelMeshElementBVH;

protected:
	static void _bind_methods();

public:
	void set_vertex_positions(const PackedVector3Array &p_values);
	PackedVector3Array get_vertex_positions() const;
	void set_vertex_alive(const PackedByteArray &p_values);
	PackedByteArray get_vertex_alive() const;
	void set_vertex_generations(const PackedInt32Array &p_values);
	PackedInt32Array get_vertex_generations() const;
	void set_free_vertex_ids(const PackedInt32Array &p_values);
	PackedInt32Array get_free_vertex_ids() const;

	void set_edge_vertices(const PackedInt32Array &p_values);
	PackedInt32Array get_edge_vertices() const;
	void set_edge_alive(const PackedByteArray &p_values);
	PackedByteArray get_edge_alive() const;
	void set_edge_generations(const PackedInt32Array &p_values);
	PackedInt32Array get_edge_generations() const;
	void set_free_edge_ids(const PackedInt32Array &p_values);
	PackedInt32Array get_free_edge_ids() const;

	void set_face_loop_starts(const PackedInt32Array &p_values);
	PackedInt32Array get_face_loop_starts() const;
	void set_face_loop_counts(const PackedInt32Array &p_values);
	PackedInt32Array get_face_loop_counts() const;
	void set_face_material_indices(const PackedInt32Array &p_values);
	PackedInt32Array get_face_material_indices() const;
	void set_face_uv_modes(const PackedInt32Array &p_values);
	PackedInt32Array get_face_uv_modes() const;
	void set_face_uv_origins(const PackedVector3Array &p_values);
	PackedVector3Array get_face_uv_origins() const;
	void set_face_uv_tangents(const PackedVector3Array &p_values);
	PackedVector3Array get_face_uv_tangents() const;
	void set_face_uv_transforms(const PackedFloat32Array &p_values);
	PackedFloat32Array get_face_uv_transforms() const;
	void set_face_polygroup_ids(const PackedInt32Array &p_values);
	PackedInt32Array get_face_polygroup_ids() const;
	void set_face_flags(const PackedInt32Array &p_values);
	PackedInt32Array get_face_flags() const;
	void set_face_alive(const PackedByteArray &p_values);
	PackedByteArray get_face_alive() const;
	void set_face_generations(const PackedInt32Array &p_values);
	PackedInt32Array get_face_generations() const;
	void set_free_face_ids(const PackedInt32Array &p_values);
	PackedInt32Array get_free_face_ids() const;

	void set_loop_vertex_indices(const PackedInt32Array &p_values);
	PackedInt32Array get_loop_vertex_indices() const;
	void set_loop_uv0(const PackedVector2Array &p_values);
	PackedVector2Array get_loop_uv0() const;
	void set_loop_colors(const PackedColorArray &p_values);
	PackedColorArray get_loop_colors() const;
	void set_loop_normals(const PackedVector3Array &p_values);
	PackedVector3Array get_loop_normals() const;
	void set_loop_alive(const PackedByteArray &p_values);
	PackedByteArray get_loop_alive() const;
	void set_free_loop_ids(const PackedInt32Array &p_values);
	PackedInt32Array get_free_loop_ids() const;

	Transform2D get_face_uv_transform(int p_face_id) const;
	void set_face_uv_transform(int p_face_id, const Transform2D &p_transform);
	bool face_is_bakeable(int p_face_id) const;

	int vertex_count() const;
	int edge_count() const;
	int face_count() const;
	int loop_count() const;

	// Low-level slot primitives for future Euler operators. They intentionally
	// do not repair dependent topology; composed operators own that work.
	bool free_vertex_slot(int p_vertex_id);
	bool free_edge_slot(int p_edge_id);
	bool free_face_slot(int p_face_id);
	void clear();

	Ref<LevelMeshData> duplicate_data() const;
	void copy_from(const Ref<LevelMeshData> &p_other);
};

VARIANT_ENUM_CAST(LevelMeshData::UVMode);
VARIANT_BITFIELD_CAST(LevelMeshData::FaceFlag);
