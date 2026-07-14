@tool
extends EditorPlugin


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


func _data_aabb(data: Object) -> AABB:
	var vertices: PackedVector3Array = data.get("vertex_positions")
	if vertices.is_empty():
		return AABB()
	var bounds := AABB(vertices[0], Vector3.ZERO)
	for vertex in vertices:
		bounds = bounds.expand(vertex)
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


func _send_motion(viewport: Viewport, position: Vector2, relative: Vector2) -> void:
	var event := InputEventMouseMotion.new()
	event.position = position
	event.global_position = position
	event.relative = relative
	event.button_mask = MOUSE_BUTTON_MASK_LEFT
	viewport.push_input(event, true)


func _send_enter(viewport: Viewport) -> void:
	var event := InputEventKey.new()
	event.keycode = KEY_ENTER
	event.physical_keycode = KEY_ENTER
	event.pressed = true
	viewport.push_input(event, true)


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
	service.set_tool_mode(TOOL_BLOCK)
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
	container.grab_focus()
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

	print("BLOCK_TOOL_SMOKE_OK")

