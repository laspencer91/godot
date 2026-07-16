extends SceneTree

const FIXTURE_SCRIPT: Script = preload("res://hotspot_fitter_fixture.gd")

const DATA_PROPERTIES: PackedStringArray = [
	"material_paths",
	"vertex_positions", "vertex_alive", "vertex_generations", "vertex_free_ids",
	"edge_vertices", "edge_alive", "edge_generations", "edge_free_ids",
	"face_loop_starts", "face_loop_counts", "face_material_indices",
	"face_uv_modes", "face_uv_origins", "face_uv_tangents", "face_uv_transforms",
	"face_hotspot_patch_names", "face_polygroup_ids", "face_flags", "face_alive",
	"face_generations", "face_free_ids",
	"loop_vertex_indices", "loop_uv0", "loop_colors", "loop_normals", "loop_alive", "loop_free_ids",
]

var failures: Array[String] = []
var fixture: RefCounted = FIXTURE_SCRIPT.new()


func _initialize() -> void:
	var atlas := _make_atlas(false)
	_check_seeded_determinism(atlas)
	_check_density_and_tiling(atlas)
	_check_partition_and_unwind(atlas)
	_check_sticky_refit_and_undo(atlas)
	if failures.is_empty():
		print("HOTSPOT_FITTER_SMOKE_OK")
		quit(0)
	else:
		printerr("HOTSPOT_FITTER_SMOKE_FAIL: %s" % ", ".join(failures))
		quit(1)


func _make_patch(name: StringName, rect: Rect2) -> HotspotPatch:
	var patch := HotspotPatch.new()
	patch.patch_name = name
	patch.rect_uv = rect
	patch.allow_rotation = true
	patch.allow_mirror_x = true
	patch.allow_mirror_y = true
	patch.allow_tiling = true
	patch.tiling_axis = HotspotPatch.TILING_AXIS_U
	return patch


func _make_atlas(disallow_random: bool) -> HotspotAtlas:
	return fixture.call("make_atlas", disallow_random) as HotspotAtlas


func _make_mesh(positions: PackedVector3Array, faces: Array) -> LevelMesh:
	var data := LevelMeshData.new()
	data.material_paths = PackedStringArray(["res://fixture_material.tres"])
	data.vertex_positions = positions
	var vertex_alive := PackedByteArray()
	vertex_alive.resize(positions.size())
	vertex_alive.fill(1)
	data.vertex_alive = vertex_alive

	var edge_map := {}
	var edge_vertices := PackedInt32Array()
	var edge_alive := PackedByteArray()
	for face: PackedInt32Array in faces:
		for corner in range(face.size()):
			var vertex_a: int = face[corner]
			var vertex_b: int = face[(corner + 1) % face.size()]
			var key := "%d:%d" % [min(vertex_a, vertex_b), max(vertex_a, vertex_b)]
			if edge_map.has(key):
				continue
			edge_map[key] = edge_alive.size()
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
	var face_polygroup_ids := PackedInt32Array()
	var face_flags := PackedInt32Array()
	var face_alive := PackedByteArray()
	var loop_vertices := PackedInt32Array()
	var loop_uv0 := PackedVector2Array()
	var loop_colors := PackedColorArray()
	var loop_normals := PackedVector3Array()
	var loop_alive := PackedByteArray()
	for face_id in range(faces.size()):
		var face: PackedInt32Array = faces[face_id]
		face_loop_starts.append(loop_vertices.size())
		face_loop_counts.append(face.size())
		face_material_indices.append(0)
		face_uv_modes.append(LevelMeshData.UV_MODE_PROJECTED)
		face_uv_origins.append(Vector3.ZERO)
		face_uv_tangents.append(Vector3(1, 0, 0))
		face_uv_transforms.append_array(PackedFloat32Array([1, 0, 0, 1, 0, 0]))
		face_polygroup_ids.append(face_id)
		face_flags.append(LevelMeshData.FACE_FLAG_TEXTURE_LOCK)
		face_alive.append(1)
		for vertex_id in face:
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
	return mesh


