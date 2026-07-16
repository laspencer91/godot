@tool
extends EditorPlugin


const SmokeInputReady = preload("res://addons/smoke_input_ready.gd")
const TOOL_BLOCK := 1
const SAVE_PATH := "res://block_tool_smoke_saved.tscn"
const EXPECTED_AABB := AABB(Vector3(-2, 0, -2), Vector3(4, 3, 4))
const DATA_ARRAY_PROPERTIES: PackedStringArray = [
	"vertex_positions",
	"vertex_alive",
	"vertex_free_ids",
	"edge_vertices",
	"edge_alive",
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
	"face_free_ids",
	"loop_vertex_indices",
	"loop_uv0",
	"loop_colors",
	"loop_normals",
	"loop_alive",
	"loop_free_ids",
]


func _enter_tree() -> void:
	_run_test()


func _fail(message: String) -> void:
	push_error("BLOCK_TOOL_SMOKE: " + message)


func _find_level_view() -> Control:
	return EditorInterface.get_base_control().find_child("LevelEditorView", true, false) as Control


func _find_block(scene_root: Node) -> Node:
	for child in scene_root.get_children():
		if child.get_class() == "LevelBlock":
			return child
	return null


func _find_blocks(scene_root: Node) -> Array:
	var blocks: Array = []
	for child in scene_root.get_children():
		if child.get_class() == "LevelBlock":
			blocks.append(child)
	return blocks


func _find_added_block(scene_root: Node, previous: Array) -> Node:
	for candidate in _find_blocks(scene_root):
		if not previous.has(candidate):
			return candidate
	return null


func _global_screen(container: Control, camera: Camera3D, world_position: Vector3) -> Vector2:
	return container.get_global_transform_with_canvas() * camera.unproject_position(world_position)


func _data_aabb(data: Object) -> AABB:
	var vertices: PackedVector3Array = data.get("vertex_positions")
	if vertices.is_empty():
		return AABB()
	var bounds := AABB(vertices[0], Vector3.ZERO)
	for vertex in vertices:
		bounds = bounds.expand(vertex)
	return bounds


func _world_data_aabb(block: Node3D) -> AABB:
	var vertices: PackedVector3Array = block.get("data").get("vertex_positions")
	if vertices.is_empty():
		return AABB()
	var first := block.global_transform * vertices[0]
	var bounds := AABB(first, Vector3.ZERO)
	for vertex in vertices:
		bounds = bounds.expand(block.global_transform * vertex)
	return bounds


func _capture_arrays(data: Object) -> Dictionary:
	var snapshot := {}
	for property_name in DATA_ARRAY_PROPERTIES:
		var value: Variant = data.get(property_name)
		snapshot[property_name] = value.duplicate()
	return snapshot


func _arrays_equal(data: Object, snapshot: Dictionary) -> bool:
	for property_name in DATA_ARRAY_PROPERTIES:
		if data.get(property_name) != snapshot[property_name]:
			return false
	return true


func _send_button(viewport: Viewport, position: Vector2, pressed: bool) -> void:
	var event := InputEventMouseButton.new()
	event.position = position
	event.global_position = position
	event.button_index = MOUSE_BUTTON_LEFT
	event.button_mask = MOUSE_BUTTON_MASK_LEFT if pressed else 0
	event.pressed = pressed
	viewport.push_input(event, true)


func _send_motion(viewport: Viewport, position: Vector2, relative: Vector2, button_mask := MOUSE_BUTTON_MASK_LEFT) -> void:
	var event := InputEventMouseMotion.new()
	event.position = position
	event.global_position = position
	event.relative = relative
	event.button_mask = button_mask
	viewport.push_input(event, true)


func _send_hover_motion(container: Control, local_position: Vector2, relative: Vector2) -> void:
	var event := InputEventMouseMotion.new()
	event.position = local_position
	event.global_position = container.get_global_transform_with_canvas() * local_position
	event.relative = relative
	event.button_mask = 0
	container.emit_signal(&"gui_input", event)


func _send_enter(viewport: Viewport) -> void:
	var event := InputEventKey.new()
	event.keycode = KEY_ENTER
	event.physical_keycode = KEY_ENTER
	event.pressed = true
	viewport.push_input(event, true)


func _send_click(viewport: Viewport, position: Vector2) -> void:
	_send_button(viewport, position, true)
	await get_tree().process_frame
	_send_button(viewport, position, false)
	await get_tree().process_frame


