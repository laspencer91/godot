extends SceneTree

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
	var level_mesh := LevelMesh.new()
	level_mesh.begin_transaction()
	_check(level_mesh.create_box(Transform3D.IDENTITY, Vector3(4, 4, 4), 0),
			"create_box accepted a valid identity-frame box")
	var diff: LevelMeshDiff = level_mesh.commit()
	_check(diff != null, "commit returned a LevelMeshDiff")
	if diff == null:
		_finish()
		return

	var data: LevelMeshData = level_mesh.data
	_check(data.vertex_count() == 8, "box has 8 vertices")
	_check(data.edge_count() == 12, "box has 12 shared edges")
	_check(data.face_count() == 6, "box has 6 faces")
	_check(data.loop_count() == 24, "box has 24 face loops")
	_check(data.vertex_generations.size() == data.vertex_positions.size() and
			data.edge_generations.size() == data.edge_alive.size() and
			data.face_generations.size() == data.face_alive.size(),
			"every vertex, edge, and face slot carries a generation stamp")
	_check_box_uvs(data)
	var adjacency: LevelMeshAdjacency = level_mesh.get_adjacency()
	var element_bvh: LevelMeshElementBVH = level_mesh.get_element_bvh()
	_check_box_adjacency(data, adjacency)
	_check_box_walks(data, adjacency)
	_check_flood_and_plane_queries(adjacency)
	_check_region_rim(adjacency)
	_check_nonmanifold_rim_rejection(data)
	_check_element_queries(level_mesh, element_bvh)
	_check_geometry_diff_bvh(data)
	_check_handle_lifecycle(data)
	_check_disconnected_coplanar_queries()

	var committed_arrays := _capture_arrays(data)
	var baker := LevelMeshBaker.new()
	var baked_mesh: ArrayMesh = baker.bake(data)
	_check(baked_mesh != null, "baker returned an ArrayMesh")
	_check(_triangle_count(baked_mesh) == 12, "baked mesh has 12 triangles")
	var collision_faces: PackedVector3Array = baker.bake_collision_faces(data)
	_check(collision_faces.size() == 36, "collision bake has 36 triangle vertices")

	var block := LevelBlock.new()
	block.data = data
	root.add_child(block)
	block.rebuild()
	var internal_mesh: MeshInstance3D = null
	var internal_body: StaticBody3D = null
	var internal_collision: CollisionShape3D = null
	for child in block.get_children(true):
		if child is MeshInstance3D:
			internal_mesh = child
		elif child is StaticBody3D:
			internal_body = child
	if internal_body != null:
		for child in internal_body.get_children(true):
			if child is CollisionShape3D:
				internal_collision = child
	_check(internal_mesh != null and _triangle_count(internal_mesh.mesh as ArrayMesh) == 12,
			"LevelBlock built its internal render mesh")
	_check(internal_collision != null and internal_collision.shape is ConcavePolygonShape3D,
			"LevelBlock built its internal trimesh collision")
	if internal_mesh == null or internal_collision == null:
		root.remove_child(block)
		block.free()
		_finish()
		return

	_check(level_mesh.revert_diff(diff), "revert_diff accepted the committed diff")
	_check(data.vertex_count() == 0 and data.edge_count() == 0 and
			data.face_count() == 0 and data.loop_count() == 0,
			"revert_diff restored the empty pre-transaction state")
	_check(internal_mesh.mesh.get_surface_count() == 0 and internal_collision.shape == null,
			"LevelBlock rebuilt empty internal children after data changed")
	_check(adjacency.get_face_edges(0).is_empty(),
			"held adjacency service rebuilt after topology revert")
	_check(not element_bvh.ray_closest(Vector3(0, 0, 5), Vector3(0, 0, -1)).hit,
			"held element BVH rebuilt empty after geometry revert")
	_check(level_mesh.apply_diff(diff), "apply_diff accepted the committed diff")
	_check(_arrays_equal(data, committed_arrays),
			"apply_diff restored every serialized topology column exactly")
	_check(_triangle_count(internal_mesh.mesh as ArrayMesh) == 12 and
			internal_collision.shape is ConcavePolygonShape3D,
			"LevelBlock rebuilt render and collision data after redo")
	_check(adjacency.get_face_edges(0).size() == 4,
			"held adjacency service rebuilt after topology apply")
	_check(element_bvh.ray_closest(Vector3(0, 0, 5), Vector3(0, 0, -1)).face_id == 1,
			"held element BVH rebuilt after geometry apply")
	root.remove_child(block)
	block.free()

	_finish()