func _make_flat_wall(face_count: int, face_width := 4.0, height := 2.0) -> LevelMesh:
	return fixture.call("make_flat_wall", face_count, face_width, height) as LevelMesh


func _make_corner_wall() -> LevelMesh:
	return fixture.call("make_corner_wall") as LevelMesh


func _make_wall_floor() -> LevelMesh:
	return fixture.call("make_wall_floor") as LevelMesh


func _faces(count: int) -> PackedInt32Array:
	return fixture.call("faces", count) as PackedInt32Array


func _fit(mesh: LevelMesh, atlas: HotspotAtlas, faces: PackedInt32Array,
		island_mode: int, seed := 0x12345678) -> Dictionary:
	return HotspotFitter.new().fit(faces, mesh, atlas, island_mode, seed)


func _check_seeded_determinism(atlas: HotspotAtlas) -> void:
	var mesh := _make_flat_wall(5)
	var selected := _faces(5)
	var first := _fit(mesh, atlas, selected, HotspotFitter.ISLAND_GROUPED)
	var second := _fit(mesh, atlas, selected, HotspotFitter.ISLAND_GROUPED)
	_check(first.ok and second.ok and var_to_bytes(first) == var_to_bytes(second),
			"same mesh_atlas_seed_is_byte_identical")
	_check(not JSON.stringify(first.diagnostics).is_empty(), "diagnostics_are_json_dumpable")

	var deterministic_atlas := _make_atlas(true)
	var stable := true
	for run in range(100):
		var result := _fit(mesh, deterministic_atlas, selected, HotspotFitter.ISLAND_GROUPED, run)
		stable = stable and result.ok and result.diagnostics[0].chosen == "a_trim" and \
				result.diagnostics[0].finalist_names[0] == "a_trim"
	_check(stable, "disallow_random_chooses_sorted_finalist_zero_across_100_runs")


func _check_density_and_tiling(atlas: HotspotAtlas) -> void:
	var grid := _make_flat_wall(5)
	var result := _fit(grid, atlas, _faces(5), HotspotFitter.ISLAND_GROUPED)
	var factor: float = pow(2.0, HotspotFitter.DEFAULT_DENSITY_MARGIN)
	var density_ok: bool = result.ok and result.island_count == 1
	var aspect_ok: bool = density_ok
	if result.ok:
		for face: Dictionary in result.faces:
			var density_u := float(face.realized_u_texels_per_meter)
			var density_v := float(face.realized_v_texels_per_meter)
			density_ok = density_ok and density_u >= atlas.texel_density_target / factor and \
					density_u <= atlas.texel_density_target * factor and \
					density_v >= atlas.texel_density_target / factor and \
					density_v <= atlas.texel_density_target * factor
			aspect_ok = aspect_ok and max(density_u / density_v, density_v / density_u) <= \
					pow(2.0, HotspotFitter.DEFAULT_ASPECT_MARGIN)
	_check(density_ok, "wall_grid_realized_density_within_octave_margin")
	_check(aspect_ok, "wall_grid_cross_axis_not_stretched_beyond_aspect_margin")

	var long_wall := _make_flat_wall(1, 16.0, 2.0)
	var tiled := _fit(long_wall, atlas, PackedInt32Array([0]), HotspotFitter.ISLAND_GROUPED)
	var tiled_face: Dictionary = tiled.faces[0] if tiled.ok else {}
	_check(tiled.ok and tiled_face.repetitions == 4 and tiled_face.tiling_seams == \
			PackedFloat32Array([0.25, 0.5, 0.75]) and \
			tiled_face.uv_mode == LevelMeshData.UV_MODE_EXPLICIT,
			"long_trim_repeats_with_explicit_rep_boundary_seams")
	_check(tiled.ok and is_equal_approx(float(tiled_face.realized_v_texels_per_meter), 32.0),
			"tiling_keeps_cross_axis_one_to_one")


