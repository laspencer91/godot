extends SceneTree

const MATERIAL_A := "res://material_a.tres"
const MATERIAL_B := "res://material_b.tres"
const DATA_PROPERTIES: PackedStringArray = [
	"material_paths",
	"vertex_positions", "vertex_alive", "vertex_generations", "vertex_free_ids",
	"edge_vertices", "edge_alive", "edge_generations", "edge_free_ids",
	"face_loop_starts", "face_loop_counts", "face_material_indices",
	"face_uv_modes", "face_uv_origins", "face_uv_tangents", "face_uv_transforms",
	"face_polygroup_ids", "face_flags", "face_alive", "face_generations", "face_free_ids",
	"loop_vertex_indices", "loop_uv0", "loop_colors", "loop_normals", "loop_alive", "loop_free_ids",
]

var failures: Array[String] = []


func _initialize() -> void:
	_check_wrap_continuity()
	_check_flow_chain()
	_check_apply_material_table_and_baker()
	_check_lift_apply_capture_contract()
	_check_justify_fit_and_panel_ops()
	if failures.is_empty():
		print("FACE_TEXTURE_SMOKE_OK")
		quit(0)
	else:
		printerr("FACE_TEXTURE_SMOKE_FAIL: %s" % ", ".join(failures))
		quit(1)


func _make_strip(face_count: int) -> LevelMesh:
	var data := LevelMeshData.new()
	var positions := PackedVector3Array()
	var vertex_alive := PackedByteArray()
	for boundary in range(face_count + 1):
		var angle := (float(boundary) - float(face_count) * 0.5) * 0.24
		var center := Vector3(sin(angle) * 5.0, 0.0, cos(angle) * 5.0)
		positions.append(center + Vector3(0, -1, 0))
		positions.append(center + Vector3(0, 1, 0))
		vertex_alive.append(1)
		vertex_alive.append(1)
	data.vertex_positions = positions
	data.vertex_alive = vertex_alive

	var edge_vertices := PackedInt32Array()
	var edge_alive := PackedByteArray()
	for boundary in range(face_count + 1):
		edge_vertices.append(boundary * 2)
		edge_vertices.append(boundary * 2 + 1)
		edge_alive.append(1)
	for face_id in range(face_count):
		edge_vertices.append(face_id * 2)
		edge_vertices.append((face_id + 1) * 2)
		edge_alive.append(1)
		edge_vertices.append(face_id * 2 + 1)
		edge_vertices.append((face_id + 1) * 2 + 1)
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
	var face_polygroup_ids := PackedInt32Array()
	var face_flags := PackedInt32Array()
	var face_alive := PackedByteArray()
	var loop_vertices := PackedInt32Array()
	var loop_uv0 := PackedVector2Array()
	var loop_colors := PackedColorArray()
	var loop_normals := PackedVector3Array()
	var loop_alive := PackedByteArray()
	for face_id in range(face_count):
		face_loop_starts.append(face_id * 4)
		face_loop_counts.append(4)
		face_material_indices.append(-1)
		face_uv_modes.append(LevelMeshData.UV_MODE_PROJECTED)
		face_uv_origins.append(Vector3.ZERO)
		face_uv_tangents.append(Vector3.ZERO)
		face_uv_transforms.append_array(PackedFloat32Array([1, 0, 0, 1, 0, 0]))
		face_polygroup_ids.append(face_id)
		face_flags.append(LevelMeshData.FACE_FLAG_TEXTURE_LOCK)
		face_alive.append(1)
		for vertex_id in [face_id * 2, (face_id + 1) * 2, (face_id + 1) * 2 + 1, face_id * 2 + 1]:
			loop_vertices.append(vertex_id)
			loop_uv0.append(Vector2.ZERO)
			loop_colors.append(Color.WHITE)
			loop_normals.append(Vector3.ZERO)
			loop_alive.append(1)
	data.face_loop_starts = face_loop_starts
	data.face_loop_counts = face_loop_counts
	data.face_material_indices = face_material_indices
	data.face_uv_modes = face_uv_modes
	data.face_uv_origins = face_uv_origins
	data.face_uv_tangents = face_uv_tangents
	data.face_uv_transforms = face_uv_transforms
	data.face_polygroup_ids = face_polygroup_ids
	data.face_flags = face_flags
	data.face_alive = face_alive
	data.loop_vertex_indices = loop_vertices
	data.loop_uv0 = loop_uv0
	data.loop_colors = loop_colors
	data.loop_normals = loop_normals
	data.loop_alive = loop_alive

	var mesh := LevelMesh.new()
	mesh.data = data
	var faces := PackedInt32Array()
	for face_id in range(face_count):
		faces.append(face_id)
	var align_diff: LevelMeshDiff = mesh.align_faces_to_grid(faces)
	_check(align_diff != null, "strip_align_%d" % face_count)
	return mesh


