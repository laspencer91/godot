@tool
extends EditorPlugin


const SmokeInputReady = preload("res://addons/smoke_input_ready.gd")
const TOOL_SELECT := 0
const FEATURE_VERTEX := 0
const FEATURE_FACE := 2
const ROTATION_EPSILON := 0.0001

var failed := false


func _enter_tree() -> void:
	_run_test()


func _fail(message: String) -> void:
	if failed:
		return
	failed = true
	push_error("TRANSFORM_SMOKE: " + message)


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
	block.name = "TransformSmokeBlock"
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


func _send_click(host_viewport: Viewport, position: Vector2, ctrl := false) -> void:
	var press := InputEventMouseButton.new()
	press.position = position
	press.global_position = position
	press.button_index = MOUSE_BUTTON_LEFT
	press.button_mask = MOUSE_BUTTON_MASK_LEFT
	press.ctrl_pressed = ctrl
	press.pressed = true
	host_viewport.push_input(press, true)
	await get_tree().process_frame
	var release := InputEventMouseButton.new()
	release.position = position
	release.global_position = position
	release.button_index = MOUSE_BUTTON_LEFT
	release.button_mask = 0
	release.ctrl_pressed = ctrl
	release.pressed = false
	host_viewport.push_input(release, true)
	await get_tree().process_frame


func _send_motion(host_viewport: Viewport, from: Vector2, to: Vector2, ctrl := false) -> void:
	var motion := InputEventMouseMotion.new()
	motion.position = to
	motion.global_position = to
	motion.relative = to - from
	motion.button_mask = 0
	motion.ctrl_pressed = ctrl
	host_viewport.push_input(motion, true)
	await get_tree().process_frame


func _send_view_motion(container: Control, from: Vector2, to: Vector2, ctrl := false) -> void:
	var motion := InputEventMouseMotion.new()
	motion.position = to
	motion.global_position = to
	motion.relative = to - from
	motion.button_mask = 0
	motion.ctrl_pressed = ctrl
	container.emit_signal(&"gui_input", motion)
	await get_tree().process_frame