func _send_base_drag(viewport: Viewport, start_position: Vector2, end_position: Vector2) -> void:
	_send_button(viewport, start_position, true)
	await get_tree().process_frame
	_send_motion(viewport, end_position, end_position - start_position)
	await get_tree().process_frame
	_send_button(viewport, end_position, false)
	await get_tree().process_frame


func _send_full_drag(viewport: Viewport, start_position: Vector2, end_position: Vector2) -> void:
	await _send_base_drag(viewport, start_position, end_position)
	_send_enter(viewport)
	for frame in 4:
		await get_tree().process_frame


func _has_rebuilt_children(block: Node) -> bool:
	var mesh_instance: MeshInstance3D = null
	var static_body: StaticBody3D = null
	var collision_shape: CollisionShape3D = null
	for child in block.get_children(true):
		if child is MeshInstance3D:
			mesh_instance = child
		elif child is StaticBody3D:
			static_body = child
	if static_body != null:
		for child in static_body.get_children(true):
			if child is CollisionShape3D:
				collision_shape = child
	return mesh_instance != null and mesh_instance.mesh != null and \
			mesh_instance.mesh.get_surface_count() > 0 and collision_shape != null and \
			collision_shape.shape is ConcavePolygonShape3D


func _run_test() -> void:
	var resource_filesystem := EditorInterface.get_resource_filesystem()
	for frame in 20:
		await get_tree().process_frame
	while resource_filesystem != null and resource_filesystem.is_scanning():
		await get_tree().process_frame
	for frame in 10:
		await get_tree().process_frame

	var filesystem := EditorInterface.get_file_system_dock()
	if filesystem == null:
		_fail("FileSystemDock is unavailable.")
		return
	var err: Error = filesystem.open_scene_in_level_editor("res://main.tscn")
	if err != OK:
		_fail("open_scene_in_level_editor returned %s." % error_string(err))
		return

	for frame in 30:
		await get_tree().process_frame
	var view := _find_level_view()
	if view == null:
		_fail("No LevelEditorView was minted.")
		return
	if not Engine.has_singleton("LevelEditor"):
		_fail("The LevelEditor service singleton is unavailable.")
		return
	var service := Engine.get_singleton("LevelEditor")
	service.set_snap_step(1.0)
	service.set_snap_enabled(true)
	service.set_tool_mode(TOOL_BLOCK)
	EditorInterface.get_selection().clear()
	await get_tree().process_frame
	if int(view.get_meta("_level_tool_mode", -1)) != TOOL_BLOCK:
		_fail("The service did not activate BlockTool in the level view.")
		return

	var scene_root: Node = view.get_meta("_level_document_root", null)
	var container := view.find_child("LevelViewportContainer", true, false) as Control
	var camera := view.find_child("LevelCamera3D", true, false) as Camera3D
	if scene_root == null or container == null or camera == null:
		_fail("The document root, viewport container, or camera is missing.")
		return
	var input_ready_error: String = await SmokeInputReady.wait_for_level_view(get_tree(), view, container, scene_root)
	if not input_ready_error.is_empty():
		_fail(input_ready_error)
		return
	var host_viewport := container.get_viewport()
	var start_local := camera.unproject_position(Vector3(-2, 0, -2))
	var end_local := camera.unproject_position(Vector3(2, 0, 2))
	var start_position: Vector2 = container.get_global_transform_with_canvas() * start_local
	var end_position: Vector2 = container.get_global_transform_with_canvas() * end_local
	if not container.get_global_rect().has_point(start_position) or not container.get_global_rect().has_point(end_position):
		_fail("Known drag points are outside the level viewport.")
		return

	host_viewport.warp_mouse(start_position)
	await get_tree().process_frame
	_send_button(host_viewport, start_position, true)
	await get_tree().process_frame
	_send_motion(host_viewport, end_position, end_position - start_position)
	await get_tree().process_frame
	_send_button(host_viewport, end_position, false)
	await get_tree().process_frame
	_send_enter(host_viewport)
	for frame in 3:
		await get_tree().process_frame

	var block := _find_block(scene_root)
	if block == null:
		_fail("The staged viewport gesture did not create a LevelBlock.")
		return
	var data: Object = block.get("data")
	if data == null or data.vertex_count() != 8 or data.edge_count() != 12 or \
			data.face_count() != 6 or data.loop_count() != 24:
		_fail("The committed block has unexpected LevelMeshData counts.")
		return
	if not _data_aabb(data).is_equal_approx(EXPECTED_AABB):
		_fail("The committed block AABB is %s, expected %s." % [_data_aabb(data), EXPECTED_AABB])
		return
	var committed_arrays := _capture_arrays(data)

	var manager := EditorInterface.get_editor_undo_redo()
	var history_id: int = manager.get_object_history_id(scene_root)
	var history: UndoRedo = manager.get_history_undo_redo(history_id)
	if history == null:
		_fail("The level document undo history is unavailable.")
		return
	history.undo()
	await get_tree().process_frame
	if block.get_parent() != null or _find_block(scene_root) != null:
		_fail("Undo did not remove the LevelBlock node.")
		return
	history.redo()
	for frame in 2:
		await get_tree().process_frame
	if block.get_parent() != scene_root or _find_block(scene_root) != block:
		_fail("Redo did not restore the identical LevelBlock node.")
		return
	if not _arrays_equal(block.get("data"), committed_arrays):
		_fail("Redo changed one or more serialized LevelMeshData arrays.")
		return

	var packed := PackedScene.new()
	err = packed.pack(scene_root)
	if err != OK:
		_fail("PackedScene.pack failed with %s." % error_string(err))
		return
	err = ResourceSaver.save(packed, SAVE_PATH)
	if err != OK:
		_fail("Saving the round-trip scene failed with %s." % error_string(err))
		return
	var reloaded_packed := ResourceLoader.load(SAVE_PATH, "", ResourceLoader.CACHE_MODE_IGNORE) as PackedScene
	if reloaded_packed == null:
		_fail("The saved round-trip scene could not be reloaded.")
		return
	var reloaded_root := reloaded_packed.instantiate()
	get_tree().root.add_child(reloaded_root)
	for frame in 3:
		await get_tree().process_frame
	var reloaded_block := _find_block(reloaded_root)
	if reloaded_block == null:
		_fail("The saved scene did not persist its LevelBlock.")
		return
	var reloaded_data: Object = reloaded_block.get("data")
	if reloaded_data == null or not _arrays_equal(reloaded_data, committed_arrays):
		_fail("The reloaded LevelBlock did not preserve its mesh arrays.")
		return
	if not _has_rebuilt_children(reloaded_block):
		_fail("The reloaded LevelBlock did not rebuild render and collision children.")
		return
	get_tree().root.remove_child(reloaded_root)
	reloaded_root.queue_free()

	# Surface start: the top-face hit freezes a +Y plane through y=3. The new
	# block therefore starts exactly on the authored block instead of Y=0.
	camera.position = Vector3(8, 10, 10)
	camera.look_at_from_position(camera.position, Vector3(0, 2, 0), Vector3.UP)
	camera.fov = 50.0
	for frame in 6:
		await get_tree().process_frame
	var top_start := _global_screen(container, camera, Vector3(-1, 3, -1))
	var top_end := _global_screen(container, camera, Vector3(1, 3, 1))
	if not container.get_global_rect().has_point(top_start) or not container.get_global_rect().has_point(top_end):
		_fail("Top-face drag points are outside the level viewport.")
		return
	var before_top := _find_blocks(scene_root)
	await _send_full_drag(host_viewport, top_start, top_end)
	var top_block := _find_added_block(scene_root, before_top)
	if top_block == null:
		_fail("A drag beginning on the authored block's top face created no block.")
		return
	var expected_top := AABB(Vector3(-1, 3, -1), Vector3(2, 3, 2))
	if not _data_aabb(top_block.get("data")).is_equal_approx(expected_top):
		_fail("The top-face block AABB is %s, expected %s." % [_data_aabb(top_block.get("data")), expected_top])
		return
	history.undo()
	for frame in 2:
		await get_tree().process_frame
	if top_block.get_parent() != null or _find_blocks(scene_root).size() != before_top.size():
		_fail("One undo did not remove exactly the top-face block.")
		return
	history.redo()
	for frame in 5:
		await get_tree().process_frame
	if top_block.get_parent() != scene_root or _find_blocks(scene_root).size() != before_top.size() + 1:
		_fail("Redo did not restore exactly the top-face block.")
		return

	# Existing geometry always retains world-grid alignment. This is intentional
	# even when free placement is enabled for the ground plane.
	EditorInterface.get_selection().clear()
	service.set_snap_enabled(false)
	var unsnapped_surface_start := _global_screen(container, camera, Vector3(-1.6, 3, 1.6))
	var unsnapped_surface_end := _global_screen(container, camera, Vector3(0.4, 3, 0.4))
	if not container.get_global_rect().has_point(unsnapped_surface_start) or not container.get_global_rect().has_point(unsnapped_surface_end):
		_fail("Snap-off surface drag points are outside the level viewport.")
		return
	var before_surface_snap := _find_blocks(scene_root)
	await _send_full_drag(host_viewport, unsnapped_surface_start, unsnapped_surface_end)
	var surface_snap_block := _find_added_block(scene_root, before_surface_snap)
	if surface_snap_block == null:
		_fail("Snap-off surface placement created no block.")
		return
	var expected_surface_snap := AABB(Vector3(-2, 3, 0), Vector3(2, 3, 2))
	if not _world_data_aabb(surface_snap_block).is_equal_approx(expected_surface_snap):
		_fail("Surface placement ignored the world grid while free ground placement was enabled: %s, expected %s." % [
				_world_data_aabb(surface_snap_block), expected_surface_snap])
		return
	service.set_snap_enabled(true)

	# Wall start: the +X hit freezes the YZ plane at x=2 and extrudes outward.
	camera.position = Vector3(12, 2, 10)
	camera.look_at_from_position(camera.position, Vector3(1.5, 1.5, 0), Vector3.UP)
	await get_tree().process_frame
	# Avoid exact half-step inputs: perspective ray reconstruction can land a few
	# ulps on either side of 0.5, while both authored points must quantize to 0/2.
	var wall_start := _global_screen(container, camera, Vector3(2, 0.4, -1))
	var wall_end := _global_screen(container, camera, Vector3(2, 2.4, 1))
	if not container.get_global_rect().has_point(wall_start) or not container.get_global_rect().has_point(wall_end):
		_fail("Wall-face drag points are outside the level viewport.")
		return
	var before_wall := _find_blocks(scene_root)
	await _send_full_drag(host_viewport, wall_start, wall_end)
	var wall_block := _find_added_block(scene_root, before_wall)
	if wall_block == null:
		_fail("A drag beginning on the authored block's +X wall created no block.")
		return
	var expected_wall := AABB(Vector3(2, 0, -1), Vector3(3, 2, 2))
	if not _data_aabb(wall_block.get("data")).is_equal_approx(expected_wall):
		_fail("The wall-face block AABB is %s, expected %s." % [_data_aabb(wall_block.get("data")), expected_wall])
		return
	for frame in 5:
		await get_tree().process_frame

	# Regression: accepting Height with a second stationary click must preserve
	# the already-previewed default height instead of re-projecting it to zero.
	camera.position = Vector3(-2, 8, 14)
	camera.look_at_from_position(camera.position, Vector3(-7, 0, -1), Vector3.UP)
	await get_tree().process_frame
	var default_start := _global_screen(container, camera, Vector3(-8, 0, -2))
	var default_end := _global_screen(container, camera, Vector3(-6, 0, 0))
	if not container.get_global_rect().has_point(default_start) or not container.get_global_rect().has_point(default_end):
		_fail("Default-height drag points are outside the level viewport.")
		return
	var before_default := _find_blocks(scene_root)
	await _send_base_drag(host_viewport, default_start, default_end)
	if _find_blocks(scene_root).size() != before_default.size():
		_fail("Base release committed before the second-LMB height acceptance case.")
		return
	await _send_click(host_viewport, default_end)
	for frame in 4:
		await get_tree().process_frame
	var default_block := _find_added_block(scene_root, before_default)
	if default_block == null:
		_fail("A stationary second LMB did not commit the default-height block.")
		return
	var expected_default := AABB(Vector3(-8, 0, -2), Vector3(2, 3, 2))
	if not _data_aabb(default_block.get("data")).is_equal_approx(expected_default):
		_fail("The second-LMB block AABB is %s, expected %s." % [_data_aabb(default_block.get("data")), expected_default])
		return

	# WP22: crossing the drag threshold without crossing either grid boundary
	# must immediately show a box and accept the negative side on both tangents.
	EditorInterface.get_selection().clear()
	service.set_snap_enabled(true)
	service.set_snap_step(1.0)
	camera.position = Vector3(-8, 8, 16)
	camera.look_at_from_position(camera.position, Vector3(-12, 0, 8), Vector3.UP)
	for frame in 3:
		await get_tree().process_frame
	var min_cell_start_world := Vector3(-12, 0, 8)
	var min_cell_end_world := min_cell_start_world + Vector3(-0.4, 0, -0.4)
	var min_cell_start := _global_screen(container, camera, min_cell_start_world)
	var min_cell_end := _global_screen(container, camera, min_cell_end_world)
	if not container.get_global_rect().has_point(min_cell_start) or not container.get_global_rect().has_point(min_cell_end):
		_fail("Minimum-cell drag points are outside the level viewport.")
		return
	var before_min_cell := _find_blocks(scene_root)
	_send_button(host_viewport, min_cell_start, true)
	await get_tree().process_frame
	view.set_meta("_level_block_overlay_has_geometry", false)
	_send_motion(host_viewport, min_cell_end, min_cell_end - min_cell_start)
	await get_tree().process_frame
	if int(view.get_meta("_level_block_state", -1)) != 2:
		_fail("A within-cell motion did not enter the base-drag state.")
		return
	if not bool(view.get_meta("_level_block_overlay_has_geometry", false)):
		_fail("A within-cell base drag did not produce overlay geometry.")
		return
	_send_button(host_viewport, min_cell_end, false)
	await get_tree().process_frame
	if _find_blocks(scene_root).size() != before_min_cell.size():
		_fail("Minimum-cell base release committed before height acceptance.")
		return
	await _send_click(host_viewport, min_cell_end)
	for frame in 4:
		await get_tree().process_frame
	var min_cell_block := _find_added_block(scene_root, before_min_cell)
	if min_cell_block == null:
		_fail("Height acceptance did not commit the minimum-cell block.")
		return
	var snap_step: float = service.get_snap_step()
	var expected_min_cell := AABB(
			min_cell_start_world + Vector3(-snap_step, 0, -snap_step),
			Vector3(snap_step, service.get_default_block_height(), snap_step))
	if not _data_aabb(min_cell_block.get("data")).is_equal_approx(expected_min_cell):
		_fail("The negative minimum-cell block AABB is %s, expected %s." % [
				_data_aabb(min_cell_block.get("data")), expected_min_cell])
		return

	# Idle hover performs one cached surface query per motion and drives the
	# overlay's script-visible geometry state without touching the document.
	var ground_hover_local := container.size * Vector2(0.5, 0.75)
	var ground_hover: Vector2 = container.get_global_transform_with_canvas() * ground_hover_local
	if not container.get_global_rect().has_point(ground_hover):
		_fail("Ground-hover probe is outside the level viewport.")
		return
	var ground_ray_origin := camera.project_ray_origin(ground_hover_local)
	var ground_ray_direction := camera.project_ray_normal(ground_hover_local)
	if ground_ray_direction.y >= -0.0001 or -ground_ray_origin.y / ground_ray_direction.y <= 0.0:
		_fail("The layout-derived ground-hover probe does not intersect the ground in front of the camera.")
		return
	view.set_meta("_level_block_overlay_has_geometry", 42)
	_send_hover_motion(container, ground_hover_local, Vector2(1, 1))
	await get_tree().process_frame
	if typeof(view.get_meta("_level_block_overlay_has_geometry", null)) != TYPE_BOOL:
		_fail("Idle pointer motion did not reach the BlockTool hover path.")
		return
	if not bool(view.get_meta("_level_block_overlay_has_geometry", false)):
		_fail("Ground hover did not create a one-cell footprint ghost (state %s)." % view.get_meta("_level_block_state", -1))
		return
	camera.position = Vector3(8, 10, 10)
	camera.look_at_from_position(camera.position, Vector3(0, 2, 0), Vector3.UP)
	await get_tree().process_frame
	var face_hover_local := camera.unproject_position(Vector3(1.5, 3, 1.5))
	var face_hover: Vector2 = container.get_global_transform_with_canvas() * face_hover_local
	if not container.get_global_rect().has_point(face_hover):
		_fail("Face-hover probe is outside the level viewport.")
		return
	_send_hover_motion(container, face_hover_local, face_hover_local - ground_hover_local)
	await get_tree().process_frame
	if not bool(view.get_meta("_level_block_overlay_has_geometry", false)):
		_fail("Face hover did not create a one-cell footprint ghost.")
		return

	camera.position = Vector3(0, 5, 12)
	camera.look_at_from_position(camera.position, Vector3(0, 5, 0), Vector3.UP)
	await get_tree().process_frame
	var visible_size := camera.get_viewport().get_visible_rect().size
	var sky_local := Vector2(visible_size.x * 0.5, 1)
	if camera.project_ray_normal(sky_local).y <= 0.0:
		_fail("The configured sky probe does not point above the ground horizon.")
		return
	var sky_position: Vector2 = container.get_global_transform_with_canvas() * sky_local
	_send_hover_motion(container, sky_local, sky_local - face_hover_local)
	await get_tree().process_frame
	if bool(view.get_meta("_level_block_overlay_has_geometry", true)):
		_fail("Empty-sky hover left stale overlay geometry visible.")
		return

	# A guaranteed-miss press must remain Idle; after release, a normal gesture
	# must work immediately (the former dead-gesture regression).
	_send_button(host_viewport, sky_position, true)
	await get_tree().process_frame
	if int(view.get_meta("_level_block_state", -1)) != 0:
		_fail("A guaranteed-miss press advanced the BlockTool state.")
		return
	_send_button(host_viewport, sky_position, false)
	await get_tree().process_frame
	camera.position = Vector3(10, 8, 12)
	camera.look_at_from_position(camera.position, Vector3(7, 0, -6), Vector3.UP)
	await get_tree().process_frame
	var recovery_start := _global_screen(container, camera, Vector3(6, 0, -7))
	var recovery_end := _global_screen(container, camera, Vector3(8, 0, -5))
	if not container.get_global_rect().has_point(recovery_start) or not container.get_global_rect().has_point(recovery_end):
		_fail("Failed-start recovery drag points are outside the level viewport.")
		return
	var before_recovery := _find_blocks(scene_root)
	await _send_full_drag(host_viewport, recovery_start, recovery_end)
	var recovery_block := _find_added_block(scene_root, before_recovery)
	if recovery_block == null:
		_fail("The first valid gesture after a failed start did not create a block.")
		return
	var expected_recovery := AABB(Vector3(6, 0, -7), Vector3(2, 3, 2))
	if not _data_aabb(recovery_block.get("data")).is_equal_approx(expected_recovery):
		_fail("The failed-start recovery block AABB is %s, expected %s." % [_data_aabb(recovery_block.get("data")), expected_recovery])
		return

	# A single selected scene node is the actual parent for new geometry. A
	# transformed Node3D parent must not alter where the viewport gesture lands.
	var selected_parent := Node3D.new()
	selected_parent.name = "SelectedCreationParent"
	selected_parent.position = Vector3(3, 0, 2)
	selected_parent.rotation.y = 0.35
	selected_parent.scale = Vector3(1.5, 1.25, 0.75)
	scene_root.add_child(selected_parent, true)
	selected_parent.owner = scene_root
	var object_selection := EditorInterface.get_selection()
	object_selection.clear()
	object_selection.add_node(selected_parent)
	service.set_snap_enabled(true)
	camera.position = Vector3(18, 10, 16)
	camera.look_at_from_position(camera.position, Vector3(11, 1, 5), Vector3.UP)
	for frame in 3:
		await get_tree().process_frame
	var parent_start := _global_screen(container, camera, Vector3(10, 0, 4))
	var parent_end := _global_screen(container, camera, Vector3(12, 0, 6))
	if not container.get_global_rect().has_point(parent_start) or not container.get_global_rect().has_point(parent_end):
		_fail("Selected-parent drag points are outside the level viewport.")
		return
	await _send_full_drag(host_viewport, parent_start, parent_end)
	var parented_block: Node3D = null
	for child in selected_parent.get_children():
		if child.get_class() == "LevelBlock":
			parented_block = child
			break
	if parented_block == null or parented_block.get_parent() != selected_parent or parented_block.owner != scene_root:
		_fail("The selected node did not become the new LevelBlock's structural parent/owner chain.")
		return
	var expected_parented := AABB(Vector3(10, 0, 4), Vector3(2, 3, 2))
	if not _world_data_aabb(parented_block).is_equal_approx(expected_parented):
		_fail("Selected-parent conversion moved the authored block: %s, expected %s." % [
				_world_data_aabb(parented_block), expected_parented])
		return
	history.undo()
	await get_tree().process_frame
	if parented_block.get_parent() != null:
		_fail("Undo did not remove the block from its selected parent.")
		return
	history.redo()
	for frame in 2:
		await get_tree().process_frame
	if parented_block.get_parent() != selected_parent or not _world_data_aabb(parented_block).is_equal_approx(expected_parented):
		_fail("Redo did not restore the block under its selected parent in place.")
		return

	print("BLOCK_TOOL_SMOKE_OK")
