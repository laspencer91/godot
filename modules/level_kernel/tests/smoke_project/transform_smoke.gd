extends SceneTree

const DATA_ARRAY_PROPERTIES: PackedStringArray = [
	"vertex_positions", "vertex_alive", "vertex_generations", "vertex_free_ids",
	"edge_vertices", "edge_alive", "edge_generations", "edge_free_ids",
	"face_loop_starts", "face_loop_counts", "face_material_indices", "face_uv_modes",
	"face_uv_origins", "face_uv_tangents", "face_uv_transforms", "face_polygroup_ids",
	"face_flags", "face_alive", "face_generations", "face_free_ids",
	"loop_vertex_indices", "loop_uv0", "loop_colors", "loop_normals", "loop_alive",
	"loop_free_ids",
]

const TOPOLOGY_PROPERTIES: PackedStringArray = [
	"vertex_alive", "vertex_generations", "vertex_free_ids",
	"edge_vertices", "edge_alive", "edge_generations", "edge_free_ids",
	"face_loop_starts", "face_loop_counts", "face_alive", "face_generations",
	"face_free_ids", "loop_vertex_indices", "loop_alive", "loop_free_ids",
]

var failures: Array[String] = []


func _initialize() -> void:
	_check_preview_lifecycle()
	_check_face_extrude()
	_check_nonmanifold_rejection()
	_check_push_pull_and_boundary_rejection()
	if failures.is_empty():
		print("TRANSFORM_SMOKE_OK")
		quit(0)
	else:
		printerr("TRANSFORM_SMOKE_FAIL: %s" % ", ".join(failures))
		quit(1)


func _make_box() -> LevelMesh:
	var mesh := LevelMesh.new()
	mesh.begin_transaction()
	if not mesh.create_box(Transform3D.IDENTITY, Vector3(4, 4, 4), 7):
		_check(false, "create_box")
		mesh.rollback()
		return mesh
	if mesh.commit() == null:
		_check(false, "create_box_diff")
	return mesh


func _check_preview_lifecycle() -> void:
	var mesh := _make_box()
	var before := _capture(mesh.data, DATA_ARRAY_PROPERTIES)
	var diff_count := [0]
	mesh.data.mesh_diff_applied.connect(func(_diff: LevelMeshDiff, _reverted: bool) -> void:
		diff_count[0] += 1
	)
	var vertex_ids := PackedInt32Array([6])
	var original: Vector3 = mesh.data.vertex_positions[6]
	_check(mesh.begin_transform_preview(vertex_ids), "preview_begin")
	_check(mesh.preview_transform_vertices(PackedVector3Array([original + Vector3(0.25, 0.5, 0.75)])),
			"preview_frame_1")
	_check(mesh.preview_transform_vertices(PackedVector3Array([original + Vector3(1, 0, 0)])),
			"preview_frame_2")
	_check(diff_count[0] == 0, "preview_has_no_diff")
	var move_diff: LevelMeshDiff = mesh.commit_transform_preview()
	_check(move_diff != null and diff_count[0] == 1 and not move_diff.touches_topology(),
			"preview_commit_one_geometry_diff")

	var cancel_snapshot := _capture(mesh.data, DATA_ARRAY_PROPERTIES)
	var moved_from: Vector3 = mesh.data.vertex_positions[6]
	_check(mesh.begin_transform_preview(vertex_ids), "cancel_begin")
	_check(mesh.preview_transform_vertices(PackedVector3Array([moved_from + Vector3(9, -3, 2)])),
			"cancel_preview")
	mesh.cancel_transform_preview()
	_check(_matches(mesh.data, cancel_snapshot, DATA_ARRAY_PROPERTIES), "cancel_bit_exact")
	_check(diff_count[0] == 1, "cancel_has_no_diff")

	# A rigid motion of a complete, non-axis-aligned box exercises the face-tangent
	# closed form and must preserve every materialized loop UV.
	var all_vertices := PackedInt32Array()
	var rotated_positions := PackedVector3Array()
	var old_uvs: PackedVector2Array = mesh.data.loop_uv0
	var rotation := Basis(Vector3(0.37, 0.81, 0.45).normalized(), 0.63)
	for vertex_id in range(mesh.data.vertex_positions.size()):
		all_vertices.append(vertex_id)
		rotated_positions.append(rotation * mesh.data.vertex_positions[vertex_id] + Vector3(2, -1, 3))
	_check(mesh.begin_transform_preview(all_vertices), "rigid_begin")
	_check(mesh.preview_transform_vertices(rotated_positions), "rigid_preview")
	var rigid_diff: LevelMeshDiff = mesh.commit_transform_preview()
	var uv_preserved := rigid_diff != null
	for loop_id in range(old_uvs.size()):
		uv_preserved = uv_preserved and mesh.data.loop_uv0[loop_id].is_equal_approx(old_uvs[loop_id])
	_check(uv_preserved, "rigid_texture_lock_face_tangent_exact")

	mesh.begin_transaction()
	_check(mesh.is_face_texture_locked(0), "texture_lock_default_on")
	_check(mesh.set_face_texture_lock(0, false), "texture_lock_toggle")
	var lock_diff: LevelMeshDiff = mesh.commit()
	_check(lock_diff != null and not mesh.is_face_texture_locked(0), "texture_lock_toggle_diff")
	_check(not _matches(mesh.data, before, DATA_ARRAY_PROPERTIES), "committed_move_changed_state")


