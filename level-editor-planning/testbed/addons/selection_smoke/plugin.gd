@tool
extends EditorPlugin


const TOOL_SELECT := 0
const MODE_VERTEX := 0
const MODE_EDGE := 1
const MODE_FACE := 2
const MODE_OBJECT := 3
const TIER_POLYGROUP := 0
const TIER_TRIANGLE := 1
const FEATURE_VERTEX := 0
const FEATURE_EDGE := 1
const FEATURE_FACE := 2

var failed := false


func _enter_tree() -> void:
	_run_test()


func _fail(message: String) -> void:
	if failed:
		return
	failed = true
	push_error("SELECTION_SMOKE: " + message)


func _check(condition: bool, message: String) -> bool:
	if not condition:
		_fail(message)
	return condition


func _find_level_view() -> Control:
	return EditorInterface.get_base_control().find_child("LevelEditorView", true, false) as Control


func _make_block(block_name: String, center: Vector3) -> LevelBlock:
	var mesh := LevelMesh.new()
	mesh.begin_transaction()
	if not mesh.create_box(Transform3D(Basis.IDENTITY, center), Vector3(2, 2, 2), 0):
		_fail("Kernel create_box rejected %s." % block_name)
		return null
	var diff: LevelMeshDiff = mesh.commit()
	if diff == null:
		_fail("Kernel commit returned no diff for %s." % block_name)
		return null
	var block := LevelBlock.new()
	block.name = block_name
	block.data = mesh.data
	return block


func _send_key(host_viewport: Viewport, keycode: Key, shift := false, ctrl := false) -> void:
	var event := InputEventKey.new()
	event.keycode = keycode
	event.physical_keycode = keycode
	event.shift_pressed = shift
	event.ctrl_pressed = ctrl
	event.pressed = true
	host_viewport.push_input(event, true)
	await get_tree().process_frame


func _send_click(host_viewport: Viewport, position: Vector2, shift := false, ctrl := false, double_click := false) -> void:
	var press := InputEventMouseButton.new()
	press.position = position
	press.global_position = position
	press.button_index = MOUSE_BUTTON_LEFT
	press.button_mask = MOUSE_BUTTON_MASK_LEFT
	press.shift_pressed = shift
	press.ctrl_pressed = ctrl
	press.double_click = double_click
	press.pressed = true
	host_viewport.push_input(press, true)
	await get_tree().process_frame
	var release := InputEventMouseButton.new()
	release.position = position
	release.global_position = position
	release.button_index = MOUSE_BUTTON_LEFT
	release.button_mask = 0
	release.shift_pressed = shift
	release.ctrl_pressed = ctrl
	release.double_click = double_click
	release.pressed = false
	host_viewport.push_input(release, true)
	await get_tree().process_frame


func _send_marquee(host_viewport: Viewport, from: Vector2, to: Vector2) -> void:
	var press := InputEventMouseButton.new()
	press.position = from
	press.global_position = from
	press.button_index = MOUSE_BUTTON_LEFT
	press.button_mask = MOUSE_BUTTON_MASK_LEFT
	press.pressed = true
	host_viewport.push_input(press, true)
	await get_tree().process_frame
	var motion := InputEventMouseMotion.new()
	motion.position = to
	motion.global_position = to
	motion.relative = to - from
	motion.button_mask = MOUSE_BUTTON_MASK_LEFT
	host_viewport.push_input(motion, true)
	await get_tree().process_frame
	var release := InputEventMouseButton.new()
	release.position = to
	release.global_position = to
	release.button_index = MOUSE_BUTTON_LEFT
	release.button_mask = 0
	release.pressed = false
	host_viewport.push_input(release, true)
	await get_tree().process_frame


func _global_screen(container: Control, camera: Camera3D, world_position: Vector3) -> Vector2:
	return container.get_global_transform_with_canvas() * camera.unproject_position(world_position)


func _entries(view: Control, feature: int) -> Array:
	var result: Array = []
	for entry: Dictionary in view.get_meta("_level_selection_entries", []):
		if int(entry.get("feature", -1)) == feature:
			result.append(entry)
	return result


