@tool
extends EditorPlugin

const SmokeInputReady = preload("res://addons/smoke_input_ready.gd")
const TOOL_SELECT := 0
const FEATURE_FACE := 2
const MODE_CONFORMING := 1
const MODE_SQUARE := 2
const MODE_PLANAR := 4
const SPACING_LENGTH := 0

var failed := false


func _enter_tree() -> void:
	_run_test()


func _fail(message: String) -> void:
	if failed:
		return
	failed = true
	push_error("FAST_TEXTURE_SMOKE: " + message)


func _check(condition: bool, message: String) -> bool:
	if not condition:
		_fail(message)
	return condition


func _find_level_view() -> Control:
	return EditorInterface.get_base_control().find_child("LevelEditorView", true, false) as Control


func _make_block() -> LevelBlock:
	var mesh := LevelMesh.new()
	mesh.begin_transaction()
	if not mesh.create_box(Transform3D.IDENTITY, Vector3(2, 2, 2), 0):
		_fail("Kernel box creation was rejected.")
		return null
	if mesh.commit() == null:
		_fail("Kernel box creation returned no diff.")
		return null
	var block := LevelBlock.new()
	block.name = "FastTextureSmokeBlock"
	block.data = mesh.data
	return block


func _send_key(host_viewport: Viewport, keycode: Key, shift := false, ctrl := false, alt := false) -> void:
	var event := InputEventKey.new()
	event.keycode = keycode
	event.physical_keycode = keycode
	event.shift_pressed = shift
	event.ctrl_pressed = ctrl
	event.alt_pressed = alt
	event.pressed = true
	host_viewport.push_input(event, true)
	await get_tree().process_frame


func _send_click(host_viewport: Viewport, position: Vector2) -> void:
	var press := InputEventMouseButton.new()
	press.position = position
	press.global_position = position
	press.button_index = MOUSE_BUTTON_LEFT
	press.button_mask = MOUSE_BUTTON_MASK_LEFT
	press.pressed = true
	host_viewport.push_input(press, true)
	await get_tree().process_frame
	var release := InputEventMouseButton.new()
	release.position = position
	release.global_position = position
	release.button_index = MOUSE_BUTTON_LEFT
	release.button_mask = 0
	release.pressed = false
	host_viewport.push_input(release, true)
	await get_tree().process_frame


func _global_screen(container: Control, camera: Camera3D, world_position: Vector3) -> Vector2:
	return container.get_global_transform_with_canvas() * camera.unproject_position(world_position)


func _face_entries(view: Control) -> Array:
	var result: Array = []
	for entry: Dictionary in view.get_meta("_level_selection_entries", []):
		if int(entry.get("feature", -1)) == FEATURE_FACE:
			result.append(entry)
	return result


func _uv_columns_bytes(data: LevelMeshData) -> PackedByteArray:
	return var_to_bytes([
		data.face_uv_modes,
		data.face_uv_origins,
		data.face_uv_tangents,
		data.face_uv_transforms,
		data.loop_uv0,
	])


func _mesh_bytes(data: LevelMeshData) -> PackedByteArray:
	return var_to_bytes([
		data.material_paths,
		data.vertex_positions, data.vertex_alive, data.vertex_generations, data.vertex_free_ids,
		data.edge_vertices, data.edge_alive, data.edge_generations, data.edge_free_ids,
		data.face_loop_starts, data.face_loop_counts, data.face_material_indices,
		data.face_uv_modes, data.face_uv_origins, data.face_uv_tangents, data.face_uv_transforms,
		data.face_polygroup_ids, data.face_flags, data.face_alive, data.face_generations, data.face_free_ids,
		data.loop_vertex_indices, data.loop_uv0, data.loop_colors, data.loop_normals,
		data.loop_alive, data.loop_free_ids,
	])


func _face_uv_bytes(data: LevelMeshData, face_id: int) -> PackedByteArray:
	var loop_uvs := PackedVector2Array()
	var loop_start: int = data.face_loop_starts[face_id]
	var loop_count: int = data.face_loop_counts[face_id]
	for corner in loop_count:
		loop_uvs.append(data.loop_uv0[loop_start + corner])
	return var_to_bytes([
		data.face_uv_modes[face_id], data.face_uv_origins[face_id], data.face_uv_tangents[face_id],
		data.get_face_uv_transform(face_id), loop_uvs,
	])


