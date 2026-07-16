extends SceneTree

const UV_PROPERTIES: PackedStringArray = [
	"face_uv_modes",
	"face_uv_origins",
	"face_uv_tangents",
	"face_uv_transforms",
	"loop_uv0",
]

var failures: Array[String] = []


func _initialize() -> void:
	_check_frame_algebra_and_round_trip()
	_check_grid_frame_continuity()
	_check_rigid_texture_lock()
	_check_partial_texture_lock_and_degeneracy()
	_check_extrude_inheritance()
	_check_hinge_solver()
	_check_explicit_read_adapter()
	if failures.is_empty():
		print("UV_SMOKE_OK")
		quit(0)
	else:
		printerr("UV_SMOKE_FAIL: %s" % ", ".join(failures))
		quit(1)


func _make_box(frame := Transform3D.IDENTITY, size := Vector3(4, 4, 4), material := 3) -> LevelMesh:
	var mesh := LevelMesh.new()
	mesh.begin_transaction()
	if not mesh.create_box(frame, size, material):
		_check(false, "make_box_create")
		mesh.rollback()
		return mesh
	if mesh.commit() == null:
		_check(false, "make_box_commit")
	return mesh


func _check_frame_algebra_and_round_trip() -> void:
	var frame := Transform3D(
			Basis(Vector3(0.31, 0.77, 0.55).normalized(), 0.83),
			Vector3(3.5, -2.25, 6.75))
	var mesh := _make_box(frame, Vector3(6, 2, 4))
	var face_id := 1
	var before_align := _capture_uv(mesh.data)
	var grid_diff: LevelMeshDiff = mesh.align_faces_to_grid(PackedInt32Array([face_id]))
	var normal := mesh.get_face_normal(face_id)
	var expected_grid_tangent: Vector3 = LevelMesh.grid_uv_tangent_for_normal(normal)
	_check(grid_diff != null and mesh.get_face_uv_mode(face_id) == LevelMeshData.UV_MODE_PROJECTED and
			mesh.get_face_uv_origin(face_id) == Vector3.ZERO and
			mesh.get_face_uv_tangent(face_id) == expected_grid_tangent,
			"grid_frame_builder_table_and_origin")

	var arbitrary_transform := Transform2D(
			Vector2(1.3, 0.25), Vector2(-0.4, 0.8), Vector2(2.75, -1.125))
	mesh.begin_transaction()
	mesh.data.set_face_uv_transform(face_id, arbitrary_transform)
	var reconciled := mesh.reconcile_face_uv(face_id)
	var transform_diff: LevelMeshDiff = mesh.commit()
	var corners := mesh.get_face_corner_positions(face_id)
	var point: Vector3 = corners[0] * 0.17 + corners[1] * 0.29 + corners[2] * 0.31 + corners[3] * 0.23
	var native := mesh.project_native(face_id, point)
	var round_trip: Vector2 = arbitrary_transform.affine_inverse() * mesh.get_uv(face_id, point)
	_check(reconciled and transform_diff != null and round_trip.is_equal_approx(native),
			"projected_round_trip_arbitrary_face")

	# A zero prior tangent selects the deterministic longest boundary edge.
	mesh.begin_transaction()
	mesh.data.set_face_uv_tangent(face_id, Vector3.ZERO)
	var zero_tangent_diff: LevelMeshDiff = mesh.commit()
	var before_face_align := _capture_uv(mesh.data)
	var face_diff: LevelMeshDiff = mesh.align_faces_to_face(PackedInt32Array([face_id]))
	var face_positions := mesh.get_face_corner_positions(face_id)
	var longest := Vector3.ZERO
	var longest_squared := 0.0
	var centroid := Vector3.ZERO
	for corner in range(face_positions.size()):
		centroid += face_positions[corner]
		var edge: Vector3 = face_positions[(corner + 1) % face_positions.size()] - face_positions[corner]
		if edge.length_squared() > longest_squared + max(1.0, longest_squared) * 0.000001:
			longest_squared = edge.length_squared()
			longest = edge.normalized()
	centroid /= face_positions.size()
	_check(zero_tangent_diff != null and face_diff != null and
			mesh.get_face_uv_origin(face_id).is_equal_approx(centroid) and
			mesh.get_face_uv_tangent(face_id).is_equal_approx(longest),
			"face_frame_longest_edge_fallback")
	_check(mesh.revert_diff(face_diff) and _uv_matches(mesh.data, before_face_align),
			"frame_builder_undo_byte_exact")
	_check(mesh.revert_diff(grid_diff) and _uv_matches(mesh.data, before_align),
			"grid_frame_undo_byte_exact")