func _check_face_extrude() -> void:
	var mesh := _make_box()
	var cap_handle := mesh.make_face_handle(1)
	var source_polygroup: int = mesh.data.face_polygroup_ids[1]
	var source_flags: int = mesh.data.face_flags[1]
	var source_material: int = mesh.data.face_material_indices[1]
	var topology_diff: LevelMeshDiff = mesh.extrude_faces(PackedInt32Array([1]))
	_check(topology_diff != null, "face_extrude_diff")
	if topology_diff == null:
		return
	_check(mesh.data.face_count() == 10 and topology_diff.touches_topology(), "face_extrude_four_walls")
	_check(mesh.resolve_face(cap_handle) == 1, "face_extrude_cap_handle_stable")

	var cap_vertices: PackedInt32Array = mesh.get_face_corner_vertex_ids(1)
	var cap_positions := PackedVector3Array()
	for vertex_id in cap_vertices:
		cap_positions.append(mesh.data.vertex_positions[vertex_id] + Vector3(0, 0, 2))
	_check(mesh.begin_transform_preview(cap_vertices), "extrude_cap_preview_begin")
	_check(mesh.preview_transform_vertices(cap_positions), "extrude_cap_preview")
	var geometry_diff: LevelMeshDiff = mesh.commit_transform_preview()
	_check(geometry_diff != null and not geometry_diff.touches_topology(), "extrude_cap_geometry_diff")

	var new_face_handles: PackedInt64Array = topology_diff.get_revert_removed_face_handles()
	var wall_ids: Array[int] = []
	for handle in new_face_handles:
		var face_id := mesh.resolve_face(handle)
		if face_id >= 0:
			wall_ids.append(face_id)
	_check(wall_ids.size() == 4, "face_extrude_created_handle_count")
	var winding_ok := wall_ids.size() == 4
	var attributes_ok := wall_ids.size() == 4
	var wall_polygroups: Dictionary = {}
	for face_id in wall_ids:
		var positions: PackedVector3Array = mesh.get_face_corner_positions(face_id)
		if positions.size() != 4:
			winding_ok = false
			continue
		var normal := (positions[1] - positions[0]).cross(positions[2] - positions[0]).normalized()
		var center := Vector3.ZERO
		for position in positions:
			center += position
		center /= 4.0
		# Side walls have no Z radial component; positive dot proves CCW-outward
		# without relying on the baker's opposite front-face convention.
		winding_ok = winding_ok and normal.dot(Vector3(center.x, center.y, 0)) > 0.0
		var polygroup: int = mesh.data.face_polygroup_ids[face_id]
		attributes_ok = attributes_ok and polygroup != source_polygroup and not wall_polygroups.has(polygroup)
		attributes_ok = attributes_ok and mesh.data.face_material_indices[face_id] == source_material
		attributes_ok = attributes_ok and mesh.data.face_flags[face_id] == source_flags
		wall_polygroups[polygroup] = true
	_check(winding_ok, "face_extrude_local_ccw_winding")
	_check(attributes_ok, "face_extrude_fresh_polygroups_and_inheritance")


