extends SceneTree

const CYCLE_EPSILON := 0.00001

const UV_PROPERTIES: PackedStringArray = [
	"face_uv_modes",
	"face_uv_origins",
	"face_uv_tangents",
	"face_uv_transforms",
	"loop_uv0",
]

const DATA_ARRAY_PROPERTIES: PackedStringArray = [
	"vertex_positions",
	"vertex_alive",
	"vertex_generations",
	"vertex_free_ids",
	"edge_vertices",
	"edge_alive",
	"edge_generations",
	"edge_free_ids",
	"face_loop_starts",
	"face_loop_counts",
	"face_material_indices",
	"face_uv_modes",
	"face_uv_origins",
	"face_uv_tangents",
	"face_uv_transforms",
	"face_polygroup_ids",
	"face_flags",
	"face_alive",
	"face_generations",
	"face_free_ids",
	"loop_vertex_indices",
	"loop_uv0",
	"loop_colors",
	"loop_normals",
	"loop_alive",
	"loop_free_ids",
]

var failures: Array[String] = []


func _initialize() -> void:
	_check_determinism()
	_check_conforming_hinge_and_static_utility()
	_check_cycle_closure()
	_check_follow_quads_termination_and_spacing()
	_check_planar_and_square_frames()
	_check_mixed_state_regeneration()
	_check_undo_redo()
	_check_reject_paths()
	if failures.is_empty():
		print("UNWRAP_SMOKE_OK")
		quit(0)
	else:
		printerr("UNWRAP_SMOKE_FAIL: %s" % ", ".join(failures))
		quit(1)


func _check_determinism() -> void:
	for mode in ["square", "planar", "conforming", "follow_quads"]:
		var mesh := _make_box()
		var selection := PackedInt32Array([5, 3, 1, 4, 0, 2, 3])
		var first_diff := _run_mode(mesh, mode, selection)
		var first := _capture(mesh.data, UV_PROPERTIES)
		var first_seams: PackedInt32Array = mesh.get_last_unwrap_seam_edge_ids()
		var second_diff := _run_mode(mesh, mode, selection)
		_check(first_diff != null and second_diff != null and
				_matches(mesh.data, first, UV_PROPERTIES) and
				mesh.get_last_unwrap_seam_edge_ids() == first_seams,
				"determinism_%s_byte_identical" % mode)


func _check_conforming_hinge_and_static_utility() -> void:
	var mesh := _make_box(Transform3D.IDENTITY, Vector3(4, 4, 4))
	var selection := PackedInt32Array([1, 3, 0])
	var diff: LevelMeshDiff = mesh.unwrap_conforming(selection)
	var continuity_ok := diff != null and mesh.get_last_unwrap_error() == LevelMesh.UNWRAP_ERROR_NONE
	for pair in [PackedInt32Array([0, 3]), PackedInt32Array([3, 1])]:
		var shared_vertices := _shared_vertices(mesh, pair[0], pair[1])
		continuity_ok = continuity_ok and shared_vertices.size() == 2
		for vertex_id in shared_vertices:
			var loop_a := _face_loop_for_vertex(mesh.data, pair[0], vertex_id)
			var loop_b := _face_loop_for_vertex(mesh.data, pair[1], vertex_id)
			continuity_ok = (continuity_ok and loop_a >= 0 and loop_b >= 0 and
					mesh.data.loop_uv0[loop_a] == mesh.data.loop_uv0[loop_b])
	for face_id in selection:
		continuity_ok = continuity_ok and _face_edges_are_isometric(mesh, face_id)
	_check(continuity_ok, "conforming_hinge_bit_exact_and_isometric")

	var shared_edge := _shared_edge(mesh, 0, 3)
	var utility: Dictionary = LevelMesh.unfold_face_across_edge(mesh.data, 0, shared_edge, 3)
	_check(utility.valid and _candidate_matches_face(utility, mesh.data, 3, true),
			"static_hinge_unfold_matches_established_child")