func _check_box_adjacency(data: LevelMeshData, adjacency: LevelMeshAdjacency) -> void:
	_check(adjacency.is_valid(), "box adjacency cache is structurally valid")
	var all_edges_have_two_faces := true
	for edge_id in range(12):
		var edge_faces := adjacency.get_edge_faces(edge_id)
		var edge_vertices := adjacency.get_edge_vertices(edge_id)
		all_edges_have_two_faces = all_edges_have_two_faces and edge_faces.size() == 2
		all_edges_have_two_faces = all_edges_have_two_faces and edge_vertices.size() == 2
		if edge_vertices.size() == 2:
			all_edges_have_two_faces = all_edges_have_two_faces and (
					adjacency.get_edge_faces_by_vertices(edge_vertices[1], edge_vertices[0]) == edge_faces)
	_check(all_edges_have_two_faces, "all 12 unordered box edges map to exactly two faces")

	var loop_vertices: PackedInt32Array = data.loop_vertex_indices
	var face_starts: PackedInt32Array = data.face_loop_starts
	var face_counts: PackedInt32Array = data.face_loop_counts
	var face_edges_follow_loops := true
	for face_id in range(6):
		var edges := adjacency.get_face_edges(face_id)
		face_edges_follow_loops = face_edges_follow_loops and edges.size() == 4
		if edges.size() != 4:
			continue
		for corner in range(face_counts[face_id]):
			var expected_a := loop_vertices[face_starts[face_id] + corner]
			var expected_b := loop_vertices[
					face_starts[face_id] + ((corner + 1) % face_counts[face_id])]
			var edge_vertices := adjacency.get_edge_vertices(edges[corner])
			face_edges_follow_loops = face_edges_follow_loops and edge_vertices.size() == 2
			if edge_vertices.size() == 2:
				face_edges_follow_loops = face_edges_follow_loops and (
						(edge_vertices[0] == expected_a and edge_vertices[1] == expected_b) or
						(edge_vertices[0] == expected_b and edge_vertices[1] == expected_a))
	_check(face_edges_follow_loops, "face to edge adjacency preserves kernel loop order")

	var every_vertex_has_box_valence := true
	for vertex_id in range(8):
		every_vertex_has_box_valence = every_vertex_has_box_valence and (
				adjacency.get_vertex_edges(vertex_id).size() == 3 and
				adjacency.get_vertex_faces(vertex_id).size() == 3)
	_check(every_vertex_has_box_valence, "box vertices each map to three edges and three faces")


func _check_box_walks(data: LevelMeshData, adjacency: LevelMeshAdjacency) -> void:
	var loops_stop_at_valence_three := true
	var rings_are_four_edge_bands := true
	var edge_vertices: PackedInt32Array = data.edge_vertices
	var positions: PackedVector3Array = data.vertex_positions
	for edge_id in range(12):
		loops_stop_at_valence_three = loops_stop_at_valence_three and (
				adjacency.walk_edge_loop(edge_id) == PackedInt32Array([edge_id]))
		var ring := adjacency.walk_edge_ring(edge_id)
		var unique_edges := {}
		var seed_direction := positions[edge_vertices[edge_id * 2 + 1]] - positions[edge_vertices[edge_id * 2]]
		rings_are_four_edge_bands = rings_are_four_edge_bands and ring.size() == 4
		for ring_edge_id in ring:
			unique_edges[ring_edge_id] = true
			var ring_direction := positions[edge_vertices[ring_edge_id * 2 + 1]] - positions[edge_vertices[ring_edge_id * 2]]
			rings_are_four_edge_bands = rings_are_four_edge_bands and (
					seed_direction.cross(ring_direction).length_squared() <= 0.000001)
		rings_are_four_edge_bands = rings_are_four_edge_bands and unique_edges.size() == 4
	_check(loops_stop_at_valence_three,
			"all box edge loops stop at their valence-3 endpoints")
	_check(rings_are_four_edge_bands,
			"all box edge rings traverse one four-edge parallel band and terminate")


func _check_flood_and_plane_queries(adjacency: LevelMeshAdjacency) -> void:
	_check(adjacency.coplanar_flood_fill(0) == PackedInt32Array([0]),
			"coplanar flood on a box face returns only that face")
	_check(adjacency.faces_on_plane(0) == PackedInt32Array([0]),
			"plane-wide query on one box returns its sole face on the seed plane")