func _send_drag(host_viewport: Viewport, from: Vector2, to: Vector2, shift := false, ctrl := false,
		cancel := false, mid_drag_key := KEY_NONE, mid_drag_key_presses := 0,
		mid_drag_keys: Array[Key] = []) -> void:
	var press := InputEventMouseButton.new()
	press.position = from
	press.global_position = from
	press.button_index = MOUSE_BUTTON_LEFT
	press.button_mask = MOUSE_BUTTON_MASK_LEFT
	press.shift_pressed = shift
	press.ctrl_pressed = ctrl
	press.pressed = true
	host_viewport.push_input(press, true)
	await get_tree().process_frame
	var motion := InputEventMouseMotion.new()
	motion.position = to
	motion.global_position = to
	motion.relative = to - from
	motion.button_mask = MOUSE_BUTTON_MASK_LEFT
	motion.shift_pressed = shift
	motion.ctrl_pressed = ctrl
	host_viewport.push_input(motion, true)
	await get_tree().process_frame
	for press_index in mid_drag_key_presses:
		await _send_key(host_viewport, mid_drag_key)
	for extra_key in mid_drag_keys:
		await _send_key(host_viewport, extra_key)
	if cancel:
		await _send_key(host_viewport, KEY_ESCAPE)
	var release := InputEventMouseButton.new()
	release.position = to
	release.global_position = to
	release.button_index = MOUSE_BUTTON_LEFT
	release.button_mask = 0
	release.shift_pressed = shift
	release.ctrl_pressed = ctrl
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
			"The level root, viewport container, or camera is missing."):
		return
	var level_editor := Engine.get_singleton("LevelEditor")
	level_editor.set_tool_mode(TOOL_SELECT)
	level_editor.snap_step = 1.0
	level_editor.snap_enabled = true
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
	var mesh := LevelMesh.new()
	mesh.data = block.data
	var host_viewport := container.get_viewport()
	var undo_manager := EditorInterface.get_editor_undo_redo()
	var history_id: int = undo_manager.get_object_history_id(scene_root)
	var history: UndoRedo = undo_manager.get_history_undo_redo(history_id)
	if not _check(history != null, "The level document undo history is unavailable."):
		return

	# Snapped free move starts from the selected vertex. The target is +1.2 m,
	# so snap-the-delta must land at exactly +1 m without absolute grid reseeding.
	await _send_key(host_viewport, KEY_1)
	# Bias one pixel into the visible face, matching selection_smoke's depth-safe
	# corner pick while remaining well inside the 10 px vertex tolerance.
	var vertex_screen := _global_screen(container, camera, Vector3(1, 1, 1)) + Vector2(-1, 1)
	await _send_click(host_viewport, vertex_screen)
	if not _check(_entries(view, FEATURE_VERTEX).size() == 1, "Vertex selection did not resolve."):
		return
	var target_screen := _global_screen(container, camera, Vector3(2.2, 1, 1)) + Vector2(-1, 1)
	await _send_drag(host_viewport, vertex_screen, target_screen)
	if not _check(block.data.vertex_positions[6].is_equal_approx(Vector3(2, 1, 1)),
			"Snapped vertex drag did not land at the relative +1 m position (got %s)." % block.data.vertex_positions[6]):
		return

	# Escape restores the complete preview snapshot and must not add history.
	var cancel_snapshot: PackedVector3Array = block.data.vertex_positions
	var cancel_version: int = history.get_version()
	var moved_screen := _global_screen(container, camera, block.data.vertex_positions[6]) + Vector2(-1, 1)
	var cancel_target := _global_screen(container, camera, block.data.vertex_positions[6] + Vector3(2.1, 0, 0)) + Vector2(-1, 1)
	await _send_drag(host_viewport, moved_screen, cancel_target, false, false, true)
	if not _check(block.data.vertex_positions == cancel_snapshot and history.get_version() == cancel_version,
			"Escape changed mesh data or created an undo entry."):
		return

	# Active move constraints re-solve the original press against the selected
	# world axis. A second press cycles the same key to the excluding plane.
	var axis_origin: Vector3 = block.data.vertex_positions[6]
	var axis_screen := _global_screen(container, camera, axis_origin) + Vector2(-1, 1)
	var axis_target := _global_screen(container, camera, axis_origin + Vector3(1.2, 1.2, 0)) + Vector2(-1, 1)
	await _send_drag(host_viewport, axis_screen, axis_target, false, false, false, KEY_X, 1)
	var axis_delta: Vector3 = block.data.vertex_positions[6] - axis_origin
	if not _check(axis_delta.x != 0.0 and axis_delta.y == 0.0 and axis_delta.z == 0.0,
			"X axis-lock drag did not commit an X-only delta (got %s)." % axis_delta):
		return
	var plane_origin: Vector3 = block.data.vertex_positions[6]
	var plane_screen := _global_screen(container, camera, plane_origin) + Vector2(-1, 1)
	var plane_target := _global_screen(container, camera, plane_origin + Vector3(1.2, 1.2, 0)) + Vector2(-1, 1)
	await _send_drag(host_viewport, plane_screen, plane_target, false, false, false, KEY_X, 2)
	var plane_delta: Vector3 = block.data.vertex_positions[6] - plane_origin
	if not _check(plane_delta.x == 0.0,
			"X plane-lock drag did not exclude X (got %s)." % plane_delta):
		return
	history.undo()
	history.undo()
	for frame in 3:
		await get_tree().process_frame

	# Polygroup tier is the DEFAULT face mode and the gesture users actually make:
	# each box face is its own polygroup, so a click selects exactly the hit face.
	# Shift-drag performs zero-distance topology creation plus one cap preview, but
	# records both diffs as a single editor undo action.
	await _send_key(host_viewport, KEY_3)
	var face_screen := _global_screen(container, camera, Vector3(0, 0, 1))
	await _send_click(host_viewport, face_screen)
	if not _check(_entries(view, FEATURE_FACE).size() == 1, "Polygroup-tier face selection did not resolve to the hit face."):
		return
	var cap_handle: int = int(_entries(view, FEATURE_FACE)[0].handle)
	var extrude_version: int = history.get_version()
	await _send_drag(host_viewport, face_screen, face_screen + Vector2(0, -80), true)
	if not _check(block.data.face_count() == 10 and history.get_version() == extrude_version + 1,
			"Face extrude did not add four walls as one undo action."):
		return
	if not _check(mesh.resolve_face(cap_handle) >= 0, "Extrude invalidated the selected cap handle."):
		return

	history.undo()
	for frame in 3:
		await get_tree().process_frame
	var no_stale_handles := true
	for entry: Dictionary in _entries(view, FEATURE_FACE):
		no_stale_handles = no_stale_handles and mesh.resolve_face(int(entry.handle)) >= 0
	if not _check(block.data.vertex_count() == 8 and block.data.edge_count() == 12 and
			block.data.face_count() == 6 and no_stale_handles,
			"Undo after extrude did not restore topology or left stale SelectionModel handles."):
		return

	# Each nudge press is its own committed grid-step diff. Ctrl during a later
	# drag disables snapping, and Ctrl+Shift+G then performs absolute point snap.
	await _send_key(host_viewport, KEY_1)
	var current_vertex: Vector3 = block.data.vertex_positions[6]
	var current_screen := _global_screen(container, camera, current_vertex) + Vector2(-1, 1)
	await _send_click(host_viewport, current_screen)
	var nudge_version: int = history.get_version()
	await _send_key(host_viewport, KEY_RIGHT)
	if not _check(block.data.vertex_positions[6].is_equal_approx(current_vertex + Vector3.RIGHT) and
			history.get_version() == nudge_version + 1,
			"Right-arrow nudge did not commit exactly one +1 m step."):
		return
	current_vertex = block.data.vertex_positions[6]
	current_screen = _global_screen(container, camera, current_vertex) + Vector2(-1, 1)
	var unsnapped_target := _global_screen(container, camera, current_vertex + Vector3(0.35, 0, 0)) + Vector2(-1, 1)
	await _send_drag(host_viewport, current_screen, unsnapped_target, false, true)
	var unsnapped_x: float = block.data.vertex_positions[6].x
	await _send_key(host_viewport, KEY_G, true, true)
	if not _check(not is_equal_approx(unsnapped_x, round(unsnapped_x)) and
			is_equal_approx(block.data.vertex_positions[6].x, round(block.data.vertex_positions[6].x)),
			"Ctrl snap inversion or Vertices-to-Grid absolute snap failed."):
		return

	# Object-tier Ctrl+D creates an independent LevelMeshData resource through
	# standard node undo, then one undo removes only the duplicate.
	await _send_key(host_viewport, KEY_4)
	face_screen = _global_screen(container, camera, Vector3(0, 0, 1))
	await _send_click(host_viewport, face_screen)
	await _send_key(host_viewport, KEY_D, false, true)
	var blocks: Array[LevelBlock] = []
	for child in scene_root.get_children():
		if child is LevelBlock:
			blocks.append(child)
	if not _check(blocks.size() == 2 and blocks[0].data != blocks[1].data,
			"Object Ctrl+D did not create an independent LevelBlock data resource."):
		return
	history.undo()
	for frame in 3:
		await get_tree().process_frame
	blocks.clear()
	for child in scene_root.get_children():
		if child is LevelBlock:
			blocks.append(child)
	if not _check(blocks.size() == 1, "Undo after object duplicate did not remove only the duplicate."):
		return

	# Modal rotation preserves the snapped screen-space sweep when X swaps the
	# camera-view axis for world X. This makes the closed-form 45-degree result
	# deterministic without re-zeroing at the constraint change.
	await _send_key(host_viewport, KEY_3)
	var rotation_pick_screen := _global_screen(container, camera, Vector3(0, -0.35, 1))
	await _send_click(host_viewport, rotation_pick_screen)
	var rotation_entries := _entries(view, FEATURE_FACE)
	if not _check(rotation_entries.size() == 1, "Rotation face selection did not resolve."):
		return
	var rotation_face_id: int = mesh.resolve_face(int(rotation_entries[0].handle))
	var rotation_vertex_ids: PackedInt32Array = mesh.get_face_corner_vertex_ids(rotation_face_id)
	if not _check(rotation_face_id >= 0 and rotation_vertex_ids.size() >= 3,
			"Rotation face handle or corners were invalid."):
		return
	var rotation_originals: PackedVector3Array = block.data.vertex_positions
	var rotation_pivot := Vector3.ZERO
	for vertex_id in rotation_vertex_ids:
		rotation_pivot += block.global_transform * rotation_originals[vertex_id]
	rotation_pivot /= float(rotation_vertex_ids.size())
	var rotation_pivot_screen := _global_screen(container, camera, rotation_pivot)
	var rotation_radius := 100.0
	var rotation_start := rotation_pick_screen
	var rotation_press_vector := rotation_start - rotation_pivot_screen
	if not _check(rotation_press_vector.length() > 4.0, "Rotation press reference was too close to the pivot."):
		return
	var raw_sweep := deg_to_rad(47.0)
	var rotation_target := rotation_pivot_screen + rotation_press_vector.normalized().rotated(raw_sweep) * rotation_radius
	host_viewport.warp_mouse(rotation_start)
	await get_tree().process_frame
	level_editor.snap_enabled = false
	var rotation_version: int = history.get_version()
	await _send_key(host_viewport, KEY_R)
	await _send_motion(host_viewport, rotation_start, rotation_target, true)
	await _send_key(host_viewport, KEY_X)
	await _send_click(host_viewport, rotation_target, true)

	var expected_positions: PackedVector3Array = rotation_originals.duplicate()
	var expected_rotation := Basis(Vector3.RIGHT, deg_to_rad(45.0))
	var block_inverse := block.global_transform.affine_inverse()
	for vertex_id in rotation_vertex_ids:
		var original_world: Vector3 = block.global_transform * rotation_originals[vertex_id]
		var expected_world: Vector3 = rotation_pivot + expected_rotation * (original_world - rotation_pivot)
		expected_positions[vertex_id] = block_inverse * expected_world
	var rotation_matches := true
	var distances_preserved := true
	for vertex_id in block.data.vertex_positions.size():
		rotation_matches = rotation_matches and block.data.vertex_positions[vertex_id].distance_to(
				expected_positions[vertex_id]) <= ROTATION_EPSILON
	for vertex_id in rotation_vertex_ids:
		var before_distance: float = (block.global_transform * rotation_originals[vertex_id]).distance_to(rotation_pivot)
		var after_distance: float = (block.global_transform * block.data.vertex_positions[vertex_id]).distance_to(rotation_pivot)
		distances_preserved = distances_preserved and abs(after_distance - before_distance) <= ROTATION_EPSILON
	if not _check(rotation_matches and distances_preserved and history.get_version() == rotation_version + 1,
			"Snapped world-X rotation mismatch (positions=%s radii=%s version=%d->%d actual=%s expected=%s)." % [
					rotation_matches, distances_preserved, rotation_version, history.get_version(),
					block.data.vertex_positions, expected_positions]):
		return
	history.undo()
	for frame in 3:
		await get_tree().process_frame
	if not _check(block.data.vertex_positions == rotation_originals,
			"Undo after modal rotation did not restore byte-identical vertex positions."):
		return
	container.grab_focus()
	await get_tree().process_frame
	await _send_key(host_viewport, KEY_1)
	await _send_key(host_viewport, KEY_3)
	await _send_click(host_viewport, rotation_start)
	if not _check(_entries(view, FEATURE_FACE).size() == 1,
			"Cancel-run face selection did not resolve."):
		return

	# Escape cancels the open kernel preview exactly and adds no history entry.
	var rotation_cancel_snapshot: PackedVector3Array = block.data.vertex_positions.duplicate()
	var rotation_cancel_version: int = history.get_version()
	var rotation_cancel_start := rotation_start
	var rotation_cancel_target := rotation_target
	host_viewport.warp_mouse(rotation_cancel_start)
	await get_tree().process_frame
	await _send_key(host_viewport, KEY_R)
	# After a programmatic undo, emit the container's real gui_input signal so
	# this second unbuttoned motion exercises the view-to-tool seam without
	# depending on headless OS-cursor hit-testing.
	await _send_view_motion(container, rotation_cancel_start, rotation_cancel_target, true)
	if not _check(block.data.vertex_positions != rotation_cancel_snapshot,
			"Modal rotation did not produce a live preview before cancellation."):
		return
	await _send_key(host_viewport, KEY_ESCAPE)
	if not _check(block.data.vertex_positions == rotation_cancel_snapshot and
			history.get_version() == rotation_cancel_version,
			"Escape changed modal-rotation data or created an undo entry."):
		return

	# R is inert during a pointer-driven move; X must remain the move constraint
	# key, yielding one rigid X-only translation rather than a rotation.
	level_editor.snap_enabled = true
	var active_move_originals: PackedVector3Array = block.data.vertex_positions
	var active_move_version: int = history.get_version()
	var active_move_keys: Array[Key] = [KEY_R, KEY_X]
	var active_move_target := rotation_pick_screen + Vector2(90, -45)
	await _send_drag(host_viewport, rotation_pick_screen, active_move_target,
			false, false, false, KEY_NONE, 0, active_move_keys)
	var active_move_delta: Vector3 = block.data.vertex_positions[rotation_vertex_ids[0]] - \
			active_move_originals[rotation_vertex_ids[0]]
	var active_move_is_translation: bool = abs(active_move_delta.x) > ROTATION_EPSILON and \
			abs(active_move_delta.y) <= ROTATION_EPSILON and abs(active_move_delta.z) <= ROTATION_EPSILON
	for vertex_id in rotation_vertex_ids:
		active_move_is_translation = active_move_is_translation and (
				block.data.vertex_positions[vertex_id] - active_move_originals[vertex_id]).distance_to(
				active_move_delta) <= ROTATION_EPSILON
	if not _check(active_move_is_translation and history.get_version() == active_move_version + 1,
			"R disrupted an active move drag or prevented the X constraint."):
		return
	history.undo()
	for frame in 3:
		await get_tree().process_frame
	if not _check(block.data.vertex_positions == active_move_originals,
			"Undo after the active-move R guard did not restore exact positions."):
		return

	# Persistent transform gizmo: an X-arrow drag enters the existing move
	# lifecycle with an axis constraint already selected. Relative snapping,
	# one-action history, and exact undo remain the same as a bare drag.
	level_editor.snap_enabled = true
	for frame in 3:
		await get_tree().process_frame
	if not _check(bool(view.get_meta("_level_transform_gizmo_visible", false)),
			"Transform gizmo was not visible for the selected face."):
		return
	var gizmo_pivot: Vector3 = view.get_meta("_level_transform_gizmo_pivot", Vector3.ZERO)
	var gizmo_scale: float = float(view.get_meta("_level_transform_gizmo_scale", 0.0))
	var arrow_offset: float = float(view.get_meta("_level_transform_gizmo_arrow_offset", 0.0))
	var gizmo_move_originals: PackedVector3Array = block.data.vertex_positions.duplicate()
	var gizmo_move_version: int = history.get_version()
	var x_arrow_world := gizmo_pivot + Vector3.RIGHT * gizmo_scale * arrow_offset
	var x_arrow_screen := _global_screen(container, camera, x_arrow_world)
	var x_arrow_target := _global_screen(container, camera, x_arrow_world + Vector3(1.2, 0, 0))
	await _send_drag(host_viewport, x_arrow_screen, x_arrow_target)
	var arrow_move_matches := true
	var arrow_move_snapped := true
	for vertex_id in block.data.vertex_positions.size():
		var before_world: Vector3 = block.global_transform * gizmo_move_originals[vertex_id]
		var after_world: Vector3 = block.global_transform * block.data.vertex_positions[vertex_id]
		var world_delta := after_world - before_world
		if rotation_vertex_ids.has(vertex_id):
			arrow_move_matches = arrow_move_matches and abs(world_delta.x) > ROTATION_EPSILON and \
					abs(world_delta.y) <= ROTATION_EPSILON and abs(world_delta.z) <= ROTATION_EPSILON
			arrow_move_snapped = arrow_move_snapped and abs(world_delta.x - round(world_delta.x)) <= ROTATION_EPSILON
		else:
			arrow_move_matches = arrow_move_matches and world_delta.length() <= ROTATION_EPSILON
	if not _check(arrow_move_matches and arrow_move_snapped and history.get_version() == gizmo_move_version + 1,
			"X-arrow drag was not a snapped X-only move with one undo action."):
		return
	history.undo()
	for frame in 3:
		await get_tree().process_frame
	if not _check(block.data.vertex_positions == gizmo_move_originals,
			"Undo after X-arrow drag did not restore exact positions."):
		return

	# The XY quad stores Z as its excluding normal. A diagonal drag must keep
	# every selected vertex's world-Z component unchanged and commit once.
	gizmo_pivot = view.get_meta("_level_transform_gizmo_pivot", Vector3.ZERO)
	gizmo_scale = float(view.get_meta("_level_transform_gizmo_scale", 0.0))
	var plane_offset: float = float(view.get_meta("_level_transform_gizmo_plane_offset", 0.0))
	var gizmo_plane_originals: PackedVector3Array = block.data.vertex_positions.duplicate()
	var gizmo_plane_version: int = history.get_version()
	var xy_plane_world := gizmo_pivot + (Vector3.RIGHT + Vector3.UP) * gizmo_scale * plane_offset
	var xy_plane_screen := _global_screen(container, camera, xy_plane_world)
	var xy_plane_target := _global_screen(container, camera, xy_plane_world + Vector3(1.2, 1.2, 0))
	await _send_drag(host_viewport, xy_plane_screen, xy_plane_target)
	var plane_move_matches := true
	for vertex_id in rotation_vertex_ids:
		var plane_before_world: Vector3 = block.global_transform * gizmo_plane_originals[vertex_id]
		var plane_after_world: Vector3 = block.global_transform * block.data.vertex_positions[vertex_id]
		var gizmo_plane_delta := plane_after_world - plane_before_world
		plane_move_matches = plane_move_matches and abs(gizmo_plane_delta.x) > ROTATION_EPSILON and \
				abs(gizmo_plane_delta.y) > ROTATION_EPSILON and abs(gizmo_plane_delta.z) <= ROTATION_EPSILON
	if not _check(plane_move_matches and history.get_version() == gizmo_plane_version + 1,
			"XY-plane gizmo drag escaped its plane or did not create exactly one undo action."):
		return
	history.undo()
	for frame in 3:
		await get_tree().process_frame
	if not _check(block.data.vertex_positions == gizmo_plane_originals,
			"Undo after plane-handle drag did not restore exact positions."):
		return

	# Grab the Z ring away from the arrow crossings. Ctrl inverts disabled
	# snapping, so a raw 17-degree sweep resolves to an exact 15 degrees.
	level_editor.snap_enabled = false
	gizmo_pivot = view.get_meta("_level_transform_gizmo_pivot", Vector3.ZERO)
	gizmo_scale = float(view.get_meta("_level_transform_gizmo_scale", 0.0))
	var ring_radius: float = float(view.get_meta("_level_transform_gizmo_ring_radius", 0.0))
	var ring_direction := (Vector3.RIGHT + Vector3.UP).normalized()
	var ring_start_world := gizmo_pivot + ring_direction * gizmo_scale * ring_radius
	var ring_start_screen := _global_screen(container, camera, ring_start_world)
	var ring_pivot_screen := _global_screen(container, camera, gizmo_pivot)
	var ring_target_screen := ring_pivot_screen + (ring_start_screen - ring_pivot_screen).rotated(deg_to_rad(17.0))
	var gizmo_rotate_originals: PackedVector3Array = block.data.vertex_positions.duplicate()
	var gizmo_rotate_version: int = history.get_version()
	await _send_drag(host_viewport, ring_start_screen, ring_target_screen, false, true)
	var gizmo_rotate_expected: PackedVector3Array = gizmo_rotate_originals.duplicate()
	var gizmo_rotation := Basis(Vector3(0, 0, 1), deg_to_rad(15.0))
	var gizmo_block_inverse := block.global_transform.affine_inverse()
	for vertex_id in rotation_vertex_ids:
		var gizmo_original_world: Vector3 = block.global_transform * gizmo_rotate_originals[vertex_id]
		var gizmo_expected_world: Vector3 = gizmo_pivot + gizmo_rotation * (gizmo_original_world - gizmo_pivot)
		gizmo_rotate_expected[vertex_id] = gizmo_block_inverse * gizmo_expected_world
	var gizmo_rotation_matches := true
	var gizmo_radii_preserved := true
	for vertex_id in block.data.vertex_positions.size():
		gizmo_rotation_matches = gizmo_rotation_matches and block.data.vertex_positions[vertex_id].distance_to(
				gizmo_rotate_expected[vertex_id]) <= ROTATION_EPSILON
	for vertex_id in rotation_vertex_ids:
		var gizmo_before_distance: float = (block.global_transform * gizmo_rotate_originals[vertex_id]).distance_to(gizmo_pivot)
		var gizmo_after_distance: float = (block.global_transform * block.data.vertex_positions[vertex_id]).distance_to(gizmo_pivot)
		gizmo_radii_preserved = gizmo_radii_preserved and abs(gizmo_after_distance - gizmo_before_distance) <= ROTATION_EPSILON
	if not _check(gizmo_rotation_matches and gizmo_radii_preserved and history.get_version() == gizmo_rotate_version + 1,
			"Ctrl-snapped Z-ring rotation did not match the closed form or one-action history."):
		return
	history.undo()
	for frame in 3:
		await get_tree().process_frame
	if not _check(block.data.vertex_positions == gizmo_rotate_originals,
			"Undo after ring rotation did not restore exact positions."):
		return

	# A second ring gesture cancelled with Escape restores the preview exactly
	# and does not create a history entry.
	gizmo_pivot = view.get_meta("_level_transform_gizmo_pivot", Vector3.ZERO)
	gizmo_scale = float(view.get_meta("_level_transform_gizmo_scale", 0.0))
	ring_start_world = gizmo_pivot + ring_direction * gizmo_scale * ring_radius
	ring_start_screen = _global_screen(container, camera, ring_start_world)
	ring_pivot_screen = _global_screen(container, camera, gizmo_pivot)
	ring_target_screen = ring_pivot_screen + (ring_start_screen - ring_pivot_screen).rotated(deg_to_rad(17.0))
	var gizmo_cancel_originals: PackedVector3Array = block.data.vertex_positions.duplicate()
	var gizmo_cancel_version: int = history.get_version()
	await _send_drag(host_viewport, ring_start_screen, ring_target_screen, false, true, true)
	if not _check(block.data.vertex_positions == gizmo_cancel_originals and history.get_version() == gizmo_cancel_version,
			"Escape on a ring drag changed geometry or created an undo entry."):
		return
	level_editor.snap_enabled = true

	# A non-handle click still replaces selection through the ordinary picker,
	# while a zero-motion click on the X arrow leaves selection and history alone.
	await _send_key(host_viewport, KEY_1)
	var selected_face_center := Vector3.ZERO
	for vertex_id in rotation_vertex_ids:
		selected_face_center += block.global_transform * block.data.vertex_positions[vertex_id]
	selected_face_center /= float(rotation_vertex_ids.size())
	var selected_face_center_screen := _global_screen(container, camera, selected_face_center)
	var first_vertex_id: int = rotation_vertex_ids[0]
	var first_vertex_screen := _global_screen(container, camera,
			block.global_transform * block.data.vertex_positions[first_vertex_id]).lerp(selected_face_center_screen, 0.02)
	await _send_click(host_viewport, first_vertex_screen)
	var first_vertex_entries := _entries(view, FEATURE_VERTEX)
	if not _check(first_vertex_entries.size() == 1, "Precondition vertex for gizmo routing was not selected."):
		return
	for frame in 3:
		await get_tree().process_frame
	var second_vertex_id := first_vertex_id
	var second_vertex_screen := first_vertex_screen
	var farthest_screen_distance := 0.0
	for candidate_id in rotation_vertex_ids:
		var candidate_screen := _global_screen(container, camera,
				block.global_transform * block.data.vertex_positions[candidate_id]).lerp(selected_face_center_screen, 0.02)
		var candidate_distance := candidate_screen.distance_to(first_vertex_screen)
		if candidate_distance > farthest_screen_distance:
			farthest_screen_distance = candidate_distance
			second_vertex_id = candidate_id
			second_vertex_screen = candidate_screen
	await _send_click(host_viewport, second_vertex_screen)
	var second_vertex_entries := _entries(view, FEATURE_VERTEX)
	if not _check(second_vertex_id != first_vertex_id and second_vertex_entries != first_vertex_entries,
			"A non-handle click did not replace the selected vertex normally (ids=%d->%d distance=%f entries=%s->%s)." % [
					first_vertex_id, second_vertex_id, farthest_screen_distance, first_vertex_entries, second_vertex_entries]):
		return
	if second_vertex_entries.is_empty():
		# The farthest projected corner can be outside the deformed face's pick
		# silhouette. Clearing selection is still the expected picker result;
		# reselect the known visible corner to exercise the handle refusal below.
		await _send_click(host_viewport, first_vertex_screen)
		second_vertex_entries = _entries(view, FEATURE_VERTEX)
		if not _check(second_vertex_entries.size() == 1, "Could not restore a vertex selection for handle refusal."):
			return
	for frame in 3:
		await get_tree().process_frame
	gizmo_pivot = view.get_meta("_level_transform_gizmo_pivot", Vector3.ZERO)
	gizmo_scale = float(view.get_meta("_level_transform_gizmo_scale", 0.0))
	x_arrow_world = gizmo_pivot + Vector3.RIGHT * gizmo_scale * arrow_offset
	x_arrow_screen = _global_screen(container, camera, x_arrow_world)
	var handle_selection_before := _entries(view, FEATURE_VERTEX).duplicate(true)
	var handle_positions_before: PackedVector3Array = block.data.vertex_positions.duplicate()
	var handle_version_before: int = history.get_version()
	await _send_click(host_viewport, x_arrow_screen)
	if not _check(_entries(view, FEATURE_VERTEX) == handle_selection_before and
			block.data.vertex_positions == handle_positions_before and history.get_version() == handle_version_before,
			"A press on a gizmo handle changed selection, geometry, or history."):
		return

	print("TRANSFORM_EDITOR_SMOKE_OK")