func _check_cycle_closure() -> void:
	var mesh := _make_box()
	var diff: LevelMeshDiff = mesh.unwrap_conforming(PackedInt32Array([5, 4, 3, 2]))
	var winner_edge := mesh.get_adjacency().find_edge(1, 5)
	var loser_edge := mesh.get_adjacency().find_edge(2, 6)
	var winner: Dictionary = LevelMesh.unfold_face_across_edge(mesh.data, 4, winner_edge, 3)
	var loser: Dictionary = LevelMesh.unfold_face_across_edge(mesh.data, 5, loser_edge, 3)
	_check(diff != null and mesh.get_last_unwrap_seam_edge_ids() == PackedInt32Array([loser_edge]) and
			winner.valid and loser.valid and _candidate_matches_face(winner, mesh.data, 3, true) and
			not _candidate_matches_face(loser, mesh.data, 3, false),
			"cycle_closure_second_arrival_seam_first_visited_wins")


func _check_follow_quads_termination_and_spacing() -> void:
	var modes := [
		LevelMesh.UNWRAP_SPACING_LENGTH,
		LevelMesh.UNWRAP_SPACING_EVEN,
		LevelMesh.UNWRAP_SPACING_LENGTH_AVERAGE,
	]
	var expected := [
		[0.0, 1.0, 3.0, 6.0],
		[0.0, 1.0 / 3.0, 2.0 / 3.0, 1.0],
		[0.0, 2.0, 4.0, 6.0],
	]
	for mode_index in range(modes.size()):
		var mesh := _make_follow_strip()
		var untouched := mesh.data.duplicate_data()
		var diff: LevelMeshDiff = mesh.unwrap_follow_quads(
				PackedInt32Array([4, 3, 2, 1, 0]), modes[mode_index])
		var boundaries := _follow_strip_u_boundaries(mesh.data)
		var parametrization_ok: bool = boundaries.size() == expected[mode_index].size()
		for index in range(min(boundaries.size(), expected[mode_index].size())):
			parametrization_ok = parametrization_ok and is_equal_approx(
					boundaries[index], expected[mode_index][index])
		_check(diff != null and parametrization_ok and _face_rows_match(
				mesh.data, untouched, PackedInt32Array([3, 4])),
				"follow_quads_%s_spacing_and_triangle_termination" % ["length", "even", "length_average"][mode_index])


func _check_planar_and_square_frames() -> void:
	var planar := _make_coplanar_pair()
	var planar_diff: LevelMeshDiff = planar.unwrap_planar(PackedInt32Array([1, 0]))
	var shared := _shared_vertices(planar, 0, 1)
	var planar_ok := (planar_diff != null and shared.size() == 2 and
			planar.get_face_uv_origin(0) == planar.get_face_uv_origin(1) and
			planar.get_face_uv_tangent(0) == planar.get_face_uv_tangent(1) and
			planar.get_face_uv_transform(0) == planar.get_face_uv_transform(1))
	for vertex_id in shared:
		var point: Vector3 = planar.data.vertex_positions[vertex_id]
		planar_ok = planar_ok and planar.get_uv(0, point) == planar.get_uv(1, point)
	_check(planar_ok, "planar_shared_frame_and_edge_continuity")

	var square := _make_coplanar_pair()
	var square_diff: LevelMeshDiff = square.unwrap_square(PackedInt32Array([0, 1]))
	_check(square_diff != null and square.get_face_uv_origin(0) != square.get_face_uv_origin(1) and
			square.get_face_uv_transform(0) == Transform2D.IDENTITY and
			square.get_face_uv_transform(1) == Transform2D.IDENTITY,
			"square_independent_centroid_frames_identity_density_scale")


func _check_mixed_state_regeneration() -> void:
	for mode in ["square", "planar", "conforming", "follow_quads"]:
		var mesh := _make_box()
		var selection := PackedInt32Array([0, 2])
		_make_selection_mixed(mesh, selection[0], selection[1])
		var diff := _run_mode(mesh, mode, selection)
		var expected_mode := LevelMeshData.UV_MODE_PROJECTED if mode in ["square", "planar"] else LevelMeshData.UV_MODE_EXPLICIT
		var regenerated := diff != null
		for face_id in selection:
			regenerated = regenerated and mesh.get_face_uv_mode(face_id) == expected_mode
			if expected_mode == LevelMeshData.UV_MODE_EXPLICIT:
				regenerated = (regenerated and mesh.get_face_uv_origin(face_id) == Vector3.ZERO and
						mesh.get_face_uv_tangent(face_id) == Vector3.ZERO and
						mesh.get_face_uv_transform(face_id) == Transform2D.IDENTITY)
			regenerated = regenerated and _face_loop_uvs_are_immediate(mesh, face_id)
		_check(regenerated, "mixed_state_regenerated_%s" % mode)