func _check_region_rim(adjacency: LevelMeshAdjacency) -> void:
	var one_face := adjacency.classify_region_rim(PackedInt32Array([0]))
	_check(one_face.valid and not one_face.ambiguous_non_manifold and
			one_face.interior_edges.is_empty() and one_face.rim_edges.size() == 4 and
			one_face.rim_owner_faces == PackedInt32Array([0, 0, 0, 0]),
			"single-face region classifies four owned rim edges")
	var closed_box := adjacency.classify_region_rim(PackedInt32Array([0, 1, 2, 3, 4, 5]))
	_check(closed_box.valid and closed_box.interior_edges.size() == 12 and
			closed_box.rim_edges.is_empty(),
			"closed box region classifies all shared edges as interior")


func _check_nonmanifold_rim_rejection(source_data: LevelMeshData) -> void:
	var nonmanifold_data := source_data.duplicate_data()
	var loop_vertices: PackedInt32Array = nonmanifold_data.loop_vertex_indices
	for corner in range(4):
		loop_vertices[4 + corner] = loop_vertices[corner]
	nonmanifold_data.loop_vertex_indices = loop_vertices
	var nonmanifold_mesh := LevelMesh.new()
	nonmanifold_mesh.data = nonmanifold_data
	var classification := nonmanifold_mesh.get_adjacency().classify_region_rim(
			PackedInt32Array([0]))
	_check(not classification.valid and classification.ambiguous_non_manifold and
			classification.reason == "non_manifold_edge",
			"region rim reports an explicit reject signal for non-manifold ownership")


func _check_element_queries(level_mesh: LevelMesh, element_bvh: LevelMeshElementBVH) -> void:
	_check(element_bvh.is_valid() and element_bvh.get_triangle_count() == 12,
			"element BVH contains the box's 12 kernel-order fan triangles")
	var hit := level_mesh.ray_closest(Vector3(0, 0, 5), Vector3(0, 0, -1))
	var barycentric: Vector3 = hit.barycentric
	_check(hit.hit and hit.face_id == 1 and hit.tri_id in [2, 3] and
			is_equal_approx(hit.t, 3.0) and is_equal_approx(
					barycentric.x + barycentric.y + barycentric.z, 1.0),
			"ray_closest hits the known +Z face center with triangle identity and barycentrics")
	_check(level_mesh.get_face_corner_vertex_ids(1).size() == 4 and
			level_mesh.get_face_corner_positions(1).size() == 4 and
			level_mesh.get_face_boundary_edge_ids(1).size() == 4 and
			level_mesh.get_face_boundary_edge_positions(1).size() == 8,
			"bulk face corners and boundary edge endpoint positions are exposed")


func _check_geometry_diff_bvh(source_data: LevelMeshData) -> void:
	var geometry_mesh := LevelMesh.new()
	geometry_mesh.data = source_data.duplicate_data()
	var held_bvh := geometry_mesh.get_element_bvh()
	var before_hit := held_bvh.ray_closest(Vector3(0, 0, 5), Vector3(0, 0, -1))
	geometry_mesh.begin_transaction()
	var translated_positions: PackedVector3Array = geometry_mesh.data.vertex_positions
	for vertex_id in range(translated_positions.size()):
		translated_positions[vertex_id] += Vector3(0, 0, 10)
	geometry_mesh.data.vertex_positions = translated_positions
	var geometry_diff: LevelMeshDiff = geometry_mesh.commit()
	var after_hit := held_bvh.ray_closest(Vector3(0, 0, 15), Vector3(0, 0, -1))
	_check(before_hit.hit and geometry_diff != null and geometry_diff.touches_geometry() and
			not geometry_diff.touches_topology() and after_hit.hit and after_hit.face_id == 1 and
			is_equal_approx(after_hit.t, 3.0),
			"position-only geometry diff dirties and lazily rebuilds a held element BVH")