func _check_nonmanifold_rejection() -> void:
	var data := LevelMeshData.new()
	data.vertex_positions = PackedVector3Array([
		Vector3(0, 0, 0), Vector3(1, 0, 0), Vector3(0, 1, 0),
		Vector3(0, 0, 1), Vector3(0, -1, 0),
	])
	data.vertex_alive = PackedByteArray([1, 1, 1, 1, 1])
	data.edge_vertices = PackedInt32Array([0, 1, 1, 2, 2, 0, 0, 3, 3, 1, 1, 4, 4, 0])
	data.edge_alive = PackedByteArray([1, 1, 1, 1, 1, 1, 1])
	data.face_loop_starts = PackedInt32Array([0, 3, 6])
	data.face_loop_counts = PackedInt32Array([3, 3, 3])
	data.face_material_indices = PackedInt32Array([0, 0, 0])
	data.face_uv_modes = PackedInt32Array([
		LevelMeshData.UV_MODE_PROJECTED, LevelMeshData.UV_MODE_PROJECTED, LevelMeshData.UV_MODE_PROJECTED,
	])
	data.face_uv_origins = PackedVector3Array([Vector3.ZERO, Vector3.ZERO, Vector3.ZERO])
	data.face_uv_tangents = PackedVector3Array([Vector3.ZERO, Vector3.ZERO, Vector3.ZERO])
	var transforms := PackedFloat32Array()
	for _face in range(3):
		transforms.append_array(PackedFloat32Array([1, 0, 0, 1, 0, 0]))
	data.face_uv_transforms = transforms
	data.face_polygroup_ids = PackedInt32Array([0, 1, 2])
	data.face_flags = PackedInt32Array([
		LevelMeshData.FACE_FLAG_TEXTURE_LOCK,
		LevelMeshData.FACE_FLAG_TEXTURE_LOCK,
		LevelMeshData.FACE_FLAG_TEXTURE_LOCK,
	])
	data.face_alive = PackedByteArray([1, 1, 1])
	data.loop_vertex_indices = PackedInt32Array([0, 1, 2, 1, 0, 3, 0, 1, 4])
	data.loop_uv0 = PackedVector2Array([Vector2.ZERO, Vector2.ZERO, Vector2.ZERO, Vector2.ZERO,
		Vector2.ZERO, Vector2.ZERO, Vector2.ZERO, Vector2.ZERO, Vector2.ZERO])
	data.loop_colors = PackedColorArray([Color.WHITE, Color.WHITE, Color.WHITE, Color.WHITE,
		Color.WHITE, Color.WHITE, Color.WHITE, Color.WHITE, Color.WHITE])
	data.loop_normals = PackedVector3Array([Vector3.ZERO, Vector3.ZERO, Vector3.ZERO, Vector3.ZERO,
		Vector3.ZERO, Vector3.ZERO, Vector3.ZERO, Vector3.ZERO, Vector3.ZERO])
	data.loop_alive = PackedByteArray([1, 1, 1, 1, 1, 1, 1, 1, 1])
	var mesh := LevelMesh.new()
	mesh.data = data
	var snapshot := _capture(data, DATA_ARRAY_PROPERTIES)
	var rejected: LevelMeshDiff = mesh.extrude_faces(PackedInt32Array([0, 1]))
	_check(rejected == null and _matches(data, snapshot, DATA_ARRAY_PROPERTIES),
			"ambiguous_nonmanifold_rejects_atomically")


func _check_push_pull_and_boundary_rejection() -> void:
	var mesh := _make_box()
	var topology := _capture(mesh.data, TOPOLOGY_PROPERTIES)
	var old_positions: PackedVector3Array = mesh.data.vertex_positions
	var push_diff: LevelMeshDiff = mesh.push_pull_faces(PackedInt32Array([1]), 1.5)
	_check(push_diff != null and not push_diff.touches_topology() and push_diff.touches_geometry(),
			"push_pull_geometry_only_diff")
	_check(_matches(mesh.data, topology, TOPOLOGY_PROPERTIES), "push_pull_topology_hash_unchanged")
	_check(mesh.data.vertex_positions != old_positions, "push_pull_moves_positions")

	var snapshot := _capture(mesh.data, DATA_ARRAY_PROPERTIES)
	var rejected: LevelMeshDiff = mesh.extrude_boundary_edges(PackedInt32Array([0]))
	_check(rejected == null and _matches(mesh.data, snapshot, DATA_ARRAY_PROPERTIES),
			"interior_edge_extrude_rejected_atomically")

	# Opening one box face turns its four former shared edges into true boundary
	# edges. One selected edge must create a welded two-vertex/three-edge quad.
	mesh.begin_transaction()
	var opened := mesh.data.free_face_slot(1)
	var open_diff: LevelMeshDiff = mesh.commit()
	var owner_faces: PackedInt32Array = mesh.get_adjacency().get_edge_faces(4)
	var before_vertices := mesh.data.vertex_count()
	var before_edges := mesh.data.edge_count()
	var before_faces := mesh.data.face_count()
	var boundary_diff: LevelMeshDiff = mesh.extrude_boundary_edges(PackedInt32Array([4]))
	_check(opened and open_diff != null and owner_faces.size() == 1 and boundary_diff != null and
			mesh.data.vertex_count() == before_vertices + 2 and
			mesh.data.edge_count() == before_edges + 3 and mesh.data.face_count() == before_faces + 1,
			"boundary_edge_extrude_creates_one_welded_quad")


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
	if not condition:
		failures.append(label)