func _open_overlay(view: Control, host_viewport: Viewport) -> Object:
	await _send_key(host_viewport, KEY_Q, true)
	if not _check(view.get_meta("_level_fast_texture_open", false),
			"Shift+Q did not open Fast Texture (status=%s mode=%s faces=%s focus=%s action=%s)." % [
				view.get_meta("_level_fast_texture_status", ""), view.get_meta("_level_selection_mode", -1),
				view.get_meta("_level_selection_face_count", -1),
				(view.find_child("LevelViewportContainer", true, false) as Control).has_focus(),
				view.get_meta("_level_last_selection_action", &"")]):
		return null
	if not _check(view.get_meta("_level_fast_texture_input_context", false), "Fast Texture did not push its modal input context."):
		return null
	var session: Object = view.get_meta("_level_fast_texture_session", null)
	if not _check(session != null and session.is_active(), "The overlay did not expose an active scriptable session."):
		return null
	return session


func _run_test() -> void:
	var resource_filesystem := EditorInterface.get_resource_filesystem()
	for frame in 20:
		await get_tree().process_frame
	while resource_filesystem != null and resource_filesystem.is_scanning():
		await get_tree().process_frame
	for frame in 10:
		await get_tree().process_frame

	var filesystem := EditorInterface.get_file_system_dock()
	if not _check(filesystem != null, "FileSystemDock is unavailable."):
		return
	var err: Error = filesystem.open_scene_in_level_editor("res://main.tscn")
	if not _check(err == OK, "open_scene_in_level_editor returned %s." % error_string(err)):
		return
	for frame in 30:
		await get_tree().process_frame

	var view := _find_level_view()
	if not _check(view != null and Engine.has_singleton("LevelEditor"), "Level editor view/service is unavailable."):
		return
	var service := Engine.get_singleton("LevelEditor")
	var scene_root: Node = view.get_meta("_level_document_root", null)
	var container := view.find_child("LevelViewportContainer", true, false) as Control
	var camera := view.find_child("LevelCamera3D", true, false) as Camera3D
	if not _check(scene_root != null and container != null and camera != null, "The level root, pane, or camera is missing."):
		return

	service.set_tool_mode(TOOL_SELECT)
	container.grab_focus()
	camera.position = Vector3(0, 0, 12)
	camera.look_at_from_position(camera.position, Vector3.ZERO, Vector3.UP)
	camera.fov = 50.0
	var block := _make_block()
	if failed or block == null:
		return
	scene_root.add_child(block, true)
	block.owner = scene_root
	for frame in 5:
		await get_tree().process_frame
	var input_ready_error: String = await SmokeInputReady.wait_for_level_view(get_tree(), view, container, scene_root)
	if not _check(input_ready_error.is_empty(), input_ready_error):
		return

	var mesh := block.get_level_mesh()
	var host_viewport := container.get_viewport()
	var undo_manager := EditorInterface.get_editor_undo_redo()
	var history_id: int = undo_manager.get_object_history_id(scene_root)
	var history: UndoRedo = undo_manager.get_history_undo_redo(history_id)
	if not _check(history != null, "The level document undo history is unavailable."):
		return

	await _send_key(host_viewport, KEY_3)
	var face_screen := _global_screen(container, camera, Vector3(0, 0, 1))
	await _send_click(host_viewport, face_screen)
	var entries := _face_entries(view)
	if not _check(entries.size() == 1, "Face selection did not resolve through the pane input route."):
		return
	var face_id := mesh.resolve_face(int(entries[0].handle))
	if not _check(face_id >= 0, "Selected face handle is stale."):
		return
	var loop_start: int = block.data.face_loop_starts[face_id]
	var loop_count: int = block.data.face_loop_counts[face_id]

	# (a) Square accept is one document action; one undo restores exact UV bytes.
	var before_square := _uv_columns_bytes(block.data)
	var square_version := history.get_version()
	var session := await _open_overlay(view, host_viewport)
	if failed or session == null:
		return
	if not _check(session.set_mode(MODE_SQUARE, SPACING_LENGTH), "Square mode was rejected."):
		return
	await _send_key(host_viewport, KEY_ENTER)
	if not _check(not view.get_meta("_level_fast_texture_open", true) and
			history.get_version() == square_version + 1,
			"Square accept did not close and create exactly one undo action."):
		return
	history.undo()
	if not _check(_uv_columns_bytes(block.data) == before_square,
			"One undo did not restore the pre-session UV columns byte-identically."):
		return

	# (b) Conforming runs only on the clone; cancel leaves every mesh column and history untouched.
	var before_cancel := _mesh_bytes(block.data)
	var cancel_version := history.get_version()
	container.grab_focus()
	await get_tree().process_frame
	session = await _open_overlay(view, host_viewport)
	if failed or session == null:
		return
	if not _check(session.set_mode(MODE_CONFORMING, SPACING_LENGTH), "Conforming mode was rejected."):
		return
	await _send_key(host_viewport, KEY_ESCAPE)
	if not _check(_mesh_bytes(block.data) == before_cancel and history.get_version() == cancel_version,
			"Cancel mutated the mesh or added an undo entry."):
		return

	# (c) The composed translation is visible in the working copy and folds into PROJECTED output.
	container.grab_focus()
	await get_tree().process_frame
	var nudge_version := history.get_version()
	session = await _open_overlay(view, host_viewport)
	if failed or session == null:
		return
	if not _check(session.set_mode(MODE_SQUARE, SPACING_LENGTH), "Square mode was rejected before nudge."):
		return
	var mode_uvs: PackedVector2Array = session.get_working_loop_uvs()
	var shift := Vector2(0.375, -0.625)
	if not _check(session.set_nudge(Transform2D(0.0, shift)), "Translation nudge was rejected."):
		return
	var shifted_uvs: PackedVector2Array = session.get_working_loop_uvs()
	for corner in loop_count:
		var loop_id := loop_start + corner
		if not _check(shifted_uvs[loop_id].is_equal_approx(mode_uvs[loop_id] + shift),
				"Working-copy nudge did not shift loop %d." % loop_id):
			return
	await _send_key(host_viewport, KEY_ENTER)
	if not _check(history.get_version() == nudge_version + 1 and
			block.data.get_face_uv_mode(face_id) == LevelMeshData.UV_MODE_PROJECTED,
			"Projected nudge did not accept as one action."):
		return
	for corner in loop_count:
		var loop_id := loop_start + corner
		var vertex_id: int = block.data.loop_vertex_indices[loop_id]
		var resolved_uv: Vector2 = mesh.get_uv(face_id, block.data.vertex_positions[vertex_id], loop_id)
		if not _check(resolved_uv.is_equal_approx(mode_uvs[loop_id] + shift),
				"Projected get_uv did not include the composed nudge at loop %d." % loop_id):
			return
	history.undo()

	# (d) Empty selection is rejected; Escape pops modal routing and normal picking works afterward.
	container.grab_focus()
	await get_tree().process_frame
	await _send_key(host_viewport, KEY_ESCAPE)
	if not _check(_face_entries(view).is_empty(), "Idle Escape did not clear the face selection."):
		return
	var empty_version := history.get_version()
	await _send_key(host_viewport, KEY_Q, true)
	if not _check(not view.get_meta("_level_fast_texture_open", false) and history.get_version() == empty_version,
			"Shift+Q opened or edited history with an empty selection."):
		return
	await _send_click(host_viewport, face_screen)
	if not _check(_face_entries(view).size() == 1, "Normal picking did not work after the rejected open."):
		return
	session = await _open_overlay(view, host_viewport)
	if failed or session == null:
		return
	await _send_key(host_viewport, KEY_ESCAPE)
	if not _check(not view.get_meta("_level_fast_texture_open", true) and
			not view.get_meta("_level_fast_texture_input_context", true),
			"Escape did not close Fast Texture and pop its input context."):
		return
	await _send_key(host_viewport, KEY_ESCAPE)
	await _send_click(host_viewport, face_screen)
	if not _check(_face_entries(view).size() == 1,
			"A normal selection click did not route after Fast Texture closed."):
		return

	# (e) Square -> Planar recomputes from the immutable original, matching a fresh Planar session.
	var expected_mesh := LevelMesh.new()
	expected_mesh.data = block.data.duplicate_data()
	var expected_session := FastTextureSession.new()
	expected_session.mesh = expected_mesh
	if not _check(expected_session.open(PackedInt32Array([face_id])) and
			expected_session.set_mode(MODE_PLANAR, SPACING_LENGTH),
			"Fresh expected Planar session failed."):
		return
	var expected_working: LevelMeshData = expected_session.get_working_data()
	var expected_face := _face_uv_bytes(expected_working, face_id)
	container.grab_focus()
	await get_tree().process_frame
	session = await _open_overlay(view, host_viewport)
	if failed or session == null:
		return
	if not _check(session.set_mode(MODE_SQUARE, SPACING_LENGTH) and
			session.set_mode(MODE_PLANAR, SPACING_LENGTH),
			"Square then Planar mode sequence was rejected."):
		return
	if not _check(_face_uv_bytes(session.get_working_data(), face_id) == expected_face,
			"Planar after Square did not match a fresh Planar working copy."):
		return
	await _send_key(host_viewport, KEY_ENTER)
	if not _check(_face_uv_bytes(block.data, face_id) == expected_face,
			"Accepted Square then Planar result differs from fresh Planar."):
		return
	expected_session.cancel()

	print("FAST_TEXTURE_EDITOR_SMOKE_OK square=one-action+exact-undo conforming=cancel-exact nudge=projected routing=restored modes=original-snapshot")