func _check_undo_redo() -> void:
	for mode in ["square", "planar", "conforming", "follow_quads"]:
		var mesh := _make_box()
		var selection := PackedInt32Array([0, 2, 4])
		_make_selection_mixed(mesh, selection[0], selection[1])
		var before := _capture(mesh.data, UV_PROPERTIES)
		var diff := _run_mode(mesh, mode, selection)
		var after := _capture(mesh.data, UV_PROPERTIES)
		var undo_ok := diff != null and mesh.revert_diff(diff) and _matches(mesh.data, before, UV_PROPERTIES)
		var redo_ok := undo_ok and mesh.apply_diff(diff) and _matches(mesh.data, after, UV_PROPERTIES)
		_check(redo_ok, "undo_redo_%s_uv_columns_byte_identical" % mode)


func _check_reject_paths() -> void:
	for mode in ["square", "planar", "conforming", "follow_quads"]:
		var mesh := _make_box()
		var before := _capture(mesh.data, DATA_ARRAY_PROPERTIES)
		var rejected := _run_mode(mesh, mode, PackedInt32Array())
		_check(rejected == null and mesh.get_last_unwrap_error() == LevelMesh.UNWRAP_ERROR_EMPTY_SELECTION and
				_matches(mesh.data, before, DATA_ARRAY_PROPERTIES),
				"typed_empty_selection_rejection_%s" % mode)

	var nonquad := _make_follow_strip()
	var nonquad_before := _capture(nonquad.data, DATA_ARRAY_PROPERTIES)
	var nonquad_diff: LevelMeshDiff = nonquad.unwrap_follow_quads(
			PackedInt32Array([4, 3]), LevelMesh.UNWRAP_SPACING_LENGTH)
	_check(nonquad_diff == null and nonquad.get_last_unwrap_error() == LevelMesh.UNWRAP_ERROR_INVALID_SEED and
			_matches(nonquad.data, nonquad_before, DATA_ARRAY_PROPERTIES),
			"typed_nonquad_seed_rejection_atomic")

	var nonmanifold := _make_nonmanifold_fan()
	var nonmanifold_before := _capture(nonmanifold.data, DATA_ARRAY_PROPERTIES)
	var nonmanifold_diff: LevelMeshDiff = nonmanifold.unwrap_conforming(PackedInt32Array([0, 1]))
	_check(nonmanifold_diff == null and
			nonmanifold.get_last_unwrap_error() == LevelMesh.UNWRAP_ERROR_NON_MANIFOLD_EDGE and
			_matches(nonmanifold.data, nonmanifold_before, DATA_ARRAY_PROPERTIES),
			"typed_nonmanifold_internal_edge_rejection_atomic")


func _run_mode(mesh: LevelMesh, mode: String, face_ids: PackedInt32Array) -> LevelMeshDiff:
	match mode:
		"square":
			return mesh.unwrap_square(face_ids)
		"planar":
			return mesh.unwrap_planar(face_ids)
		"conforming":
			return mesh.unwrap_conforming(face_ids)
		"follow_quads":
			return mesh.unwrap_follow_quads(face_ids, LevelMesh.UNWRAP_SPACING_LENGTH)
	return null


func _make_box(frame := Transform3D.IDENTITY, size := Vector3(4, 4, 4)) -> LevelMesh:
	var mesh := LevelMesh.new()
	mesh.begin_transaction()
	if not mesh.create_box(frame, size, 0):
		mesh.rollback()
		_check(false, "fixture_box_create")
		return mesh
	if mesh.commit() == null:
		_check(false, "fixture_box_commit")
	return mesh


