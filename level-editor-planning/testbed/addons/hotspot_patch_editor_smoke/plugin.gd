@tool
extends EditorPlugin

const TYPE_HOTSPOT_ATLAS := 10
const TOOL_SELECT := 0
const FEATURE_FACE := 2
const OUTPUT_DIR := "res://.godot/wp21_hotspot_patch_editor_smoke"
const TEXTURE_PATH := OUTPUT_DIR + "/reference_texture.tres"
const ATLAS_PATH := OUTPUT_DIR + "/fixture_atlas.tres"
const EXPORT_PATH := OUTPUT_DIR + "/exported.rect"
const BINDING_PATH := OUTPUT_DIR + "/bindings.tres"
const IMPORT_PATH := "res://assets/hotspot_patch_editor_fixture.rect"
const BINDING_KEY := "res://textures/wp21_wall"

var failed := false


func _enter_tree() -> void:
	_run_test()


func _fail(message: String) -> void:
	if failed:
		return
	failed = true
	push_error("HOTSPOT_PATCH_EDITOR_SMOKE: " + message)


func _check(condition: bool, message: String) -> bool:
	if not condition:
		_fail(message)
	return condition


func _find_patch_view() -> Control:
	return EditorInterface.get_base_control().find_child("HotspotPatchEditor", true, false) as Control


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
	block.name = "HotspotPatchPreviewBlock"
	block.data = mesh.data
	return block


func _send_key(host_viewport: Viewport, keycode: Key) -> void:
	var event := InputEventKey.new()
	event.keycode = keycode
	event.physical_keycode = keycode
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


func _uv_snapshot(data: LevelMeshData) -> PackedByteArray:
	return var_to_bytes([
		data.face_uv_modes, data.face_uv_origins, data.face_uv_tangents,
		data.face_uv_transforms, data.loop_uv0,
	])


func _rect_close(a: Rect2, b: Rect2) -> bool:
	return a.position.is_equal_approx(b.position) and a.size.is_equal_approx(b.size)


func _patch_snapshot(atlas: HotspotAtlas) -> PackedByteArray:
	var values: Array = []
	for patch: HotspotPatch in atlas.patches:
		values.append([
			patch.patch_name, patch.rect_uv, patch.allow_rotation,
			patch.allow_mirror_x, patch.allow_mirror_y, patch.allow_tiling,
			patch.tiling_axis, patch.inset_px, patch.extra,
		])
	return var_to_bytes(values)


func _make_fixture() -> HotspotAtlas:
	DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path(OUTPUT_DIR))
	var texture := GradientTexture2D.new()
	texture.gradient = Gradient.new()
	texture.width = 256
	texture.height = 128
	if not _check(ResourceSaver.save(texture, TEXTURE_PATH) == OK, "Could not save the reference texture fixture."):
		return null
	var saved_texture := ResourceLoader.load(TEXTURE_PATH, "GradientTexture2D", ResourceLoader.CACHE_MODE_IGNORE) as GradientTexture2D
	if not _check(saved_texture != null and saved_texture.width == 256 and saved_texture.height == 128,
			"The reference texture fixture did not reload with exact dimensions."):
		return null
	var initial := HotspotPatch.new()
	initial.patch_name = &"initial"
	initial.rect_uv = Rect2(0.0625, 0.0625, 0.25, 0.25)
	var atlas := HotspotAtlas.new()
	atlas.atlas_id = &"wp21_editor_fixture"
	atlas.reference_texture = saved_texture
	atlas.patches = [initial]
	if not _check(ResourceSaver.save(atlas, ATLAS_PATH) == OK, "Could not save the HotspotAtlas fixture .tres."):
		return null
	return ResourceLoader.load(ATLAS_PATH, "HotspotAtlas", ResourceLoader.CACHE_MODE_IGNORE) as HotspotAtlas


func _close_hotspot_tab() -> bool:
	for candidate in EditorInterface.get_base_control().find_children("*", "TabBar", true, false):
		var tab_bar := candidate as TabBar
		if tab_bar == null:
			continue
		for tab_index in tab_bar.tab_count:
			if tab_bar.get_tab_title(tab_index).ends_with(" [Hotspot]"):
				tab_bar.emit_signal("tab_close_pressed", tab_index)
				return true
	return false


