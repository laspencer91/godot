@tool
extends EditorPlugin


# EditorDocument::Type is append-only; TYPE_LEVEL follows TYPE_SHADER in LE0.
const TYPE_LEVEL := 9


func _enter_tree() -> void:
	_run_test()


func _find_level_view() -> Control:
	return EditorInterface.get_base_control().find_child("LevelEditorView", true, false) as Control


func _fail(message: String) -> void:
	push_error("LEVEL_TAB_SMOKE: " + message)


func _run_test() -> void:
	# Plugin initialization precedes workspace construction and layout restore.
	var resource_filesystem := EditorInterface.get_resource_filesystem()
	for frame in 20:
		await get_tree().process_frame
	while resource_filesystem != null and resource_filesystem.is_scanning():
		await get_tree().process_frame
	# Filesystem scan completion schedules importer finalization and layout work; let both settle.
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
		_fail("No LevelEditorView surface was minted in a workspace pane.")
		return
	if int(view.get_meta("_level_document_type", -1)) != TYPE_LEVEL:
		_fail("The surface is not backed by EditorDocument::TYPE_LEVEL.")
		return

	var level_viewport := view.find_child("LevelViewport", true, false) as SubViewport
	if level_viewport == null:
		_fail("The LevelEditorView has no LevelViewport SubViewport.")
		return
	var document_world: World3D = view.get_meta("_level_document_world_3d", null)
	if document_world == null or level_viewport.world_3d != document_world:
		_fail("LevelViewport.world_3d is not the LevelDocument world_3d.")
		return

	var closed := false
	for candidate in EditorInterface.get_base_control().find_children("*", "TabBar", true, false):
		var tab_bar := candidate as TabBar
		if tab_bar == null:
			continue
		for tab_index in tab_bar.tab_count:
			if tab_bar.get_tab_title(tab_index).ends_with(" [Level]"):
				tab_bar.emit_signal("tab_close_pressed", tab_index)
				closed = true
				break
		if closed:
			break
	if not closed:
		_fail("Could not find the LevelDocument workspace tab to close.")
		return

	for frame in 30:
		await get_tree().process_frame
	if _find_level_view() != null:
		_fail("LevelEditorView survived after its workspace tab closed.")
		return

	print("LEVEL_TAB_SMOKE_OK")