func _make_coplanar_pair() -> LevelMesh:
	return _make_custom_mesh([
		Vector3(0, 0, 0), Vector3(1, 0, 0), Vector3(2, 0, 0),
		Vector3(0, 1, 0), Vector3(1, 1, 0), Vector3(2, 1, 0),
	], [
		PackedInt32Array([0, 1, 4, 3]),
		PackedInt32Array([1, 2, 5, 4]),
	])


func _make_follow_strip() -> LevelMesh:
	return _make_custom_mesh([
		Vector3(0, 0, 0), Vector3(1, 0, 0), Vector3(3, 0, 0), Vector3(6, 0, 0),
		Vector3(0, 1, 0), Vector3(1, 1, 0), Vector3(3, 1, 0), Vector3(6, 1, 0),
		Vector3(7.5, 0.5, 0), Vector3(8, 1.5, 0), Vector3(6.5, 2, 0),
	], [
		PackedInt32Array([0, 1, 5, 4]),
		PackedInt32Array([1, 2, 6, 5]),
		PackedInt32Array([2, 3, 7, 6]),
		PackedInt32Array([3, 8, 7]),
		PackedInt32Array([7, 8, 9, 10]),
	])


func _make_nonmanifold_fan() -> LevelMesh:
	return _make_custom_mesh([
		Vector3(0, 0, 0), Vector3(1, 0, 0), Vector3(0, 1, 0),
		Vector3(0, 0, 1), Vector3(0, -1, 0),
	], [
		PackedInt32Array([0, 1, 2]),
		PackedInt32Array([1, 0, 3]),
		PackedInt32Array([0, 1, 4]),
	])


func _make_custom_mesh(vertex_values: Array, face_values: Array) -> LevelMesh:
	var data := LevelMeshData.new()
	data.vertex_positions = PackedVector3Array(vertex_values)
	var vertex_alive := PackedByteArray()
	for _vertex in vertex_values:
		vertex_alive.append(1)
	data.vertex_alive = vertex_alive

	var edge_lookup := {}
	var edge_vertices := PackedInt32Array()
	var edge_alive := PackedByteArray()
	for face_vertices: PackedInt32Array in face_values:
		for corner in range(face_vertices.size()):
			var vertex_a: int = face_vertices[corner]
			var vertex_b: int = face_vertices[(corner + 1) % face_vertices.size()]
			var key := "%d:%d" % [min(vertex_a, vertex_b), max(vertex_a, vertex_b)]
			if not edge_lookup.has(key):
				edge_lookup[key] = edge_alive.size()
				edge_vertices.append(vertex_a)
				edge_vertices.append(vertex_b)
				edge_alive.append(1)
	data.edge_vertices = edge_vertices
	data.edge_alive = edge_alive

	var face_loop_starts := PackedInt32Array()
	var face_loop_counts := PackedInt32Array()
	var face_material_indices := PackedInt32Array()
	var face_uv_modes := PackedInt32Array()
	var face_uv_origins := PackedVector3Array()
	var face_uv_tangents := PackedVector3Array()
	var face_uv_transforms := PackedFloat32Array()
	var face_polygroups := PackedInt32Array()
	var face_flags := PackedInt32Array()
	var face_alive := PackedByteArray()
	var loop_vertex_indices := PackedInt32Array()
	var loop_uv0 := PackedVector2Array()
	var loop_colors := PackedColorArray()
	var loop_normals := PackedVector3Array()
	var loop_alive := PackedByteArray()
	for face_id in range(face_values.size()):
		var face_vertices: PackedInt32Array = face_values[face_id]
		face_loop_starts.append(loop_vertex_indices.size())
		face_loop_counts.append(face_vertices.size())
		face_material_indices.append(0)
		face_uv_modes.append(LevelMeshData.UV_MODE_PROJECTED)
		face_uv_origins.append(Vector3(10 + face_id, 20 + face_id, 30 + face_id))
		var tangent: Vector3 = (vertex_values[face_vertices[1]] - vertex_values[face_vertices[0]]).normalized()
		face_uv_tangents.append(tangent)
		var scale := 1.0 + face_id * 0.125
		face_uv_transforms.append_array(PackedFloat32Array([
			scale, 0, 0, scale, 40 + face_id, -40 - face_id,
		]))
		face_polygroups.append(face_id)
		face_flags.append(LevelMeshData.FACE_FLAG_TEXTURE_LOCK)
		face_alive.append(1)
		for vertex_id in face_vertices:
			loop_vertex_indices.append(vertex_id)
			loop_uv0.append(Vector2(100 + loop_vertex_indices.size(), -100 - loop_vertex_indices.size()))
			loop_colors.append(Color.WHITE)
			loop_normals.append(Vector3(9, 8, 7))
			loop_alive.append(1)
	data.face_loop_starts = face_loop_starts
	data.face_loop_counts = face_loop_counts
	data.face_material_indices = face_material_indices
	data.face_uv_modes = face_uv_modes
	data.face_uv_origins = face_uv_origins
	data.face_uv_tangents = face_uv_tangents
	data.face_uv_transforms = face_uv_transforms
	data.face_polygroup_ids = face_polygroups
	data.face_flags = face_flags
	data.face_alive = face_alive
	data.loop_vertex_indices = loop_vertex_indices
	data.loop_uv0 = loop_uv0
	data.loop_colors = loop_colors
	data.loop_normals = loop_normals
	data.loop_alive = loop_alive
	var mesh := LevelMesh.new()
	mesh.data = data
	return mesh