func _check_handle_lifecycle(source_data: LevelMeshData) -> void:
	var handle_mesh := LevelMesh.new()
	handle_mesh.data = source_data.duplicate_data()
	var vertex_handle := handle_mesh.make_vertex_handle(0)
	var edge_handle := handle_mesh.make_edge_handle(0)
	var face_handle := handle_mesh.make_face_handle(0)
	_check(vertex_handle != -1 and edge_handle != -1 and face_handle != -1 and
			handle_mesh.resolve_vertex(vertex_handle) == 0 and
			handle_mesh.resolve_edge(edge_handle) == 0 and
			handle_mesh.resolve_face(face_handle) == 0,
			"vertex, edge, and face handles round-trip to their live slots")

	handle_mesh.begin_transaction()
	var freed_all := handle_mesh.data.free_vertex_slot(0)
	freed_all = handle_mesh.data.free_edge_slot(0) and freed_all
	freed_all = handle_mesh.data.free_face_slot(0) and freed_all
	var removal_diff: LevelMeshDiff = handle_mesh.commit()
	_check(freed_all and removal_diff != null and removal_diff.touches_topology(),
			"data-layer simulated frees commit as a topology diff")
	if removal_diff == null:
		return
	_check(handle_mesh.resolve_vertex(vertex_handle) == -1 and
			handle_mesh.resolve_edge(edge_handle) == -1 and
			handle_mesh.resolve_face(face_handle) == -1,
			"generation bumps make all freed element handles stale")
	_check(removal_diff.removed_vertex_handles == PackedInt64Array([vertex_handle]) and
			removal_diff.removed_edge_handles == PackedInt64Array([edge_handle]) and
			removal_diff.removed_face_handles == PackedInt64Array([face_handle]),
			"diff exposes exact generation-stamped removed handles")
	_check(handle_mesh.revert_diff(removal_diff) and
			handle_mesh.resolve_vertex(vertex_handle) == 0 and
			handle_mesh.resolve_edge(edge_handle) == 0 and
			handle_mesh.resolve_face(face_handle) == 0,
			"reverting a free restores the original stable identities")
	_check(handle_mesh.apply_diff(removal_diff) and
			handle_mesh.resolve_vertex(vertex_handle) == -1 and
			handle_mesh.resolve_edge(edge_handle) == -1 and
			handle_mesh.resolve_face(face_handle) == -1,
			"reapplying a free invalidates the restored handles again")

	var branch_mesh := LevelMesh.new()
	branch_mesh.begin_transaction()
	var branch_created := branch_mesh.create_box(Transform3D.IDENTITY, Vector3.ONE, 0)
	var create_diff: LevelMeshDiff = branch_mesh.commit()
	var abandoned_handle := branch_mesh.make_vertex_handle(0)
	var branch_reverted := create_diff != null and branch_mesh.revert_diff(create_diff)
	branch_mesh.begin_transaction()
	branch_created = branch_mesh.create_box(Transform3D.IDENTITY, Vector3.ONE, 0) and branch_created
	var replacement_diff := branch_mesh.commit()
	var replacement_handle := branch_mesh.make_vertex_handle(0)
	_check(branch_created and branch_reverted and replacement_diff != null and
			replacement_handle != abandoned_handle and branch_mesh.resolve_vertex(abandoned_handle) == -1,
			"branching after undo assigns a fresh generation to a reused tail slot")


func _check_disconnected_coplanar_queries() -> void:
	var two_shell_mesh := LevelMesh.new()
	two_shell_mesh.begin_transaction()
	var created := two_shell_mesh.create_box(Transform3D.IDENTITY, Vector3(4, 4, 4), 0)
	created = two_shell_mesh.create_box(
			Transform3D(Basis.IDENTITY, Vector3(4, 0, 0)), Vector3(4, 4, 4), 0) and created
	var diff := two_shell_mesh.commit()
	var adjacency := two_shell_mesh.get_adjacency()
	_check(created and diff != null and adjacency.coplanar_flood_fill(0) == PackedInt32Array([0]) and
			adjacency.faces_on_plane(0) == PackedInt32Array([0, 6]),
			"flood respects connectivity while plane-wide query finds disconnected coplanar faces")
	_check(adjacency.coplanar_flood_fill(3) == PackedInt32Array([3]) and
			adjacency.faces_on_plane(3) == PackedInt32Array([3, 8]),
			"plane-wide query treats opposite-winding faces as lying on the same infinite plane")

	var touching_mesh := LevelMesh.new()
	touching_mesh.begin_transaction()
	created = touching_mesh.create_box(
			Transform3D(Basis.IDENTITY, Vector3(4, 0, 0)), Vector3(4, 4, 4), 0)
	diff = touching_mesh.commit()
	_check(created and diff != null and
			touching_mesh.get_adjacency().coplanar_flood_fill(2) == PackedInt32Array([2]),
			"coplanar touching boxes in separate LevelMesh instances cannot cross-flood")


