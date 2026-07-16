/**************************************************************************/
/*  level_mesh.h                                                          */
/**************************************************************************/

#pragma once

#include "core/math/rect2.h"
#include "core/math/transform_3d.h"
#include "core/object/ref_counted.h"
#include "core/templates/vector.h"
#include "core/variant/array.h"
#include "core/variant/type_info.h"

class LevelMeshData;
class LevelMeshDiff;
class LevelMeshAdjacency;
class LevelMeshElementBVH;

class LevelMesh : public RefCounted {
	GDCLASS(LevelMesh, RefCounted);

public:
	enum UnwrapSpacingMode {
		UNWRAP_SPACING_LENGTH,
		UNWRAP_SPACING_EVEN,
		UNWRAP_SPACING_LENGTH_AVERAGE,
	};

	enum UnwrapError {
		UNWRAP_ERROR_NONE,
		UNWRAP_ERROR_BUSY,
		UNWRAP_ERROR_EMPTY_SELECTION,
		UNWRAP_ERROR_INVALID_FACE,
		UNWRAP_ERROR_INVALID_TOPOLOGY,
		UNWRAP_ERROR_NON_MANIFOLD_EDGE,
		UNWRAP_ERROR_INVALID_SEED,
		UNWRAP_ERROR_INVALID_SPACING_MODE,
		UNWRAP_ERROR_INVALID_THRESHOLD,
		UNWRAP_ERROR_UNFOLD_FAILED,
	};

	static constexpr real_t DEFAULT_CONFORMING_DISTORTION_THRESHOLD = (real_t)3.14159265358979323846;
	static constexpr real_t CYCLE_CLOSURE_EPSILON = (real_t)1e-5;

private:
	Ref<LevelMeshData> data;
	Ref<LevelMeshData> transaction_before;
	Ref<LevelMeshAdjacency> adjacency;
	Ref<LevelMeshElementBVH> element_bvh;
	bool transaction_active = false;
	bool transaction_changed = false;
	bool transform_preview_active = false;
	bool geometry_change_notification = false;
	PackedInt32Array transform_preview_vertex_ids;
	PackedInt32Array transform_preview_face_ids;
	UnwrapError last_unwrap_error = UNWRAP_ERROR_NONE;
	PackedInt32Array last_unwrap_seam_edge_ids;
	// WP21: editor-session diagnostics from the last committed hotspot fit.
	// Deliberately not part of LevelMeshData/diffs/serialization.
	Array last_hotspot_fit_diagnostics;

	static Vector3 _dominant_axis_tangent(const Vector3 &p_normal);
	static Vector3 _world_aligned_uv_tangent(const Vector3 &p_normal);
	static void _append_uv_transform(LevelMeshData &r_data, const Transform2D &p_transform);
	static bool _compute_face_geometry(const LevelMeshData &p_data, int p_face_id,
			Vector3 &r_centroid, Vector3 &r_normal, real_t &r_area_x2, real_t &r_longest_edge_squared);
	static bool _compute_face_basis(const LevelMeshData &p_data, int p_face_id, bool p_canonical_tangent,
			Vector3 &r_origin, Vector3 &r_tangent, Vector3 &r_bitangent, Vector3 &r_normal);
	static bool _get_projection_basis(const LevelMeshData &p_data, int p_face_id,
			Vector3 &r_tangent, Vector3 &r_bitangent, Vector3 &r_normal);
	static bool _sample_explicit_uv(const LevelMeshData &p_data, int p_face_id, const Vector3 &p_point,
			int p_loop_id, Vector2 &r_uv);

