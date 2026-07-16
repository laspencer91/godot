@tool
extends EditorPlugin


const SmokeInputReady = preload("res://addons/smoke_input_ready.gd")
const SOURCE_PROJECT := 0
const SOURCE_IN_LEVEL := 1
const SOURCE_HIDDEN := 2
const TOOL_SELECT := 0
const TOOL_BLOCK := 1
const EXPECTED_INDEX_COUNT := 10
const FIXTURE_ROOT := "res://assets/material_browser_fixture"
const OVERRIDE_ROOT := FIXTURE_ROOT + "/overrides"
const SQUARE_PATH := FIXTURE_ROOT + "/M_BrickSquare.tres"
const TALL_PATH := FIXTURE_ROOT + "/PlainTall.tres"
const SHADER_PATH := OVERRIDE_ROOT + "/M_ShaderMatch.tres"
const NO_TEXTURE_PATH := OVERRIDE_ROOT + "/PlainNoTexture.tres"

var failed := false
var active_signal_count := 0
var active_signal_path := ""


func _enter_tree() -> void:
	_run_test()


func _fail(message: String) -> void:
	if failed:
		return
	failed = true
	push_error("MATERIAL_BROWSER_SMOKE: " + message)


func _check(condition: bool, message: String) -> bool:
	if not condition:
		_fail(message)
	return condition


func _find_level_view() -> Control:
	return EditorInterface.get_base_control().find_child("LevelEditorView", true, false) as Control


func _find_level_view_for_path(path: String) -> Control:
	for candidate in EditorInterface.get_base_control().find_children("LevelEditorView", "", true, false):
		var view := candidate as Control
		var root: Node = view.get_meta("_level_document_root", null) if view != null else null
		if root != null and root.scene_file_path == path:
			return view
	return null


func _find_level_document_scope(view: Control) -> Control:
	var cursor: Node = view
	while cursor != null:
		if cursor is HSplitContainer and cursor.find_child("MaterialBrowserDock", true, false) != null and \
				cursor.find_child("MaterialsBottomDockToggle", true, false) != null:
			return cursor as Control
		cursor = cursor.get_parent()
	return null


func _on_active_material_changed(_material: Material, path: String) -> void:
	active_signal_count += 1
	active_signal_path = path


func _send_blockout_key(host_viewport: Viewport, keycode: Key) -> void:
	var event := InputEventKey.new()
	event.keycode = keycode
	event.physical_keycode = keycode
	event.shift_pressed = true
	event.alt_pressed = true
	event.pressed = true
	host_viewport.push_input(event, true)
	await get_tree().process_frame