func _similarity(angle: float, scale: float, offset: Vector2) -> Transform2D:
	return Transform2D(
			Vector2(cos(angle), sin(angle)) * scale,
			Vector2(-sin(angle), cos(angle)) * scale,
			offset)


func _set_transform(mesh: LevelMesh, face_id: int, transform: Transform2D) -> LevelMeshDiff:
	mesh.begin_transaction()
	mesh.data.set_face_uv_transform(face_id, transform)
	var reconciled := mesh.reconcile_face_uv(face_id)
	var diff: LevelMeshDiff = mesh.commit()
	_check(reconciled and diff != null, "set_transform_%d" % face_id)
	return diff


func _check_edge_continuity(mesh: LevelMesh, face_a: int, face_b: int, vertex_a: int, vertex_b: int) -> bool:
	var world_a: Vector3 = mesh.data.vertex_positions[vertex_a]
	var world_b: Vector3 = mesh.data.vertex_positions[vertex_b]
	for t in [0.0, 0.25, 0.5, 0.75, 1.0]:
		var point := world_a.lerp(world_b, t)
		if not mesh.get_uv(face_a, point).is_equal_approx(mesh.get_uv(face_b, point)):
			return false
	return true


func _check_wrap_continuity() -> void:
	var mesh := _make_strip(2)
	_set_transform(mesh, 0, _similarity(0.41, 1.7, Vector2(2.3, -4.1)))
	var before := _snapshot(mesh.data)
	var diff: LevelMeshDiff = mesh.wrap_faces(0, PackedInt32Array([1]))
	_check(diff != null and mesh.get_face_uv_mode(1) == LevelMeshData.UV_MODE_PROJECTED and
			_check_edge_continuity(mesh, 0, 1, 2, 3), "wrap_dihedral_full_edge_continuity")
	_check(mesh.revert_diff(diff) and _matches(mesh.data, before), "wrap_byte_exact_undo")


func _check_flow_chain() -> void:
	var mesh := _make_strip(6)
	_set_transform(mesh, 0, _similarity(-0.63, 1.45, Vector2(-3.25, 5.5)))
	var before := _snapshot(mesh.data)
	var chain := PackedInt32Array([0, 1, 2, 3, 4, 5])
	var diff: LevelMeshDiff = mesh.flow_faces(chain)
	var continuous := diff != null
	for boundary in range(1, 6):
		continuous = continuous and _check_edge_continuity(mesh, boundary - 1, boundary, boundary * 2, boundary * 2 + 1)
	var first_scale := mesh.get_face_uv_transform(0).x.length()
	var last_scale := mesh.get_face_uv_transform(5).x.length()
	_check(continuous, "flow_six_face_internal_edge_continuity")
	_check(is_equal_approx(first_scale, last_scale), "flow_scale_drift_bound")
	_check(diff != null and mesh.revert_diff(diff) and _matches(mesh.data, before),
			"flow_single_diff_byte_exact_undo")