func _check_grid_frame_continuity() -> void:
	var left := _make_box(Transform3D(Basis.IDENTITY, Vector3(-2, 0, 0)))
	var right := _make_box(Transform3D(Basis.IDENTITY, Vector3(2, 0, 0)))
	var left_diff: LevelMeshDiff = left.align_faces_to_grid(PackedInt32Array([1]))
	var right_diff: LevelMeshDiff = right.align_faces_to_grid(PackedInt32Array([1]))
	var continuous := left_diff != null and right_diff != null
	for point in [Vector3(0, -1.75, 2), Vector3(0, 0.125, 2), Vector3(0, 1.5, 2)]:
		continuous = continuous and left.get_uv(1, point).is_equal_approx(right.get_uv(1, point))
	_check(continuous, "grid_frame_cross_block_continuity")


func _check_rigid_texture_lock() -> void:
	var mesh := _make_box()
	var faces := PackedInt32Array([0, 1, 2, 3, 4, 5])
	var align_diff: LevelMeshDiff = mesh.align_faces_to_grid(faces)
	var before_uv := _capture_uv(mesh.data)
	var old_positions: PackedVector3Array = mesh.data.vertex_positions
	var old_loop_uvs: PackedVector2Array = mesh.data.loop_uv0
	var vertex_ids := PackedInt32Array()
	var moved_positions := PackedVector3Array()
	var rotation := Basis(Vector3(0.37, -0.58, 0.72).normalized(), 1.137)
	var translation := Vector3(4.25, -3.5, 2.125)
	for vertex_id in range(old_positions.size()):
		vertex_ids.append(vertex_id)
		moved_positions.append(rotation * old_positions[vertex_id] + translation)
	var began := mesh.begin_transform_preview(vertex_ids)
	var previewed := began and mesh.preview_transform_vertices(moved_positions)
	var preview_uv_unchanged := mesh.data.loop_uv0 == old_loop_uvs
	var rigid_diff: LevelMeshDiff = mesh.commit_transform_preview() if previewed else null
	var locked := align_diff != null and rigid_diff != null
	for face_id in range(mesh.data.face_alive.size()):
		if mesh.data.face_alive[face_id] == 0:
			continue
		var loop_start: int = mesh.data.face_loop_starts[face_id]
		var loop_count: int = mesh.data.face_loop_counts[face_id]
		for corner in range(loop_count):
			var loop_id := loop_start + corner
			var position: Vector3 = mesh.data.vertex_positions[mesh.data.loop_vertex_indices[loop_id]]
			locked = locked and mesh.get_uv(face_id, position, loop_id).is_equal_approx(old_loop_uvs[loop_id])
			locked = locked and mesh.data.loop_uv0[loop_id].is_equal_approx(old_loop_uvs[loop_id])
	_check(previewed and preview_uv_unchanged, "transform_preview_positions_only")
	_check(locked, "rigid_texture_lock_exact_path")
	_check(mesh.revert_diff(rigid_diff) and _uv_matches(mesh.data, before_uv),
			"rigid_texture_lock_undo_byte_exact")