func _make_selection_mixed(mesh: LevelMesh, projected_face: int, explicit_face: int) -> void:
	mesh.begin_transaction()
	mesh.data.set_face_uv_mode(projected_face, LevelMeshData.UV_MODE_PROJECTED)
	mesh.data.set_face_uv_origin(projected_face, Vector3(17, 19, 23))
	mesh.data.set_face_uv_tangent(projected_face, Vector3(-1, 0, 0))
	mesh.data.set_face_uv_transform(projected_face, Transform2D(
			Vector2(1.5, 0.25), Vector2(-0.125, 0.75), Vector2(31, -47)))
	mesh.reconcile_face_uv(projected_face)
	mesh.data.set_face_uv_mode(explicit_face, LevelMeshData.UV_MODE_EXPLICIT)
	mesh.data.set_face_uv_origin(explicit_face, Vector3(101, 102, 103))
	mesh.data.set_face_uv_tangent(explicit_face, Vector3(7, 8, 9))
	mesh.data.set_face_uv_transform(explicit_face, Transform2D(
			Vector2(3, 1), Vector2(2, 4), Vector2(99, -99)))
	var uvs: PackedVector2Array = mesh.data.loop_uv0
	var loop_start: int = mesh.data.face_loop_starts[explicit_face]
	for corner in range(mesh.data.face_loop_counts[explicit_face]):
		uvs[loop_start + corner] = Vector2(200 + corner * 3, -300 - corner * 5)
	mesh.data.loop_uv0 = uvs
	mesh.commit()


func _follow_strip_u_boundaries(data: LevelMeshData) -> Array[float]:
	return [
		data.loop_uv0[data.face_loop_starts[0]].x,
		data.loop_uv0[data.face_loop_starts[0] + 1].x,
		data.loop_uv0[data.face_loop_starts[1] + 1].x,
		data.loop_uv0[data.face_loop_starts[2] + 1].x,
	]


func _shared_vertices(mesh: LevelMesh, face_a: int, face_b: int) -> PackedInt32Array:
	var result := PackedInt32Array()
	var vertices_b := mesh.get_face_corner_vertex_ids(face_b)
	for vertex_id in mesh.get_face_corner_vertex_ids(face_a):
		if vertices_b.has(vertex_id):
			result.append(vertex_id)
	return result


func _shared_edge(mesh: LevelMesh, face_a: int, face_b: int) -> int:
	for edge_id in mesh.get_adjacency().get_face_edges(face_a):
		if mesh.get_adjacency().get_edge_faces(edge_id).has(face_b):
			return edge_id
	return -1


func _face_loop_for_vertex(data: LevelMeshData, face_id: int, vertex_id: int) -> int:
	var loop_start: int = data.face_loop_starts[face_id]
	for corner in range(data.face_loop_counts[face_id]):
		if data.loop_vertex_indices[loop_start + corner] == vertex_id:
			return loop_start + corner
	return -1