func _check_apply_material_table_and_baker() -> void:
	var mesh := _make_strip(2)
	var before := _snapshot(mesh.data)
	var apply_a: LevelMeshDiff = mesh.apply_face_texture(PackedInt32Array([0]), MATERIAL_A)
	var apply_b: LevelMeshDiff = mesh.apply_face_texture(PackedInt32Array([1]), MATERIAL_B)
	var table_before_dedup: PackedStringArray = mesh.data.material_paths
	var duplicate_index := mesh.data.intern_material_path(MATERIAL_A)
	_check(apply_a != null and apply_b != null and mesh.data.material_paths == PackedStringArray([MATERIAL_A, MATERIAL_B]) and
			mesh.data.face_material_indices == PackedInt32Array([0, 1]) and duplicate_index == 0 and
			mesh.data.material_paths == table_before_dedup, "apply_material_table_indices_and_dedup")
	var duplicate: LevelMeshData = mesh.data.duplicate_data()
	_check(duplicate.material_paths == mesh.data.material_paths, "material_table_duplicate_coverage")
	var serialized_path := "res://.godot/face_texture_material_table.tres"
	DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path("res://.godot"))
	var save_error := ResourceSaver.save(mesh.data, serialized_path)
	var reloaded := ResourceLoader.load(serialized_path, "LevelMeshData") as LevelMeshData
	_check(save_error == OK and reloaded != null and reloaded.material_paths == mesh.data.material_paths and
			reloaded.face_material_indices == mesh.data.face_material_indices,
			"material_table_serialization_round_trip")
	DirAccess.remove_absolute(ProjectSettings.globalize_path(serialized_path))
	var baked: ArrayMesh = LevelMeshBaker.new().bake(mesh.data)
	var baked_ok := baked != null and baked.get_surface_count() == 2
	if baked_ok:
		baked_ok = baked.surface_get_material(0) != null and baked.surface_get_material(1) != null and \
				baked.surface_get_material(0).resource_path == MATERIAL_A and \
				baked.surface_get_material(1).resource_path == MATERIAL_B
	_check(baked_ok, "baker_assigns_material_per_surface_group")
	_check(mesh.revert_diff(apply_b) and mesh.data.material_paths == PackedStringArray([MATERIAL_A]) and
			mesh.revert_diff(apply_a) and _matches(mesh.data, before), "material_table_diff_byte_exact_undo")


func _check_lift_apply_capture_contract() -> void:
	var mesh := _make_strip(2)
	var apply_source: LevelMeshDiff = mesh.apply_face_texture(PackedInt32Array([0]), MATERIAL_A)
	_set_transform(mesh, 0, _similarity(0.27, 0.82, Vector2(7.5, -2.0)))
	var capture: Dictionary = mesh.capture_face_texture(0)
	var apply_capture: LevelMeshDiff = mesh.apply_face_texture(PackedInt32Array([1]), MATERIAL_A, capture)
	_check(apply_source != null and apply_capture != null and capture.valid and capture.has_mapping and
			mesh.data.face_material_indices[1] == mesh.data.face_material_indices[0] and
			mesh.get_face_uv_mode(1) == mesh.get_face_uv_mode(0) and
			mesh.get_face_uv_origin(1) == mesh.get_face_uv_origin(0) and
			mesh.get_face_uv_tangent(1) == mesh.get_face_uv_tangent(0) and
			mesh.get_face_uv_transform(1) == mesh.get_face_uv_transform(0),
			"projected_lift_apply_full_state_round_trip")

	var source_b: LevelMeshDiff = mesh.apply_face_texture(PackedInt32Array([0]), MATERIAL_B)
	mesh.begin_transaction()
	var modes: PackedInt32Array = mesh.data.face_uv_modes
	modes[0] = LevelMeshData.UV_MODE_EXPLICIT
	mesh.data.face_uv_modes = modes
	var explicit_uvs: PackedVector2Array = mesh.data.loop_uv0
	var loop_start: int = mesh.data.face_loop_starts[0]
	for corner in range(4):
		explicit_uvs[loop_start + corner] = Vector2(corner * 0.7, corner * corner * 0.2)
	mesh.data.loop_uv0 = explicit_uvs
	var explicit_setup: LevelMeshDiff = mesh.commit()
	var explicit_capture: Dictionary = mesh.capture_face_texture(0)
	var destination_mapping := _face_mapping(mesh, 1)
	var explicit_apply: LevelMeshDiff = mesh.apply_face_texture(PackedInt32Array([1]), MATERIAL_B, explicit_capture)
	_check(source_b != null and explicit_setup != null and explicit_apply != null and explicit_capture.valid and
			not explicit_capture.has_mapping and explicit_capture.uv_transform == Transform2D.IDENTITY and
			mesh.data.face_material_indices[1] == mesh.data.face_material_indices[0] and
			_face_mapping(mesh, 1) == destination_mapping, "explicit_lift_degrades_to_material_only")