func _check_partial_texture_lock_and_degeneracy() -> void:
	var mesh := _make_box()
	var face_id := 1
	var align_diff: LevelMeshDiff = mesh.align_faces_to_grid(PackedInt32Array([face_id]))
	var loop_start: int = mesh.data.face_loop_starts[face_id]
	var loop_count: int = mesh.data.face_loop_counts[face_id]
	var face_vertices := mesh.get_face_corner_vertex_ids(face_id)
	var moved_corner := 2
	var moved_vertex: int = face_vertices[moved_corner]
	var old_uvs := PackedVector2Array()
	for corner in range(loop_count):
		old_uvs.append(mesh.data.loop_uv0[loop_start + corner])
	var old_position: Vector3 = mesh.data.vertex_positions[moved_vertex]
	var began := mesh.begin_transform_preview(PackedInt32Array([moved_vertex]))
	var previewed := began and mesh.preview_transform_vertices(PackedVector3Array([
			old_position + Vector3(0.45, -0.3, 0.8),
	]))
	var partial_diff: LevelMeshDiff = mesh.commit_transform_preview() if previewed else null
	var stable_exact := align_diff != null and partial_diff != null
	var stable_corners: Array[int] = []
	for corner in range(loop_count):
		if corner != moved_corner:
			stable_corners.append(corner)
			stable_exact = stable_exact and mesh.data.loop_uv0[loop_start + corner] == old_uvs[corner]
	var p0 := mesh.project_native(face_id, mesh.data.vertex_positions[face_vertices[stable_corners[0]]])
	var p1 := mesh.project_native(face_id, mesh.data.vertex_positions[face_vertices[stable_corners[1]]])
	var p2 := mesh.project_native(face_id, mesh.data.vertex_positions[face_vertices[stable_corners[2]]])
	var stable_fit := _solve_affine_three(p0, p1, p2,
			old_uvs[stable_corners[0]], old_uvs[stable_corners[1]], old_uvs[stable_corners[2]])
	var moved_native := mesh.project_native(face_id, mesh.data.vertex_positions[moved_vertex])
	var expected_moved: Vector2 = stable_fit * moved_native
	_check(stable_exact, "partial_texture_lock_stationary_loops_bit_exact")
	_check(mesh.data.loop_uv0[loop_start + moved_corner].is_equal_approx(expected_moved),
			"partial_texture_lock_affine_prediction")

	var before_collapse_uv := _capture_uv(mesh.data)
	var frozen_transform := _face_transform_words(mesh.data, face_id)
	var collapse_positions := PackedVector3Array()
	var anchor: Vector3 = mesh.data.vertex_positions[face_vertices[0]]
	for corner in range(face_vertices.size()):
		collapse_positions.append(anchor + Vector3(0.0000001 * corner, 0, 0))
	var collapse_began := mesh.begin_transform_preview(face_vertices)
	var collapse_previewed := collapse_began and mesh.preview_transform_vertices(collapse_positions)
	var collapse_diff: LevelMeshDiff = mesh.commit_transform_preview() if collapse_previewed else null
	var finite := mesh.get_face_uv_transform(face_id).is_finite()
	for uv in mesh.data.loop_uv0:
		finite = finite and uv.is_finite()
	_check(collapse_diff != null and _face_transform_words(mesh.data, face_id) == frozen_transform and finite,
			"partial_texture_lock_degenerate_freeze")
	_check(mesh.revert_diff(collapse_diff) and _uv_matches(mesh.data, before_collapse_uv),
			"degenerate_texture_lock_undo_byte_exact")


func _check_extrude_inheritance() -> void:
	var mesh := _make_box(Transform3D(Basis.IDENTITY, Vector3(7, 11, 13)), Vector3(4, 4, 4), 17)
	var face_id := 1
	var cap_transform := Transform2D(
			Vector2(0.75, 0.2), Vector2(-0.35, 1.1), Vector2(8.0, -3.0))
	mesh.begin_transaction()
	mesh.data.set_face_uv_transform(face_id, cap_transform)
	var flags: PackedInt32Array = mesh.data.face_flags
	flags[face_id] |= LevelMeshData.FACE_FLAG_SMOOTH
	mesh.data.face_flags = flags
	var cap_reconciled := mesh.reconcile_face_uv(face_id)
	var cap_setup_diff: LevelMeshDiff = mesh.commit()
	var before_extrude_uv := _capture_uv(mesh.data)
	var cap_uv_before: PackedVector2Array = _face_uvs(mesh.data, face_id)
	var source_material: int = mesh.data.face_material_indices[face_id]
	var topology_diff: LevelMeshDiff = mesh.extrude_faces(PackedInt32Array([face_id]))
	var wall_ids: Array[int] = []
	if topology_diff != null:
		for handle in topology_diff.get_created_face_handles():
			var wall_id := mesh.resolve_face(handle)
			if wall_id >= 0:
				wall_ids.append(wall_id)
	var topology_uv_ok := cap_reconciled and cap_setup_diff != null and topology_diff != null and wall_ids.size() == 4
	topology_uv_ok = topology_uv_ok and _face_uvs(mesh.data, face_id) == cap_uv_before
	for wall_id in wall_ids:
		var wall_uvs := _face_uvs(mesh.data, wall_id)
		var all_finite_nonzero := not wall_uvs.is_empty()
		for uv in wall_uvs:
			all_finite_nonzero = all_finite_nonzero and uv.is_finite() and uv != Vector2.ZERO
		topology_uv_ok = topology_uv_ok and mesh.get_face_uv_mode(wall_id) == LevelMeshData.UV_MODE_PROJECTED
		topology_uv_ok = topology_uv_ok and mesh.get_face_uv_origin(wall_id) == Vector3.ZERO
		topology_uv_ok = topology_uv_ok and mesh.get_face_uv_transform(wall_id).is_equal_approx(Transform2D.IDENTITY)
		topology_uv_ok = topology_uv_ok and not mesh.get_face_uv_transform(wall_id).is_equal_approx(cap_transform)
		topology_uv_ok = topology_uv_ok and mesh.data.face_material_indices[wall_id] == source_material
		topology_uv_ok = topology_uv_ok and (
				mesh.data.face_flags[wall_id] & LevelMeshData.FACE_FLAG_SMOOTH) != 0
		topology_uv_ok = topology_uv_ok and all_finite_nonzero
	_check(topology_uv_ok, "extrude_wall_fresh_grid_inheritance")

	var cap_vertices := mesh.get_face_corner_vertex_ids(face_id)
	var cap_positions := PackedVector3Array()
	for vertex_id in cap_vertices:
		cap_positions.append(mesh.data.vertex_positions[vertex_id] + Vector3(0, 0, 2.5))
	var began := mesh.begin_transform_preview(cap_vertices)
	var previewed := began and mesh.preview_transform_vertices(cap_positions)
	var geometry_diff: LevelMeshDiff = mesh.commit_transform_preview() if previewed else null
	var final_uv_ok := geometry_diff != null
	for corner in range(cap_uv_before.size()):
		final_uv_ok = final_uv_ok and mesh.data.loop_uv0[mesh.data.face_loop_starts[face_id] + corner].is_equal_approx(cap_uv_before[corner])
	for wall_id in wall_ids:
		var normal := mesh.get_face_normal(wall_id)
		final_uv_ok = final_uv_ok and normal.length_squared() > 0.9
		final_uv_ok = final_uv_ok and mesh.get_face_uv_tangent(wall_id) == LevelMesh.grid_uv_tangent_for_normal(normal)
		final_uv_ok = final_uv_ok and mesh.get_face_uv_transform(wall_id).is_equal_approx(Transform2D.IDENTITY)
		var wall_start: int = mesh.data.face_loop_starts[wall_id]
		for corner in range(mesh.data.face_loop_counts[wall_id]):
			var loop_id := wall_start + corner
			var position: Vector3 = mesh.data.vertex_positions[mesh.data.loop_vertex_indices[loop_id]]
			final_uv_ok = final_uv_ok and mesh.data.loop_uv0[loop_id].is_equal_approx(mesh.get_uv(wall_id, position, loop_id))
	_check(final_uv_ok, "extrude_cap_lock_and_wall_reconcile")
	var undo_ok := mesh.revert_diff(geometry_diff) and mesh.revert_diff(topology_diff)
	_check(undo_ok and _uv_matches(mesh.data, before_extrude_uv),
			"extrude_uv_undo_byte_exact")