func _check_partition_and_unwind(atlas: HotspotAtlas) -> void:
	var flat := _make_flat_wall(5)
	var flat_result := _fit(flat, atlas, _faces(5), HotspotFitter.ISLAND_GROUPED)
	_check(flat_result.ok and flat_result.island_count == 1,
			"five_coplanar_quads_form_one_island")

	var corner := _make_corner_wall()
	var corner_result := _fit(corner, atlas, PackedInt32Array([0, 1]), HotspotFitter.ISLAND_GROUPED)
	var ranges_contiguous := false
	if corner_result.ok and corner_result.faces.size() == 2:
		var range_a: Vector2 = corner_result.faces[0].arc_u_range
		var range_b: Vector2 = corner_result.faces[1].arc_u_range
		var ordered := [range_a, range_b]
		ordered.sort_custom(func(left: Vector2, right: Vector2) -> bool: return left.x < right.x)
		ranges_contiguous = ordered[0].x >= -0.00001 and ordered[1].y <= 1.00001 and \
				ordered[0].y <= ordered[1].x + 0.00001 and \
				is_equal_approx(ordered[0].y, ordered[1].x)
	_check(corner_result.ok and corner_result.island_count == 1 and \
			corner_result.diagnostics[0].developable_strip and ranges_contiguous,
			"developable_corner_unwinds_to_one_contiguous_arc_split_ribbon")

	var wall_floor := _make_wall_floor()
	var wall_floor_result := _fit(wall_floor, atlas, PackedInt32Array([0, 1]), HotspotFitter.ISLAND_GROUPED)
	_check(wall_floor_result.ok and wall_floor_result.island_count == 2,
			"wall_floor_fold_splits_into_two_islands")
	var individual := _fit(flat, atlas, _faces(5), HotspotFitter.ISLAND_INDIVIDUAL)
	_check(individual.ok and individual.island_count == 5,
			"individual_mode_makes_one_island_per_face")


func _check_sticky_refit_and_undo(atlas: HotspotAtlas) -> void:
	var mesh := _make_flat_wall(20)
	var selected := _faces(20)
	var before_first := _snapshot(mesh.data)
	var first := _fit(mesh, atlas, selected, HotspotFitter.ISLAND_GROUPED, 777)
	var first_diff: LevelMeshDiff = mesh.apply_hotspot_fit(first.faces) if first.ok else null
	var first_names: PackedStringArray = mesh.data.face_hotspot_patch_names
	var first_undo_exact := first_diff != null and mesh.revert_diff(first_diff) and _matches(mesh.data, before_first)
	if first_diff != null:
		mesh.apply_diff(first_diff)
	_check(first_undo_exact, "first_hotspot_fit_undo_is_byte_exact")

	mesh.begin_transaction()
	var moved: PackedVector3Array = mesh.data.vertex_positions
	moved[0] += Vector3(0.01, 0.0, 0.0)
	mesh.data.vertex_positions = moved
	var nudge_diff: LevelMeshDiff = mesh.commit()
	var before_second := _snapshot(mesh.data)
	var second := _fit(mesh, atlas, selected, HotspotFitter.ISLAND_GROUPED, 777)
	var second_diff: LevelMeshDiff = mesh.apply_hotspot_fit(second.faces) if second.ok else null
	var kept := 0
	for face_id in range(first_names.size()):
		if face_id < mesh.data.face_hotspot_patch_names.size() and \
				first_names[face_id] == mesh.data.face_hotspot_patch_names[face_id]:
			kept += 1
	var second_undo_exact := second_diff != null and mesh.revert_diff(second_diff) and _matches(mesh.data, before_second)
	_check(nudge_diff != null and second.ok and kept >= int(ceil(first_names.size() * 0.95)),
			"sticky_refit_keeps_at_least_95_percent_of_patch_names")
	_check(second_undo_exact, "second_hotspot_fit_undo_is_byte_exact")


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
