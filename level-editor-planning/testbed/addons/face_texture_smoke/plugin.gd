@tool
extends EditorPlugin

const SmokeInputReady = preload("res://addons/smoke_input_ready.gd")
const TOOL_SELECT := 0
const FEATURE_FACE := 2
const MATERIAL_A := "res://assets/material_browser_fixture/M_BrickSquare.tres"
const MATERIAL_B := "res://assets/material_browser_fixture/PlainTall.tres"
const QUICK_MATERIAL := "res://assets/material_browser_fixture/overrides/M_ShaderMatch.tres"
const HOTSPOT_ATLAS := "res://.godot/hotspot_fitter_editor_smoke.tres"

var failed := false


func _enter_tree() -> void:
	_run_test()


func _fail(message: String) -> void:
	if failed:
		return
	failed = true
	push_error("FACE_TEXTURE_EDITOR_SMOKE: " + message)


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
	block.name = "FaceTextureSmokeBlock"
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


func _send_lift(host_viewport: Viewport, position: Vector2) -> void:
	var press := InputEventMouseButton.new()
	press.position = position
	press.global_position = position
	press.button_index = MOUSE_BUTTON_RIGHT
	press.button_mask = MOUSE_BUTTON_MASK_RIGHT
	press.shift_pressed = true
	press.pressed = true
	host_viewport.push_input(press, true)
	await get_tree().process_frame
	var release := InputEventMouseButton.new()
	release.position = position
	release.global_position = position
	release.button_index = MOUSE_BUTTON_RIGHT
	release.button_mask = 0
	release.shift_pressed = true
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
	var texture_context := view.find_child("LevelTextureContext", true, false)
	var hotspot_mapping := view.find_child("HotspotMappingModeOverride", true, false) as OptionButton
	if not _check(scene_root != null and container != null and camera != null and texture_context != null and
			texture_context.find_child("ModifyShiftE", true, false) != null and hotspot_mapping != null and
			hotspot_mapping.item_count == 5,
			"The level root, pane, camera, or contextual texture controls are missing."):
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

	service.set_active_material(load(MATERIAL_A), MATERIAL_A)
	var apply_version := history.get_version()
	await _send_key(host_viewport, KEY_T, true)
	if not _check(block.data.material_paths == PackedStringArray([MATERIAL_A]) and
			block.data.face_material_indices[face_id] == 0 and history.get_version() == apply_version + 1,
			"Shift+T did not apply the active material in exactly one undo action."):
		return
	history.undo()
	if not _check(block.data.material_paths.is_empty(), "Undo did not restore the pre-Apply material table."):
		return
	history.redo()
	if not _check(block.data.material_paths == PackedStringArray([MATERIAL_A]), "Redo did not restore Apply."):
		return

	service.set_active_material(load(MATERIAL_B), MATERIAL_B)
	var lift_version := history.get_version()
	await _send_lift(host_viewport, face_screen)
	var capture: Dictionary = service.get_captured_mapping()
	if not _check(service.get_active_material_path() == MATERIAL_A and capture.get("valid", false) and
			capture.get("has_mapping", false) and history.get_version() == lift_version and
			view.get_meta("_level_lifted_face", -1) == face_id,
			"Shift+RMB did not lift material plus projected mapping without an undo step."):
		return

	container.grab_focus()
	await get_tree().process_frame
	var before_transform := mesh.get_face_uv_transform(face_id)
	var modify_version := history.get_version()
	await _send_key(host_viewport, KEY_KP_6)
	var after_transform := mesh.get_face_uv_transform(face_id)
	if not _check((after_transform.origin - before_transform.origin).is_equal_approx(Vector2(0.125, 0)) and
			history.get_version() == modify_version + 1 and
			view.get_meta("_level_last_selection_action", &"") == &"texture_modify",
			"Pane-focused Numpad 6 did not perform one UV-space shift undo action."):
		return
	history.undo()
	if not _check(mesh.get_face_uv_transform(face_id) == before_transform,
			"One undo did not restore the exact pre-nudge UV transform."):
		return

	EditorInterface.get_editor_settings().set("level_editor/material_browser/blockout_override_folder",
			"res://assets/material_browser_fixture/overrides")
	container.grab_focus()
	var quick_version := history.get_version()
	await _send_key(host_viewport, KEY_2, true, false, true)
	var quick_index: int = block.data.face_material_indices[face_id]
	if not _check(service.get_active_material_path() == QUICK_MATERIAL and
			block.data.get_material_path(quick_index) == QUICK_MATERIAL and history.get_version() == quick_version + 1,
			"Shift+Alt+2 did not set the quick-slot material and Apply it in one action."):
		return
	history.undo()
	if not _check(block.data.get_material_path(block.data.face_material_indices[face_id]) == MATERIAL_A,
			"One undo did not restore the material before quick-slot Apply."):
		return

	var patch := HotspotPatch.new()
	patch.patch_name = &"editor_smoke_patch"
	patch.rect_uv = Rect2(0, 0, 1, 1)
	var atlas := HotspotAtlas.new()
	atlas.atlas_id = &"editor/smoke"
	atlas.reference_texture = load("res://assets/material_browser_fixture/textures/T_Square.svg")
	atlas.texel_density_target = 8.0
	atlas.default_mapping_mode = HotspotAtlas.MAPPING_SQUARE
	atlas.disallow_random = true
	var patches: Array[HotspotPatch] = [patch]
	atlas.patches = patches
	DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path("res://.godot"))
	if not _check(ResourceSaver.save(atlas, HOTSPOT_ATLAS) == OK and
			service.bind_hotspot_atlas(MATERIAL_A, HOTSPOT_ATLAS) == OK,
			"The bound hotspot atlas fixture could not be saved."):
		return
	var hotspot_before_transform := mesh.get_face_uv_transform(face_id)
	var hotspot_before_mode := mesh.get_face_uv_mode(face_id)
	var hotspot_before_uvs: PackedVector2Array = block.data.loop_uv0
	var hotspot_before_names: PackedStringArray = block.data.face_hotspot_patch_names
	var hotspot_selection := _face_entries(view)
	var hotspot_version := history.get_version()
	container.grab_focus()
	await _send_key(host_viewport, KEY_H, true)
	if not _check(history.get_version() == hotspot_version + 1 and
			mesh.get_face_uv_transform(face_id) != hotspot_before_transform and
			block.data.face_hotspot_patch_names[face_id] == "editor_smoke_patch" and
			_face_entries(view) == hotspot_selection and
			view.get_meta("_level_last_selection_action", &"") == &"hotspot_fit_grouped",
			"Shift+H did not apply a bound grouped hotspot fit in exactly one undo action."):
		return
	history.undo()
	if not _check(mesh.get_face_uv_transform(face_id) == hotspot_before_transform and
			mesh.get_face_uv_mode(face_id) == hotspot_before_mode and
			block.data.loop_uv0 == hotspot_before_uvs and
			block.data.face_hotspot_patch_names == hotspot_before_names,
			"One undo did not restore the exact pre-hotspot UV and sticky-name rows."):
		return

	print("FACE_TEXTURE_EDITOR_SMOKE_OK apply=shift+t lift=shift+rmb modify=kp6 quick=shift+alt+2 hotspot=shift+h undo=single panel=contextual-texture")