func _face_edges_are_isometric(mesh: LevelMesh, face_id: int) -> bool:
	var data := mesh.data
	var loop_start: int = data.face_loop_starts[face_id]
	var loop_count: int = data.face_loop_counts[face_id]
	for corner in range(loop_count):
		var next_corner := (corner + 1) % loop_count
		var loop_a := loop_start + corner
		var loop_b := loop_start + next_corner
		var position_a: Vector3 = data.vertex_positions[data.loop_vertex_indices[loop_a]]
		var position_b: Vector3 = data.vertex_positions[data.loop_vertex_indices[loop_b]]
		if not is_equal_approx(position_a.distance_to(position_b),
				data.loop_uv0[loop_a].distance_to(data.loop_uv0[loop_b])):
			return false
	return true


func _candidate_matches_face(candidate: Dictionary, data: LevelMeshData, face_id: int,
		require_bit_exact: bool) -> bool:
	var loop_ids: PackedInt32Array = candidate.loop_ids
	var uvs: PackedVector2Array = candidate.uvs
	if loop_ids.size() != data.face_loop_counts[face_id] or loop_ids.size() != uvs.size():
		return false
	var all_match := true
	for index in range(loop_ids.size()):
		if require_bit_exact:
			all_match = all_match and data.loop_uv0[loop_ids[index]] == uvs[index]
		else:
			all_match = all_match and data.loop_uv0[loop_ids[index]].distance_to(uvs[index]) <= CYCLE_EPSILON
	return all_match


func _face_loop_uvs_are_immediate(mesh: LevelMesh, face_id: int) -> bool:
	var data := mesh.data
	var loop_start: int = data.face_loop_starts[face_id]
	for corner in range(data.face_loop_counts[face_id]):
		var loop_id := loop_start + corner
		var point: Vector3 = data.vertex_positions[data.loop_vertex_indices[loop_id]]
		if data.loop_uv0[loop_id] != mesh.get_uv(face_id, point, loop_id):
			return false
	return true


func _face_rows_match(data: LevelMeshData, snapshot: LevelMeshData,
		face_ids: PackedInt32Array) -> bool:
	for face_id in face_ids:
		if data.face_material_indices[face_id] != snapshot.face_material_indices[face_id] or \
				data.face_uv_modes[face_id] != snapshot.face_uv_modes[face_id] or \
				data.face_uv_origins[face_id] != snapshot.face_uv_origins[face_id] or \
				data.face_uv_tangents[face_id] != snapshot.face_uv_tangents[face_id] or \
				data.face_polygroup_ids[face_id] != snapshot.face_polygroup_ids[face_id] or \
				data.face_flags[face_id] != snapshot.face_flags[face_id] or \
				data.face_alive[face_id] != snapshot.face_alive[face_id]:
			return false
		for word in range(face_id * 6, face_id * 6 + 6):
			if data.face_uv_transforms[word] != snapshot.face_uv_transforms[word]:
				return false
		var loop_start: int = data.face_loop_starts[face_id]
		for corner in range(data.face_loop_counts[face_id]):
			var loop_id := loop_start + corner
			if data.loop_vertex_indices[loop_id] != snapshot.loop_vertex_indices[loop_id] or \
					data.loop_uv0[loop_id] != snapshot.loop_uv0[loop_id] or \
					data.loop_colors[loop_id] != snapshot.loop_colors[loop_id] or \
					data.loop_normals[loop_id] != snapshot.loop_normals[loop_id] or \
					data.loop_alive[loop_id] != snapshot.loop_alive[loop_id]:
				return false
	return true


func _capture(data: LevelMeshData, properties: PackedStringArray) -> Dictionary:
	var snapshot := {}
	for property_name in properties:
		snapshot[property_name] = data.get(property_name)
	return snapshot


func _matches(data: LevelMeshData, snapshot: Dictionary, properties: PackedStringArray) -> bool:
	for property_name in properties:
		if data.get(property_name) != snapshot[property_name]:
			return false
	return true


func _check(condition: bool, label: String) -> void:
	if condition:
		print("PASS: " + label)
	else:
		failures.append(label)
		printerr("FAIL: " + label)