func _run_test() -> void:
	var resource_filesystem := EditorInterface.get_resource_filesystem()
	for frame in 20:
		await get_tree().process_frame
	while resource_filesystem != null and resource_filesystem.is_scanning():
		await get_tree().process_frame
	for frame in 10:
		await get_tree().process_frame

	if not _check(Engine.has_singleton("LevelEditor"), "LevelEditor service is unavailable."):
		return
	var service := Engine.get_singleton("LevelEditor")
	var editor_settings := EditorInterface.get_editor_settings()
	editor_settings.set_setting("level_editor/hotspot/bindings_path", BINDING_PATH)
	var reload_error: Error = service.reload_hotspot_bindings()
	if reload_error != OK and reload_error != ERR_FILE_NOT_FOUND:
		_fail("Could not reset the isolated binding registry (%s)." % error_string(reload_error))
		return

	# Keep a real selected face resident behind the atlas tab so the noncommitting
	# preview fallback can be driven and its exact UV restoration asserted.
	var filesystem := EditorInterface.get_file_system_dock()
	if not _check(filesystem != null and filesystem.open_scene_in_level_editor("res://main.tscn") == OK,
			"Could not open the preview fixture as a LevelDocument."):
		return
	for frame in 30:
		await get_tree().process_frame
	var level_view := _find_level_view()
	if not _check(level_view != null, "No LevelEditorView was minted for the preview fixture."):
		return
	var scene_root: Node = level_view.get_meta("_level_document_root", null)
	var level_container := level_view.find_child("LevelViewportContainer", true, false) as Control
	var level_camera := level_view.find_child("LevelCamera3D", true, false) as Camera3D
	if not _check(scene_root != null and level_container != null and level_camera != null,
			"The preview level root, pane, or camera is missing."):
		return
	service.set_tool_mode(TOOL_SELECT)
	level_container.grab_focus()
	level_camera.position = Vector3(0, 0, 12)
	level_camera.look_at_from_position(level_camera.position, Vector3.ZERO, Vector3.UP)
	level_camera.fov = 50.0
	var preview_block := _make_block()
	if failed or preview_block == null:
		return
	scene_root.add_child(preview_block, true)
	preview_block.owner = scene_root
	for frame in 5:
		await get_tree().process_frame
	var host_viewport := level_container.get_viewport()
	await _send_key(host_viewport, KEY_3)
	await _send_click(host_viewport, _global_screen(level_container, level_camera, Vector3(0, 0, 1)))
	if not _check(_face_entries(level_view).size() == 1, "Could not establish one face selection for preview."):
		return
	var preview_uv_before := _uv_snapshot(preview_block.data)

	var atlas := _make_fixture()
	if not _check(atlas != null, "The fixture HotspotAtlas did not reload."):
		return
	EditorInterface.edit_resource(atlas)
	for frame in 35:
		await get_tree().process_frame

	var view := _find_patch_view()
	var state: Dictionary = service.get_hotspot_patch_editor_state()
	if not _check(view != null and service.get_hotspot_patch_editor_count() == 1,
			"The resource route did not mint exactly one HotspotPatchEditor view."):
		return
	if not _check(int(state.get("document_type", -1)) == TYPE_HOTSPOT_ATLAS and
			String(state.get("view_class", "")) == "HotspotPatchEditor" and
			String(state.get("atlas_path", "")) == ATLAS_PATH,
			"The tab is not backed by TYPE_HOTSPOT_ATLAS/HotspotPatchEditor (%s)." % state):
		return
	if not _check(view.find_child("HotspotPatchCanvas", true, false) != null,
			"The dedicated patch canvas is missing (generic resource fallback suspected)."):
		return

	var undo_manager := EditorInterface.get_editor_undo_redo()
	var history_id: int = undo_manager.get_object_history_id(atlas)
	var history: UndoRedo = undo_manager.get_history_undo_redo(history_id)
	if not _check(history != null, "The resource document undo history is unavailable."):
		return

	# Scripted handlers are the exact gesture-release paths: create and resize each add one action.
	var create_version := history.get_version()
	if not _check(service.hotspot_editor_create_patch_px(Rect2(32, 16, 64, 32)), "Scripted patch creation was rejected."):
		return
	if not _check(atlas.patches.size() == 2 and history.get_version() == create_version + 1 and
			_rect_close(atlas.get_patch_rect_px(1), Rect2(32, 16, 64, 32)),
			"Create did not produce one exact undoable patch gesture."):
		return
	var resize_version := history.get_version()
	if not _check(service.hotspot_editor_set_patch_rect_px(1, Rect2(48, 24, 80, 40)), "Scripted resize was rejected."):
		return
	if not _check(history.get_version() == resize_version + 1 and
			_rect_close(atlas.get_patch_rect_px(1), Rect2(48, 24, 80, 40)),
			"Resize did not commit one exact undo action."):
		return
	history.undo()
	if not _check(_rect_close(atlas.get_patch_rect_px(1), Rect2(32, 16, 64, 32)),
			"One undo did not restore the pre-resize rect."):
		return
	history.undo()
	if not _check(atlas.patches.size() == 1, "A second undo did not remove the created patch."):
		return
	history.redo()
	history.redo()
	state = service.get_hotspot_patch_editor_state()
	if not _check(bool(state.get("dirty", false)) and bool(state.get("resource_edited", false)),
			"Patch gestures did not dirty the document/resource."):
		return

	service.hotspot_editor_save()
	for frame in 12:
		await get_tree().process_frame
	var saved_snapshot := _patch_snapshot(atlas)
	var reloaded := ResourceLoader.load(ATLAS_PATH, "HotspotAtlas", ResourceLoader.CACHE_MODE_IGNORE) as HotspotAtlas
	if not _check(reloaded != null and _patch_snapshot(reloaded) == saved_snapshot,
			"Standard save and cache-ignored reload did not round-trip patch edits."):
		return

	# Import replaces the complete set as one action; export must reimport byte-identically.
	var before_import := _patch_snapshot(atlas)
	var import_version := history.get_version()
	if not _check(service.hotspot_editor_import_rect(IMPORT_PATH) == OK,
			"The button handler's .rect import path failed."):
		return
	var imported_snapshot := _patch_snapshot(atlas)
	if not _check(atlas.patches.size() == 2 and imported_snapshot != before_import and
			history.get_version() == import_version + 1,
			".rect import did not replace the patch set in one undo action."):
		return
	history.undo()
	if not _check(_patch_snapshot(atlas) == before_import, "One undo did not restore the pre-import patch set."):
		return
	history.redo()
	if not _check(_patch_snapshot(atlas) == imported_snapshot, "Redo did not restore the imported patch set."):
		return
	if not _check(service.hotspot_editor_export_rect(EXPORT_PATH) == OK and FileAccess.file_exists(EXPORT_PATH),
			"The button handler's .rect export path did not produce a file."):
		return
	var exported := HotspotAtlas.new()
	exported.reference_texture = atlas.reference_texture
	if not _check(exported.import_rect(EXPORT_PATH) == OK and _patch_snapshot(exported) == imported_snapshot,
			"The exported .rect did not reimport identically."):
		return

	# Add/remove go through the editor handler and the registry's existing lazy save path.
	if not _check(service.hotspot_editor_add_binding(BINDING_KEY) == OK and FileAccess.file_exists(BINDING_PATH),
			"Adding this atlas binding did not save the isolated registry."):
		return
	var bindings := ResourceLoader.load(BINDING_PATH, "HotspotBinding", ResourceLoader.CACHE_MODE_IGNORE) as HotspotBinding
	if not _check(bindings != null and bindings.resolve_pattern(BINDING_KEY) == ATLAS_PATH,
			"The added binding did not round-trip from disk."):
		return
	if not _check(service.hotspot_editor_remove_binding(BINDING_KEY) == OK, "Removing this atlas binding failed."):
		return
	bindings = ResourceLoader.load(BINDING_PATH, "HotspotBinding", ResourceLoader.CACHE_MODE_IGNORE) as HotspotBinding
	if not _check(bindings != null and bindings.resolve_pattern(BINDING_KEY).is_empty(),
			"The removed binding survived a cache-ignored registry reload."):
		return

	var preview_runs_before := int(level_view.get_meta("_level_hotspot_preview_run_count", 0))
	service.hotspot_editor_set_preview_enabled(true)
	service.hotspot_editor_set_debug_enabled(true)
	for frame in 3:
		await get_tree().process_frame
	state = service.get_hotspot_patch_editor_state()
	if not _check(bool(state.get("preview_enabled", false)) and bool(state.get("debug_enabled", false)),
			"Preview/debug toggles did not enter the enabled state."):
		return
	if not _check(int(level_view.get_meta("_level_hotspot_preview_run_count", 0)) > preview_runs_before and
			bool(level_view.get_meta("_level_hotspot_preview_ok", false)) and
			int(level_view.get_meta("_level_hotspot_preview_island_count", 0)) > 0 and
			String(level_view.get_meta("_level_hotspot_overlay_mode", "")) == "preview",
			"The selected-face fitter preview did not produce a transient island overlay."):
		return
	service.hotspot_editor_set_preview_enabled(false)
	service.hotspot_editor_set_debug_enabled(false)
	for frame in 2:
		await get_tree().process_frame
	state = service.get_hotspot_patch_editor_state()
	if not _check(not bool(state.get("preview_enabled", true)) and not bool(state.get("debug_enabled", true)),
			"Preview/debug toggles did not restore the disabled state."):
		return
	if not _check(_uv_snapshot(preview_block.data) == preview_uv_before,
			"The noncommitting preview changed real face UV state."):
		return

	service.hotspot_editor_save()
	for frame in 12:
		await get_tree().process_frame
	if not _check(not bool(service.get_hotspot_patch_editor_state().get("dirty", true)),
			"The standard save path did not clear document dirty state before close."):
		return
	if not _check(_close_hotspot_tab(), "Could not find the HotspotAtlas workspace tab to close."):
		return
	for frame in 30:
		await get_tree().process_frame
	if not _check(_find_patch_view() == null and service.get_hotspot_patch_editor_count() == 0,
			"The HotspotPatchEditor survived after its workspace tab closed."):
		return

	print("HOTSPOT_PATCH_EDITOR_SMOKE_OK document=type10+dedicated-view gestures=one-action rect_io=one-action+roundtrip bindings=roundtrip preview=islands+uv-exact debug=toggle close=clean")