func _all_entries_belong_to(entries: Array, block: Node) -> bool:
	for entry: Dictionary in entries:
		if int(entry.get("block_id", 0)) != block.get_instance_id():
			return false
	return true


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
	if not _check(view != null, "No LevelEditorView was minted."):
		return
	var scene_root: Node = view.get_meta("_level_document_root", null)
	var container := view.find_child("LevelViewportContainer", true, false) as Control
	var camera := view.find_child("LevelCamera3D", true, false) as Camera3D
	if not _check(scene_root != null and container != null and camera != null,
			"The level document root, viewport container, or camera is missing."):
		return
	if not _check(Engine.has_singleton("LevelEditor"), "The LevelEditor singleton is unavailable."):
		return
	Engine.get_singleton("LevelEditor").set_tool_mode(TOOL_SELECT)
	container.grab_focus()
	camera.position = Vector3(0, 0, 12)
	camera.look_at_from_position(camera.position, Vector3.ZERO, Vector3.UP)
	camera.fov = 50.0
	await get_tree().process_frame

	var block_a := _make_block("SelectionBlockA", Vector3(-2, 0, 0))
	var block_b := _make_block("SelectionBlockB", Vector3(2, 0, 0))
	if failed or block_a == null or block_b == null:
		return
	scene_root.add_child(block_a, true)
	block_a.owner = scene_root

	var undo_manager := EditorInterface.get_editor_undo_redo()
	undo_manager.create_action("Add Selection Smoke Block", UndoRedo.MERGE_DISABLE, scene_root)
	undo_manager.add_do_method(scene_root, &"add_child", block_b, true)
	undo_manager.add_do_method(block_b, &"set_owner", scene_root)
	undo_manager.add_do_reference(block_b)
	undo_manager.add_undo_method(scene_root, &"remove_child", block_b)
	undo_manager.commit_action()
	for frame in 5:
		await get_tree().process_frame

	var host_viewport := container.get_viewport()
	var mesh_a := LevelMesh.new()
	mesh_a.data = block_a.data
	var face_center_a := _global_screen(container, camera, Vector3(-2, 0, 1))
	await _send_key(host_viewport, KEY_3)
	await _send_click(host_viewport, face_center_a)
	var face_entries := _entries(view, FEATURE_FACE)
	if not _check(int(view.get_meta("_level_selection_mode", -1)) == MODE_FACE and
			int(view.get_meta("_level_selection_tier", -1)) == TIER_POLYGROUP,
			"Face hotkey did not select polygroup-tier face mode."):
		return
	# Box faces are one polygroup EACH (non-coplanar faces never share a group), so a
	# polygroup-tier face click selects exactly the picked +Z face.
	if not _check(face_entries.size() == 1 and _all_entries_belong_to(face_entries, block_a),
			"Face click did not select exactly the picked face's polygroup."):
		return
	var active_face: Dictionary = view.get_meta("_level_selection_active", {})
	if not _check(int(active_face.get("handle", -1)) == mesh_a.make_face_handle(1) and
			int(face_entries[0].handle) == mesh_a.make_face_handle(1),
			"The picked +Z face is not the selected/active face."):
		return

	await _send_key(host_viewport, KEY_2)
	var edge_position := _global_screen(container, camera, Vector3(-2, 1, 1)) + Vector2(0, 4)
	await _send_click(host_viewport, edge_position)
	var edge_entries := _entries(view, FEATURE_EDGE)
	if not _check(edge_entries.size() == 1 and int(edge_entries[0].handle) == mesh_a.make_edge_handle(6),
			"8 px edge-tolerance pick did not resolve the exact top-front edge handle."):
		return

	await _send_key(host_viewport, KEY_1)
	# Keep the ray on the visible face within the distance-scaled depth bias while
	# still proving the pick is pixel-tolerant rather than coordinate-exact.
	var vertex_7_position := _global_screen(container, camera, Vector3(-3, 1, 1)) + Vector2(1, 1)
	var vertex_6_position := _global_screen(container, camera, Vector3(-1, 1, 1)) + Vector2(-1, 1)
	await _send_click(host_viewport, vertex_7_position)
	var vertex_entries := _entries(view, FEATURE_VERTEX)
	if not _check(vertex_entries.size() == 1 and int(vertex_entries[0].handle) == mesh_a.make_vertex_handle(7),
			"10 px vertex-tolerance pick did not resolve the exact top-left-front vertex handle (got %s, expected %s)." %
					[vertex_entries, mesh_a.make_vertex_handle(7)]):
		return
	await _send_click(host_viewport, vertex_6_position, true)
	if not _check(_entries(view, FEATURE_VERTEX).size() == 2, "Shift-click did not add a second vertex."):
		return
	await _send_click(host_viewport, vertex_7_position, false, true)
	vertex_entries = _entries(view, FEATURE_VERTEX)
	if not _check(vertex_entries.size() == 1 and int(vertex_entries[0].handle) == mesh_a.make_vertex_handle(6),
			"Ctrl-click did not toggle the first vertex out of the ordered set."):
		return

	var projected: Array[Vector2] = []
	for position: Vector3 in block_a.data.vertex_positions:
		projected.append(camera.unproject_position(position))
	var min_screen := projected[0]
	var max_screen := projected[0]
	for point in projected:
		min_screen = min_screen.min(point)
		max_screen = max_screen.max(point)
	var marquee_from: Vector2 = container.get_global_transform_with_canvas() * (min_screen - Vector2(12, 12))
	var marquee_to: Vector2 = container.get_global_transform_with_canvas() * (max_screen + Vector2(12, 12))
	await _send_marquee(host_viewport, marquee_from, marquee_to)
	vertex_entries = _entries(view, FEATURE_VERTEX)
	if not _check(vertex_entries.size() == 8 and _all_entries_belong_to(vertex_entries, block_a),
			"X-ray marquee did not enclose all eight vertices of only the first block."):
		return
	await _send_key(host_viewport, KEY_I, false, true)
	vertex_entries = _entries(view, FEATURE_VERTEX)
	if not _check(vertex_entries.size() == 8 and _all_entries_belong_to(vertex_entries, block_b),
			"Ctrl+I did not invert the current vertex selection to only the other block."):
		return
	await _send_key(host_viewport, KEY_A, false, true)
	vertex_entries = _entries(view, FEATURE_VERTEX)
	if not _check(vertex_entries.size() == 16,
			"Ctrl+A did not select every vertex in the current mode."):
		return

	await _send_key(host_viewport, KEY_3, true, true)
	await _send_click(host_viewport, face_center_a, false, false, true)
	face_entries = _entries(view, FEATURE_FACE)
	if not _check(int(view.get_meta("_level_selection_tier", -1)) == TIER_TRIANGLE and
			face_entries.size() == 2 and view.get_meta("_level_last_selection_action", &"") == &"flood",
			"Double-click flood did not select both fan triangles of the contiguous coplanar face."):
		return

	await _send_key(host_viewport, KEY_2)
	await _send_click(host_viewport, edge_position)
	await _send_key(host_viewport, KEY_L)
	if not _check(_entries(view, FEATURE_EDGE).size() == 1 and
			view.get_meta("_level_last_selection_action", &"") == &"loop",
			"L walk did not terminate at the box edge's valence-3 endpoints."):
		return
	await _send_key(host_viewport, KEY_X)
	if not _check(_entries(view, FEATURE_EDGE).size() == 4 and
			view.get_meta("_level_last_selection_action", &"") == &"ring",
			"X ring did not return the four-edge parallel box band and terminate."):
		return

	await _send_key(host_viewport, KEY_4)
	await _send_click(host_viewport, face_center_a)
	var object_selection := EditorInterface.get_selection().get_selected_nodes()
	if not _check(object_selection.size() == 1 and object_selection[0] == block_a,
			"Object-mode click did not route the LevelBlock through EditorSelection."):
		return

	await _send_key(host_viewport, KEY_3)
	var face_center_b := _global_screen(container, camera, Vector3(2, 0, 1))
	await _send_click(host_viewport, face_center_b)
	if not _check(_entries(view, FEATURE_FACE).size() == 1 and
			_all_entries_belong_to(_entries(view, FEATURE_FACE), block_b),
			"The second block was not selected before undo revalidation."):
		return
	var history_id: int = undo_manager.get_object_history_id(scene_root)
	var history: UndoRedo = undo_manager.get_history_undo_redo(history_id)
	if not _check(history != null, "The level document undo history is unavailable."):
		return
	history.undo()
	for frame in 3:
		await get_tree().process_frame
	if not _check(block_b.get_parent() == null and _entries(view, FEATURE_FACE).is_empty(),
			"Undoing block creation left stale sub-object handles in SelectionModel."):
		return

	print("SELECTION_SMOKE_OK")