	int _next_polygroup_id() const;
	int _append_vertex(const Vector3 &p_position);
	int _append_edge(int p_vertex_a, int p_vertex_b);
	int _append_quad_face(const int p_vertex_ids[4], int p_material_index, int p_polygroup_id, int p_face_flags);
	bool _get_face_directed_edge(int p_face_id, int p_edge_id, int &r_vertex_a, int &r_vertex_b) const;
	bool _initialize_face_projection(int p_face_id);
	bool _initialize_wall_projection(int p_face_id, int p_owner_face_id, int p_vertex_a, int p_vertex_b);
	bool _get_shared_edge_vertices(int p_face_a, int p_face_b, int &r_edge_id, int &r_vertex_a, int &r_vertex_b) const;
	bool _calculate_wrap_transform_internal(int p_source_face_id, int p_destination_face_id,
			Transform2D &r_transform, int &r_edge_id, String &r_reason) const;
	bool _apply_wrap_pair(int p_source_face_id, int p_destination_face_id);
	bool _get_face_uv_bounds(int p_face_id, Rect2 &r_bounds) const;
	bool _set_grid_frame(int p_face_id, bool p_reset_transform);
	bool _set_face_frame(int p_face_id, bool p_reset_transform);
	bool _solve_texture_lock(int p_face_id, const LevelMeshData &p_before, const Transform3D *p_exact_transform);
	bool _restore_transform_preview_baseline();
	bool _validate_unwrap_selection(const PackedInt32Array &p_face_ids, Vector<int> &r_face_ids);
	bool _collect_push_pull(const PackedInt32Array &p_face_ids, real_t p_distance,
			PackedInt32Array &r_vertex_ids, PackedVector3Array &r_positions) const;
	bool _reconcile_face_uv(int p_face_id, const LevelMeshData *p_before = nullptr,
			int p_source_face_id = -1, bool p_transfer_explicit = false,
			const Vector3 *p_normal_override = nullptr);
	bool _restore_diff_state(const Ref<LevelMeshDiff> &p_diff, bool p_reverted);
	void _invalidate_topology();
	void _invalidate_geometry();
	void _on_data_changed();

protected:
	static void _bind_methods();

public:
	enum TextureModifyOperation {
		TEXTURE_MODIFY_SHIFT,
		TEXTURE_MODIFY_SCALE,
		TEXTURE_MODIFY_ROTATE,
		TEXTURE_MODIFY_FIT,
		TEXTURE_MODIFY_JUSTIFY_LEFT,
		TEXTURE_MODIFY_JUSTIFY_RIGHT,
		TEXTURE_MODIFY_JUSTIFY_TOP,
		TEXTURE_MODIFY_JUSTIFY_BOTTOM,
		TEXTURE_MODIFY_JUSTIFY_CENTER,
		TEXTURE_MODIFY_FLIP_HORIZONTAL,
		TEXTURE_MODIFY_FLIP_VERTICAL,
	};

	void set_data(const Ref<LevelMeshData> &p_data);
	Ref<LevelMeshData> get_data() const;

	void begin_transaction();
	Ref<LevelMeshDiff> commit();
	void rollback();
	bool is_transaction_active() const;

	bool create_box(const Transform3D &p_frame, const Vector3 &p_size, int p_material_index);
	bool reconcile_face_uv(int p_face_id);
	bool set_face_texture_lock(int p_face_id, bool p_enabled);
	bool is_face_texture_locked(int p_face_id) const;