func _run_test() -> void:
	var resource_filesystem := EditorInterface.get_resource_filesystem()
	for frame in 20:
		await get_tree().process_frame
	while resource_filesystem != null and resource_filesystem.is_scanning():
		await get_tree().process_frame
	for frame in 12:
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
	if not _check(Engine.has_singleton("LevelEditor"), "LevelEditor singleton is unavailable."):
		return
	var service := Engine.get_singleton("LevelEditor")
	var index: Object = service.get_material_index()
	var scanner: Object = service.get_texel_density_scanner()
	var dock: Object = service.get_material_browser_dock()
	var registry: Object = service.get_blockout_material_registry()
	if not _check(index != null and scanner != null and dock != null and registry != null,
			"One or more material-browser services are unavailable."):
		return

	index.rebuild()
	await get_tree().process_frame
	var entries: Array = index.get_entries()
	if not _check(index.get_count() == EXPECTED_INDEX_COUNT,
			"Index count is %d, expected %d (%s)." % [index.get_count(), EXPECTED_INDEX_COUNT, index.get_paths()]):
		return
	var standard_count := 0
	var shader_count := 0
	var convention_count := 0
	for entry: Dictionary in entries:
		standard_count += int(entry.class_name == "StandardMaterial3D")
		shader_count += int(entry.class_name == "ShaderMaterial")
		convention_count += int(entry.is_convention_named)
	if not _check(standard_count == 8 and shader_count == 2 and convention_count == 8,
			"Classification mismatch (standard=%d shader=%d M_*=%d)." % [standard_count, shader_count, convention_count]):
		return

	var square: Dictionary = scanner.scan_path(SQUARE_PATH)
	var tall: Dictionary = scanner.scan_path(TALL_PATH)
	var shader: Dictionary = scanner.scan_path(SHADER_PATH)
	var no_texture: Dictionary = scanner.scan_path(NO_TEXTURE_PATH)
	if not _check(square.found and square.dimensions == Vector2i(16, 16),
			"Standard square scan mismatch: %s." % square):
		return
	if not _check(tall.found and tall.dimensions == Vector2i(8, 24),
			"Standard tall scan mismatch: %s." % tall):
		return
	if not _check(shader.found and shader.dimensions == Vector2i(32, 16) and shader.uniform_name == &"albedo_texture",
			"ShaderMaterial named-uniform scan mismatch: %s." % shader):
		return
	if not _check(not no_texture.found, "No-texture ShaderMaterial fabricated dimensions: %s." % no_texture):
		return

	dock.set_filters(SOURCE_PROJECT, "", false)
	if not _check(dock.get_filtered_count() == 10, "Default Project filter did not expose all materials."):
		return
	dock.set_filters(SOURCE_PROJECT, "", true)
	if not _check(dock.get_filtered_count() == 8, "M_*-only filter count mismatch."):
		return
	dock.set_filters(SOURCE_PROJECT, "brick", false)
	if not _check(dock.get_filtered_count() == 1 and dock.get_filtered_paths()[0] == SQUARE_PATH,
			"Case-insensitive name search did not isolate M_BrickSquare."):
		return

	index.set_hidden(TALL_PATH, true)
	index.reload_hidden_paths()
	if not _check(index.is_hidden(TALL_PATH), "Hidden path did not round-trip through project metadata."):
		return
	dock.set_filters(SOURCE_PROJECT, "", false)
	if not _check(dock.get_filtered_count() == 9, "Hidden material remained in Project source."):
		return
	dock.set_filters(SOURCE_HIDDEN, "", false)
	if not _check(dock.get_filtered_count() == 1 and dock.get_filtered_paths()[0] == TALL_PATH,
			"Hidden source did not contain exactly the hidden material."):
		return
	index.set_hidden(TALL_PATH, false)
	index.reload_hidden_paths()
	dock.set_filters(SOURCE_HIDDEN, "", false)
	if not _check(not index.is_hidden(TALL_PATH) and dock.get_filtered_count() == 0,
			"Unhide did not persist or remove the material from Hidden source."):
		return

	var scene_root: Node = view.get_meta("_level_document_root", null)
	var block := LevelBlock.new()
	block.set_meta("material_paths", PackedStringArray([SQUARE_PATH]))
	scene_root.add_child(block)
	dock.set_filters(SOURCE_IN_LEVEL, "", false)
	if not _check(dock.get_filtered_count() == 1 and dock.get_filtered_paths()[0] == SQUARE_PATH,
			"In Level source did not resolve the block's path metadata seam."):
		return
	scene_root.remove_child(block)
	block.queue_free()

	service.active_material_changed.connect(_on_active_material_changed)
	dock.set_filters(SOURCE_PROJECT, "", false)
	var active_material := load(SQUARE_PATH) as Material
	service.set_active_material(active_material, SQUARE_PATH)
	for frame in 3:
		await get_tree().process_frame
	if not _check(active_signal_count == 1 and active_signal_path == SQUARE_PATH,
			"set_active_material did not emit the expected signal."):
		return
	if not _check(service.get_active_material_path() == SQUARE_PATH and
			view.get_meta("_level_active_material_path", "") == SQUARE_PATH and dock.get_selected_path() == SQUARE_PATH,
			"Service, pane swatch, and dock disagree on the active material."):
		return

	var settings := EditorInterface.get_editor_settings()
	settings.set("level_editor/material_browser/blockout_override_folder", "")
	var builtins: Array = registry.get_resolved_materials()
	var builtin_paths: PackedStringArray = registry.get_resolved_paths()
	var builtin_ids := {}
	for slot in 10:
		if not _check(builtins[slot] != null and builtin_paths[slot].is_empty(),
				"Zero-override slot %d is null or file-backed." % slot):
			return
		builtin_ids[builtins[slot].get_instance_id()] = true
	if not _check(builtin_ids.size() == 10, "Procedural built-ins are not ten distinct materials."):
		return

	settings.set("level_editor/material_browser/blockout_override_folder", OVERRIDE_ROOT)
	var overridden: Array = registry.get_resolved_materials()
	var override_paths: PackedStringArray = registry.get_resolved_paths()
	if not _check(overridden.size() == 10 and override_paths[0].ends_with("M_BlockoutOverrideA.tres") and
			override_paths[1].ends_with("M_ShaderMatch.tres") and override_paths[2].ends_with("PlainNoTexture.tres") and
			override_paths[3].is_empty(), "Three sorted overrides did not fill slots 1..3 then pad built-ins: %s." % override_paths):
		return

	settings.set("level_editor/material_browser/blockout_override_folder", "")
	var container := view.find_child("LevelViewportContainer", true, false) as Control
	var input_ready_error: String = await SmokeInputReady.wait_for_level_view(get_tree(), view, container, scene_root)
	if not _check(input_ready_error.is_empty(), input_ready_error):
		return
	await _send_blockout_key(container.get_viewport(), KEY_3)
	if not _check(service.get_active_material_path().is_empty() and
			service.get_active_material_display_name() == "Blockout 3 - Mid Gray" and active_signal_count == 2,
			"Shift+Alt+3 did not set procedural blockout slot 3 as active-only state."):
		return

	var queue: Object = dock.get_preview_queue()
	queue.request(SQUARE_PATH)
	for frame in 60:
		if queue.has_completed(SQUARE_PATH):
			break
		await get_tree().process_frame
	if not _check(queue.has_completed(SQUARE_PATH), "Private preview queue did not complete bookkeeping."):
		return

	# WP22: the browser is a document-owned bottom drawer. Opening it must only
	# reduce the surface side of the scene split, never the right accordion.
	var first_scope := _find_level_document_scope(view)
	var first_toggle := first_scope.find_child("MaterialsBottomDockToggle", true, false) as Button if first_scope != null else null
	var first_panel := first_scope.find_child("MaterialsBottomDockPanel", true, false) as Control if first_scope != null else null
	var first_context_toggle := view.find_child("LevelContextPanelToggle", true, false) as Button
	var first_context := view.find_child("LevelContextPanel", true, false) as Control
	var context_separator := view.find_child("LevelToolContextSeparator", true, false) as Control
	var select_tool_button := view.find_child("LevelSelectToolButton", true, false) as Button
	var nudge_up := view.find_child("ModifyShiftN", true, false) as Button
	var grid_label := first_scope.find_child("LevelGridStepLabel", true, false) as Label if first_scope != null else null
	var grid_decrease := first_scope.find_child("LevelGridStepDecrease", true, false) as Button if first_scope != null else null
	var grid_increase := first_scope.find_child("LevelGridStepIncrease", true, false) as Button if first_scope != null else null
	var right_column := first_scope.get_child(1) as Control if first_scope != null and first_scope.get_child_count() > 1 else null
	if not _check(first_scope != null and first_toggle != null and first_panel != null and right_column != null and
			first_context_toggle != null and first_context != null and context_separator != null and
			select_tool_button != null and nudge_up != null and grid_label != null and
			grid_decrease != null and grid_increase != null,
			"The document-local Materials drawer or Level context controls are missing."):
		return
	if not _check(select_tool_button.icon_alignment == HORIZONTAL_ALIGNMENT_CENTER and
			nudge_up.icon_alignment == HORIZONTAL_ALIGNMENT_CENTER,
			"Tool-rail or icon-only context buttons are not center-aligned."):
		return
	service.set_snap_step(1.0)
	grid_decrease.emit_signal("pressed")
	await get_tree().process_frame
	if not _check(is_equal_approx(service.get_snap_step(), 0.5) and
			grid_label.text.contains("0.5 m") and grid_label.text.contains("2 m"),
			"Grid decrease did not update the shared snap step and live grid label: %s." % grid_label.text):
		return
	grid_increase.emit_signal("pressed")
	await get_tree().process_frame
	if not _check(is_equal_approx(service.get_snap_step(), 1.0),
			"Grid increase did not restore the shared snap step."):
		return
	var viewport_height_before := container.size.y
	var right_height_before := right_column.size.y
	first_toggle.emit_signal("toggled", true)
	for frame in 4:
		await get_tree().process_frame
	if not _check(first_toggle.button_pressed and first_panel.visible and
			container.size.y < viewport_height_before - 40.0 and
			is_equal_approx(right_column.size.y, right_height_before),
			"Opening Materials did not shrink only the viewport side (viewport %.1f -> %.1f, right %.1f -> %.1f)." % [
					viewport_height_before, container.size.y, right_height_before, right_column.size.y]):
		return

	# Give the first document unmistakable semantic and presentation state, then
	# open a second Level document. The second must start clean and both live
	# views must continue to display their own material context.
	dock.set_filters(SOURCE_PROJECT, "brick", false)
	dock.set_zoom(124)
	service.set_active_material(load(SQUARE_PATH), SQUARE_PATH)
	service.set_captured_mapping({ "valid": true, "has_mapping": true, "source": "first-document" })
	service.set_hotspot_mapping_mode_override(2)
	first_context_toggle.emit_signal("pressed")
	await get_tree().process_frame
	if not _check(not first_context.visible and not context_separator.visible and dock.get_zoom() == 124 and dock.get_filtered_count() == 1,
			"The first document did not accept its distinct context/filter/zoom state."):
		return

	var second_err: Error = filesystem.open_scene_in_level_editor("res://main_second.tscn")
	if not _check(second_err == OK, "Opening the second Level document returned %s." % error_string(second_err)):
		return
	for frame in 30:
		await get_tree().process_frame
	var first_view := _find_level_view_for_path("res://main.tscn")
	var second_view := _find_level_view_for_path("res://main_second.tscn")
	var second_scope := _find_level_document_scope(second_view)
	var first_dock: Object = first_scope.find_child("MaterialBrowserDock", true, false)
	var second_dock: Object = second_scope.find_child("MaterialBrowserDock", true, false) if second_scope != null else null
	var second_toggle := second_scope.find_child("MaterialsBottomDockToggle", true, false) as Button if second_scope != null else null
	var second_context := second_view.find_child("LevelContextPanel", true, false) as Control if second_view != null else null
	if not _check(first_view != null and second_view != null and first_dock != null and second_dock != null and
			second_toggle != null and second_context != null,
			"Two live Level documents did not mint two complete material/context views."):
		return
	if not _check(service.get_material_browser_dock() == second_dock and
			first_dock.get_preview_queue() == second_dock.get_preview_queue(),
			"The active browser did not resolve to document two or the preview cache was duplicated."):
		return
	var second_state: Dictionary = second_dock.get_presentation_state()
	if not _check(first_toggle.button_pressed and not second_toggle.button_pressed and
			first_dock.get_zoom() == 124 and second_dock.get_zoom() == 108 and
			second_state.search == "" and second_state.source == SOURCE_PROJECT and
			not first_context.visible and second_context.visible and
			service.get_captured_mapping().is_empty() and service.get_hotspot_mapping_mode_override() == -1,
			"Document two inherited document one's drawer, filter, zoom, context, capture, or hotspot state."):
		return
	service.set_tool_mode(TOOL_BLOCK)
	await get_tree().process_frame
	if not _check(first_view.get_meta("_level_tool_mode", -1) == TOOL_SELECT and
			second_view.get_meta("_level_tool_mode", -1) == TOOL_BLOCK,
			"Changing document two's tool rail changed document one's tool mode."):
		return
	service.set_tool_mode(TOOL_SELECT)

	service.set_active_material(load(TALL_PATH), TALL_PATH)
	for frame in 3:
		await get_tree().process_frame
	if not _check(first_view.get_meta("_level_active_material_path", "") == SQUARE_PATH and
			second_view.get_meta("_level_active_material_path", "") == TALL_PATH and
			first_dock.get_selected_path() == SQUARE_PATH and second_dock.get_selected_path() == TALL_PATH,
			"Changing document two's active material crossed into document one."):
		return

	print("MATERIAL_BROWSER_SMOKE_OK scanner=standard16x16,standard8x24,shader32x16,no-texture-none queue=%s pool=%d overscan=%d documents=isolated drawer=surface-only" % [
			"rid" if queue.is_rendering_available() else "headless-bookkeeping",
			dock.get_virtualized_pool_size(), dock.get_overscan_rows()])