func _check_hinge_solver() -> void:
	var mesh := _make_box()
	var face_a := 1
	var face_b := 3
	var align_a: LevelMeshDiff = mesh.align_faces_to_grid(PackedInt32Array([face_a]))
	var align_b: LevelMeshDiff = mesh.align_faces_to_grid(PackedInt32Array([face_b]))
	var transform_a := Transform2D(
			Vector2(1.15, 0.42), Vector2(-0.27, 0.93), Vector2(3.2, -4.7))
	mesh.begin_transaction()
	mesh.data.set_face_uv_transform(face_a, transform_a)
	var reconciled_a := mesh.reconcile_face_uv(face_a)
	var source_diff: LevelMeshDiff = mesh.commit()
	var shared_vertices: Array[int] = []
	for vertex_id in mesh.get_face_corner_vertex_ids(face_a):
		if mesh.get_face_corner_vertex_ids(face_b).has(vertex_id):
			shared_vertices.append(vertex_id)
	var solved_ok := align_a != null and align_b != null and reconciled_a and source_diff != null and shared_vertices.size() == 2
	if shared_vertices.size() != 2:
		_check(false, "hinge_shared_edge")
		return
	var world_p0: Vector3 = mesh.data.vertex_positions[shared_vertices[0]]
	var world_p1: Vector3 = mesh.data.vertex_positions[shared_vertices[1]]
	var solution: Dictionary = LevelMesh.solve_edge_hinge_similarity(
			mesh.project_native(face_b, world_p0), mesh.project_native(face_b, world_p1),
			mesh.get_uv(face_a, world_p0), mesh.get_uv(face_a, world_p1))
	solved_ok = solved_ok and solution.valid
	var before_solve_uv := _capture_uv(mesh.data)
	mesh.begin_transaction()
	if solution.valid:
		mesh.data.set_face_uv_transform(face_b, solution.transform)
	var reconciled_b: bool = solution.valid and mesh.reconcile_face_uv(face_b)
	var solve_diff: LevelMeshDiff = mesh.commit()
	solved_ok = solved_ok and reconciled_b and solve_diff != null
	for t in [0.0, 0.25, 0.5, 0.75, 1.0]:
		var point := world_p0.lerp(world_p1, t)
		solved_ok = solved_ok and mesh.get_uv(face_a, point).is_equal_approx(mesh.get_uv(face_b, point))
	var rejected: Dictionary = LevelMesh.solve_edge_hinge_similarity(Vector2.ONE, Vector2.ONE, Vector2.ZERO, Vector2.RIGHT)
	_check(solved_ok and not rejected.valid, "hinge_similarity_full_edge_and_guard")
	_check(mesh.revert_diff(solve_diff) and _uv_matches(mesh.data, before_solve_uv),
			"hinge_solver_undo_byte_exact")