	// Native projection coordinates are measured in world units. Therefore an
	// identity uv_transform means exactly one UV unit per world unit; material
	// texel-density conversion is intentionally a later editor-layer concern.
	Vector2 project_native(int p_face_id, const Vector3 &p_point) const;
	Vector2 get_uv(int p_face_id, const Vector3 &p_point, int p_loop_id = -1) const;
	int get_face_uv_mode(int p_face_id) const;
	Vector3 get_face_uv_origin(int p_face_id) const;
	Vector3 get_face_uv_tangent(int p_face_id) const;
	Transform2D get_face_uv_transform(int p_face_id) const;
	Dictionary capture_face_texture(int p_face_id) const;
	Ref<LevelMeshDiff> apply_face_texture(const PackedInt32Array &p_face_ids, const String &p_material_path,
			const Dictionary &p_captured_mapping = Dictionary());
	Dictionary calculate_wrap_transform(int p_source_face_id, int p_destination_face_id) const;
	Ref<LevelMeshDiff> wrap_faces(int p_source_face_id, const PackedInt32Array &p_destination_face_ids);
	Ref<LevelMeshDiff> flow_faces(const PackedInt32Array &p_ordered_face_ids);
	Ref<LevelMeshDiff> modify_face_uv(const PackedInt32Array &p_face_ids, int p_operation,
			const Vector2 &p_value = Vector2(1, 1));
	Ref<LevelMeshDiff> apply_hotspot_fit(const Array &p_face_results);
	Array get_last_hotspot_fit_diagnostics() const { return last_hotspot_fit_diagnostics; }
	Ref<LevelMeshDiff> align_faces_to_grid(const PackedInt32Array &p_face_ids);
	Ref<LevelMeshDiff> align_faces_to_face(const PackedInt32Array &p_face_ids);
	static Vector3 grid_uv_tangent_for_normal(const Vector3 &p_normal);
	static Dictionary solve_edge_hinge_similarity(const Vector2 &p0, const Vector2 &p1,
			const Vector2 &q0, const Vector2 &q1);
	static Dictionary unfold_face_across_edge(const Ref<LevelMeshData> &p_data,
			int p_face_a, int p_edge_id, int p_face_b);
	Ref<LevelMeshDiff> unwrap_square(const PackedInt32Array &p_face_ids);
	Ref<LevelMeshDiff> unwrap_planar(const PackedInt32Array &p_face_ids);
	Ref<LevelMeshDiff> unwrap_conforming(const PackedInt32Array &p_face_ids,
			real_t p_distortion_threshold = DEFAULT_CONFORMING_DISTORTION_THRESHOLD);
	Ref<LevelMeshDiff> unwrap_follow_quads(const PackedInt32Array &p_face_ids, int p_spacing_mode);
	UnwrapError get_last_unwrap_error() const;
	PackedInt32Array get_last_unwrap_seam_edge_ids() const;

	bool begin_transform_preview(const PackedInt32Array &p_vertex_ids);
	bool preview_transform_vertices(const PackedVector3Array &p_new_positions);
	Ref<LevelMeshDiff> commit_transform_preview();
	void cancel_transform_preview();
	bool is_transform_preview_active() const;

	Ref<LevelMeshDiff> extrude_faces(const PackedInt32Array &p_face_ids);
	Dictionary calculate_push_pull(const PackedInt32Array &p_face_ids, real_t p_distance) const;
	Ref<LevelMeshDiff> push_pull_faces(const PackedInt32Array &p_face_ids, real_t p_distance);
	Ref<LevelMeshDiff> extrude_boundary_edges(const PackedInt32Array &p_edge_ids);

	bool apply_diff(const Ref<LevelMeshDiff> &p_diff);
	bool revert_diff(const Ref<LevelMeshDiff> &p_diff);

	Ref<LevelMeshAdjacency> get_adjacency() const;
	Ref<LevelMeshElementBVH> get_element_bvh() const;
	Dictionary ray_closest(const Vector3 &p_local_origin, const Vector3 &p_local_direction) const;
	PackedInt32Array get_face_corner_vertex_ids(int p_face_id) const;
	PackedVector3Array get_face_corner_positions(int p_face_id) const;
	int get_face_triangle_count(int p_face_id) const;
	PackedInt32Array get_face_triangle_vertex_ids(int p_face_id, int p_local_tri) const;
	Vector3 get_face_normal(int p_face_id) const;
	PackedInt32Array get_face_boundary_edge_ids(int p_face_id, bool p_polygroup_tier = false) const;
	PackedVector3Array get_face_boundary_edge_positions(int p_face_id, bool p_polygroup_tier = false) const;

	int64_t make_vertex_handle(int p_vertex_id) const;
	int64_t make_edge_handle(int p_edge_id) const;
	int64_t make_face_handle(int p_face_id) const;
	int resolve_vertex(int64_t p_handle) const;
	int resolve_edge(int64_t p_handle) const;
	int resolve_face(int64_t p_handle) const;

	LevelMesh();
	~LevelMesh();
};

VARIANT_ENUM_CAST(LevelMesh::TextureModifyOperation);
VARIANT_ENUM_CAST(LevelMesh::UnwrapSpacingMode);
VARIANT_ENUM_CAST(LevelMesh::UnwrapError);