func _check_justify_fit_and_panel_ops() -> void:
	var mesh := _make_strip(1)
	_set_transform(mesh, 0, _similarity(deg_to_rad(37.0), 1.35, Vector2(3.7, -5.2)))
	var justify: LevelMeshDiff = mesh.modify_face_uv(PackedInt32Array([0]), LevelMesh.TEXTURE_MODIFY_JUSTIFY_LEFT, Vector2.ONE)
	var justified_bounds := _uv_bounds(mesh, 0)
	_check(justify != null and is_zero_approx(justified_bounds.position.x), "justify_left_rotation_invariant")
	var fit: LevelMeshDiff = mesh.modify_face_uv(PackedInt32Array([0]), LevelMesh.TEXTURE_MODIFY_FIT, Vector2.ONE)
	var fit_bounds := _uv_bounds(mesh, 0)
	_check(fit != null and fit_bounds.size.is_equal_approx(Vector2.ONE) and
			fit_bounds.position.is_equal_approx(Vector2.ZERO), "fit_covers_normalized_material_footprint")

	var cases: Array[Array] = [
		[LevelMesh.TEXTURE_MODIFY_SHIFT, Vector2(0.125, 0)],
		[LevelMesh.TEXTURE_MODIFY_SCALE, Vector2(1.189207115, 1)],
		[LevelMesh.TEXTURE_MODIFY_ROTATE, Vector2(deg_to_rad(15.0), 0)],
		[LevelMesh.TEXTURE_MODIFY_JUSTIFY_CENTER, Vector2.ONE],
		[LevelMesh.TEXTURE_MODIFY_FLIP_HORIZONTAL, Vector2.ONE],
		[LevelMesh.TEXTURE_MODIFY_FLIP_VERTICAL, Vector2.ONE],
	]
	var every_press_one_diff := true
	for operation in cases:
		var before := _snapshot(mesh.data)
		var diff: LevelMeshDiff = mesh.modify_face_uv(PackedInt32Array([0]), operation[0], operation[1])
		every_press_one_diff = every_press_one_diff and diff != null and mesh.revert_diff(diff) and _matches(mesh.data, before)
	_check(every_press_one_diff, "panel_operations_each_one_diff_and_exact_undo")
	var origin_before := mesh.get_face_uv_transform(0).origin
	var shift: LevelMeshDiff = mesh.modify_face_uv(PackedInt32Array([0]), LevelMesh.TEXTURE_MODIFY_SHIFT, Vector2(0.125, -0.25))
	var origin_after := mesh.get_face_uv_transform(0).origin
	_check(shift != null and (origin_after - origin_before).is_equal_approx(Vector2(0.125, -0.25)),
			"panel_shift_is_current_uv_space")


func _uv_bounds(mesh: LevelMesh, face_id: int) -> Rect2:
	var points := mesh.get_face_corner_positions(face_id)
	var minimum := Vector2(INF, INF)
	var maximum := Vector2(-INF, -INF)
	for point in points:
		var uv := mesh.get_uv(face_id, point)
		minimum.x = min(minimum.x, uv.x)
		minimum.y = min(minimum.y, uv.y)
		maximum.x = max(maximum.x, uv.x)
		maximum.y = max(maximum.y, uv.y)
	return Rect2(minimum, maximum - minimum)


func _face_mapping(mesh: LevelMesh, face_id: int) -> Array:
	return [mesh.get_face_uv_mode(face_id), mesh.get_face_uv_origin(face_id),
			mesh.get_face_uv_tangent(face_id), mesh.get_face_uv_transform(face_id)]


func _snapshot(data: LevelMeshData) -> Dictionary:
	var result := {}
	for property_name in DATA_PROPERTIES:
		result[property_name] = data.get(property_name)
	return result


func _matches(data: LevelMeshData, snapshot: Dictionary) -> bool:
	for property_name in DATA_PROPERTIES:
		if data.get(property_name) != snapshot[property_name]:
			return false
	return true


func _check(condition: bool, label: String) -> void:
	if condition:
		print("PASS: " + label)
	else:
		failures.append(label)
		printerr("FAIL: " + label)