func _check_box_uvs(data: LevelMeshData) -> void:
	var vertices: PackedVector3Array = data.vertex_positions
	var face_starts: PackedInt32Array = data.face_loop_starts
	var face_counts: PackedInt32Array = data.face_loop_counts
	var face_modes: PackedInt32Array = data.face_uv_modes
	var face_origins: PackedVector3Array = data.face_uv_origins
	var face_tangents: PackedVector3Array = data.face_uv_tangents
	var loop_vertices: PackedInt32Array = data.loop_vertex_indices
	var loop_uv0: PackedVector2Array = data.loop_uv0
	var loop_normals: PackedVector3Array = data.loop_normals
	var all_projected := true
	var all_outward := true
	var all_materialized := true

	for face_id in range(6):
		var loop_start := face_starts[face_id]
		var loop_count := face_counts[face_id]
		var p0 := vertices[loop_vertices[loop_start]]
		var p1 := vertices[loop_vertices[loop_start + 1]]
		var p2 := vertices[loop_vertices[loop_start + 2]]
		var normal := (p1 - p0).cross(p2 - p0).normalized()
		var expected_tangent := _dominant_axis_tangent(normal)
		var tangent := face_tangents[face_id]
		var bitangent := normal.cross(tangent).normalized()
		var uv_transform := data.get_face_uv_transform(face_id)
		all_projected = all_projected and face_modes[face_id] == LevelMeshData.UV_MODE_PROJECTED
		all_projected = all_projected and face_origins[face_id].is_equal_approx(Vector3.ZERO)
		all_projected = all_projected and tangent.is_equal_approx(expected_tangent)
		all_projected = all_projected and uv_transform.is_equal_approx(Transform2D.IDENTITY)

		var face_center := Vector3.ZERO
		for corner in range(loop_count):
			var loop_id := loop_start + corner
			var position := vertices[loop_vertices[loop_id]]
			face_center += position
			var relative_position := position - face_origins[face_id]
			var raw_uv := Vector2(relative_position.dot(tangent), relative_position.dot(bitangent))
			var expected_uv: Vector2 = uv_transform * raw_uv
			all_materialized = all_materialized and loop_uv0[loop_id].is_equal_approx(expected_uv)
			all_materialized = all_materialized and loop_normals[loop_id].is_equal_approx(normal)
		face_center /= loop_count
		all_outward = all_outward and normal.dot(face_center) > 0.0

	_check(all_projected, "all faces use dominant-axis PROJECTED UV frames")
	_check(all_materialized, "all 24 loop uv0 values and normals are materialized from their frames")
	_check(all_outward, "all six quad faces have outward winding")


func _dominant_axis_tangent(normal: Vector3) -> Vector3:
	# Deliberately amended with WP13: default frames use the Cyclops negative sign
	# table (tools/09 §1.2) so create_box agrees with Align-to-Grid/extrude walls.
	var absolute_normal := normal.abs()
	var world_tangent := Vector3(0, 0, -1) if (
			absolute_normal.x >= absolute_normal.y and absolute_normal.x >= absolute_normal.z
	) else Vector3(-1, 0, 0)
	return (world_tangent - normal * normal.dot(world_tangent)).normalized()


func _capture_arrays(data: LevelMeshData) -> Dictionary:
	var snapshot := {}
	for property_name in DATA_ARRAY_PROPERTIES:
		snapshot[property_name] = data.get(property_name)
	return snapshot


func _arrays_equal(data: LevelMeshData, snapshot: Dictionary) -> bool:
	for property_name in DATA_ARRAY_PROPERTIES:
		if data.get(property_name) != snapshot[property_name]:
			printerr("FAIL DETAIL: serialized column changed after redo: " + property_name)
			return false
	return true


func _triangle_count(mesh: ArrayMesh) -> int:
	var total := 0
	for surface_index in range(mesh.get_surface_count()):
		var arrays := mesh.surface_get_arrays(surface_index)
		var indices: PackedInt32Array = arrays[ArrayMesh.ARRAY_INDEX]
		if indices.is_empty():
			var surface_vertices: PackedVector3Array = arrays[ArrayMesh.ARRAY_VERTEX]
			total += int(surface_vertices.size() / 3)
		else:
			total += int(indices.size() / 3)
	return total


func _check(condition: bool, label: String) -> void:
	if condition:
		print("PASS: " + label)
	else:
		print("FAIL: " + label)
		failures.append(label)


func _finish() -> void:
	if failures.is_empty():
		print("LEVEL_KERNEL_SMOKE: PASS")
		quit(0)
	else:
		printerr("LEVEL_KERNEL_SMOKE: FAIL (%d checks)" % failures.size())
		quit(1)