func _check_explicit_read_adapter() -> void:
	var mesh := _make_box()
	var face_id := 1
	var loop_start: int = mesh.data.face_loop_starts[face_id]
	var authored := PackedVector2Array([
			Vector2(2, 3), Vector2(7, 2), Vector2(8, 9), Vector2(1, 8),
	])
	mesh.begin_transaction()
	var modes: PackedInt32Array = mesh.data.face_uv_modes
	modes[face_id] = LevelMeshData.UV_MODE_EXPLICIT
	mesh.data.face_uv_modes = modes
	var all_uvs: PackedVector2Array = mesh.data.loop_uv0
	for corner in range(authored.size()):
		all_uvs[loop_start + corner] = authored[corner]
	mesh.data.loop_uv0 = all_uvs
	var explicit_diff: LevelMeshDiff = mesh.commit()
	var corners := mesh.get_face_corner_positions(face_id)
	var explicit_ok := explicit_diff != null
	for corner in range(authored.size()):
		explicit_ok = explicit_ok and mesh.get_uv(face_id, corners[corner], loop_start + corner) == authored[corner]
	var midpoint: Vector3 = corners[0].lerp(corners[1], 0.5)
	explicit_ok = explicit_ok and mesh.get_uv(face_id, midpoint).is_equal_approx(authored[0].lerp(authored[1], 0.5))
	var face_vertices := mesh.get_face_corner_vertex_ids(face_id)
	var moved := PackedVector3Array()
	for vertex_id in face_vertices:
		moved.append(mesh.data.vertex_positions[vertex_id] + Vector3(0, 0, 1.25))
	var began := mesh.begin_transform_preview(face_vertices)
	var previewed := began and mesh.preview_transform_vertices(moved)
	var rigid_diff: LevelMeshDiff = mesh.commit_transform_preview() if previewed else null
	for corner in range(authored.size()):
		explicit_ok = explicit_ok and mesh.data.loop_uv0[loop_start + corner] == authored[corner]
	_check(explicit_ok and rigid_diff != null, "explicit_uv_read_and_rigid_preservation")


func _solve_affine_three(p0: Vector2, p1: Vector2, p2: Vector2,
		q0: Vector2, q1: Vector2, q2: Vector2) -> Transform2D:
	var edge1 := p1 - p0
	var edge2 := p2 - p0
	var target1 := q1 - q0
	var target2 := q2 - q0
	var determinant := edge1.cross(edge2)
	var m00 := (target1.x * edge2.y - target2.x * edge1.y) / determinant
	var m01 := (-target1.x * edge2.x + target2.x * edge1.x) / determinant
	var m10 := (target1.y * edge2.y - target2.y * edge1.y) / determinant
	var m11 := (-target1.y * edge2.x + target2.y * edge1.x) / determinant
	var offset := q0 - Vector2(m00 * p0.x + m01 * p0.y, m10 * p0.x + m11 * p0.y)
	return Transform2D(Vector2(m00, m10), Vector2(m01, m11), offset)


func _face_uvs(data: LevelMeshData, face_id: int) -> PackedVector2Array:
	var result := PackedVector2Array()
	var loop_start: int = data.face_loop_starts[face_id]
	for corner in range(data.face_loop_counts[face_id]):
		result.append(data.loop_uv0[loop_start + corner])
	return result


func _face_transform_words(data: LevelMeshData, face_id: int) -> PackedFloat32Array:
	var result := PackedFloat32Array()
	var words: PackedFloat32Array = data.face_uv_transforms
	for index in range(face_id * 6, face_id * 6 + 6):
		result.append(words[index])
	return result


func _capture_uv(data: LevelMeshData) -> Dictionary:
	var result := {}
	for property_name in UV_PROPERTIES:
		result[property_name] = data.get(property_name)
	return result


func _uv_matches(data: LevelMeshData, snapshot: Dictionary) -> bool:
	for property_name in UV_PROPERTIES:
		if data.get(property_name) != snapshot[property_name]:
			return false
	return true


func _check(condition: bool, label: String) -> void:
	if condition:
		print("PASS: " + label)
	else:
		failures.append(label)
		printerr("FAIL: " + label)
