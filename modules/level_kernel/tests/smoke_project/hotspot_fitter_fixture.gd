class_name HotspotFitterFixture
extends RefCounted


func make_patch(name: StringName, rect: Rect2) -> HotspotPatch:
	var patch := HotspotPatch.new()
	patch.patch_name = name
	patch.rect_uv = rect
	patch.allow_rotation = true
	patch.allow_mirror_x = true
	patch.allow_mirror_y = true
	patch.allow_tiling = true
	patch.tiling_axis = HotspotPatch.TILING_AXIS_U
	return patch


func make_atlas(disallow_random: bool) -> HotspotAtlas:
	var image := Image.create(256, 256, false, Image.FORMAT_RGBA8)
	image.fill(Color(0.25, 0.5, 0.75, 1.0))
	var texture := ImageTexture.create_from_image(image)
	var atlas := HotspotAtlas.new()
	atlas.atlas_id = &"smoke/wall_trim"
	atlas.reference_texture = texture
	atlas.texel_density_target = 32.0
	atlas.default_mapping_mode = HotspotAtlas.MAPPING_AUTOMATIC
	atlas.disallow_random = disallow_random
	atlas.tiling_policy = HotspotAtlas.TILING_ALLOW
	var patches: Array[HotspotPatch] = [
		make_patch(&"a_trim", Rect2(0.0, 0.0, 0.5, 0.25)),
		make_patch(&"b_trim", Rect2(0.5, 0.0, 0.5, 0.25)),
	]
	atlas.patches = patches
	return atlas


func make_mesh(positions: PackedVector3Array, faces: Array) -> LevelMesh:
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


func make_flat_wall(face_count: int, face_width := 4.0, height := 2.0) -> LevelMesh:
	var positions := PackedVector3Array()
	for boundary in range(face_count + 1):
		positions.append(Vector3(boundary * face_width, 0, 0))
		positions.append(Vector3(boundary * face_width, height, 0))
	var faces: Array = []
	for face_id in range(face_count):
		faces.append(PackedInt32Array([
			face_id * 2, (face_id + 1) * 2,
			(face_id + 1) * 2 + 1, face_id * 2 + 1,
		]))
	return make_mesh(positions, faces)


func make_corner_wall() -> LevelMesh:
	return make_mesh(PackedVector3Array([
		Vector3(0, 0, 0), Vector3(0, 2, 0),
		Vector3(4, 0, 0), Vector3(4, 2, 0),
		Vector3(4, 0, 4), Vector3(4, 2, 4),
	]), [
		PackedInt32Array([0, 2, 3, 1]),
		PackedInt32Array([2, 4, 5, 3]),
	])


func make_wall_floor() -> LevelMesh:
	return make_mesh(PackedVector3Array([
		Vector3(0, 0, 0), Vector3(4, 0, 0),
		Vector3(4, 2, 0), Vector3(0, 2, 0),
		Vector3(4, 0, 2), Vector3(0, 0, 2),
	]), [
		PackedInt32Array([0, 1, 2, 3]),
		PackedInt32Array([0, 5, 4, 1]),
	])


func faces(count: int) -> PackedInt32Array:
	var result := PackedInt32Array()
	for face_id in range(count):
		result.append(face_id)
	return result
