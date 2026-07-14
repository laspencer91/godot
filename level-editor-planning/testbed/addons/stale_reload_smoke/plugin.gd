@tool
extends EditorPlugin


var failed := false


func _enter_tree() -> void:
	_run_test()


func _fail(message: String) -> void:
	if failed:
		return
	failed = true
	push_error("STALE_RELOAD_SMOKE: " + message)


func _check(condition: bool, message: String) -> bool:
	if not condition:
		_fail(message)
	return condition


func _make_block(block_name: String) -> LevelBlock:
	var mesh := LevelMesh.new()
	mesh.begin_transaction()
	if not mesh.create_box(Transform3D.IDENTITY, Vector3(2, 2, 2), 0):
		_fail("Kernel box creation was rejected.")
		return null
	if mesh.commit() == null:
		_fail("Kernel box creation returned no diff.")
		return null
	var block := LevelBlock.new()
	block.name = block_name
	block.data = mesh.data
	return block


func _add_node_with_undo(scene_root: Node, node: Node, action_name: String) -> void:
	var undo_manager := EditorInterface.get_editor_undo_redo()
	undo_manager.create_action(action_name, UndoRedo.MERGE_DISABLE, scene_root)
	undo_manager.add_do_method(scene_root, &"add_child", node, true)
	undo_manager.add_do_method(node, &"set_owner", scene_root)
	undo_manager.add_do_reference(node)
	undo_manager.add_undo_method(scene_root, &"remove_child", node)
	undo_manager.commit_action()


func _find_level_view() -> Control:
	return EditorInterface.get_base_control().find_child("LevelEditorView", true, false) as Control


func _find_plain_root(level_root: Node, required_child: StringName = &"") -> Node:
	for candidate in EditorInterface.get_open_scene_roots():
		var scene_root := candidate as Node
		if scene_root == null or scene_root == level_root:
			continue
		if required_child.is_empty() or scene_root.get_node_or_null(NodePath(String(required_child))) != null:
			return scene_root
	return null


func _find_disk_changed_dialog() -> ConfirmationDialog:
	for candidate in EditorInterface.get_base_control().find_children("*", "ConfirmationDialog", true, false):
		var dialog := candidate as ConfirmationDialog
		if dialog != null and dialog.title == "Files have been modified outside Godot":
			return dialog
	return null


func _workspace_scene_tab_titles() -> PackedStringArray:
	var titles := PackedStringArray()
	for candidate in EditorInterface.get_base_control().find_children("*", "TabBar", true, false):
		var tab_bar := candidate as TabBar
		if tab_bar == null:
			continue
		var has_level_document := false
		for tab_index in tab_bar.tab_count:
			if tab_bar.get_tab_title(tab_index).ends_with(" [Level]"):
				has_level_document = true
				break
		if not has_level_document:
			continue
		for tab_index in tab_bar.tab_count:
			titles.append(tab_bar.get_tab_title(tab_index))
	return titles


func _find_legacy_scene_tabs() -> TabBar:
	for candidate in EditorInterface.get_base_control().find_children("*", "TabBar", true, false):
		var tab_bar := candidate as TabBar
		if tab_bar == null or tab_bar.tab_count != 2:
			continue
		var only_main_scene := true
		for tab_index in tab_bar.tab_count:
			if tab_bar.get_tab_title(tab_index) != "main.tscn":
				only_main_scene = false
				break
		if only_main_scene:
			return tab_bar
	return null


func _run_test() -> void:
	var resource_filesystem := EditorInterface.get_resource_filesystem()
	for frame in 20:
		await get_tree().process_frame
	while resource_filesystem != null and resource_filesystem.is_scanning():
		await get_tree().process_frame
	for frame in 10:
		await get_tree().process_frame

	EditorInterface.open_scene_from_path("res://main.tscn")
	for frame in 30:
		await get_tree().process_frame
	var initial_plain_root := EditorInterface.get_edited_scene_root()
	if not _check(initial_plain_root != null, "The plain scene document did not open."):
		return
	var initial_plain_root_id := initial_plain_root.get_instance_id()

	var filesystem := EditorInterface.get_file_system_dock()
	if not _check(filesystem != null, "FileSystemDock is unavailable."):
		return
	var err: Error = filesystem.open_scene_in_level_editor("res://main.tscn")
	if not _check(err == OK, "open_scene_in_level_editor returned %s." % error_string(err)):
		return
	for frame in 30:
		await get_tree().process_frame

	var level_view := _find_level_view()
	if not _check(level_view != null, "No LevelEditorView was minted."):
		return
	var level_root := level_view.get_meta("_level_document_root", null) as Node
	if not _check(level_root != null and EditorInterface.get_edited_scene_root() == level_root,
			"The level document is not current after opening."):
		return
	if not _check(EditorInterface.get_open_scene_roots().size() == 2,
			"The same path was not open as exactly two scene documents."):
		return

	var saved_block := _make_block("StaleReloadBlock")
	if failed or saved_block == null:
		return
	_add_node_with_undo(level_root, saved_block, "Add stale-reload smoke block")
	for frame in 3:
		await get_tree().process_frame

	# EditorData stores mtimes in whole seconds. Cross a tick after both documents
	# captured the original value so this smoke deterministically exercises staleness.
	OS.delay_msec(1100)
	EditorInterface.save_scene_as("res://main.tscn", false)

	var reloaded_plain_root: Node = null
	for frame in 90:
		reloaded_plain_root = _find_plain_root(level_root, &"StaleReloadBlock")
		if reloaded_plain_root != null:
			break
		await get_tree().process_frame
	if not _check(reloaded_plain_root != null,
			"The clean plain sibling was not silently reloaded from the level save."):
		return
	if not _check(reloaded_plain_root.get_instance_id() != initial_plain_root_id,
			"The plain sibling root was not replaced by the reload path."):
		return
	if not _check(EditorInterface.get_edited_scene_root() == level_root and
			level_root.get_node_or_null(^"StaleReloadBlock") == saved_block,
			"Silent reload changed or unfocused the saving level document."):
		return

	var disk_dialog := _find_disk_changed_dialog()
	if not _check(disk_dialog != null and not disk_dialog.visible,
			"The disk-changed dialog appeared for the clean sibling reload."):
		return

	# The background reload deliberately restores the level document before the
	# deferred workspace reveal runs. Select the existing plain scene entry long
	# enough to mint its view and expose the actual document-type title, then put
	# the level document back exactly where the save left it.
	var legacy_scene_tabs := _find_legacy_scene_tabs()
	if not _check(legacy_scene_tabs != null, "The legacy scene tab strip could not be resolved."):
		return
	legacy_scene_tabs.current_tab = 0
	for frame in 30:
		await get_tree().process_frame
	var workspace_titles := _workspace_scene_tab_titles()
	if not _check(workspace_titles.count("main.tscn") == 1 and
			workspace_titles.count("main.tscn [Level]") == 1,
			"Reload did not preserve one plain and one TYPE_LEVEL workspace document (%s)." % workspace_titles):
		return
	if not _check(EditorInterface.get_edited_scene_root() == reloaded_plain_root,
			"Selecting the plain scene tab did not make its document current."):
		return
	legacy_scene_tabs.current_tab = 1
	for frame in 10:
		await get_tree().process_frame
	if not _check(EditorInterface.get_edited_scene_root() == level_root,
			"Restoring the level scene tab did not restore the saving document."):
		return

	# The dirty-sibling counterpart is intentionally omitted in headless mode:
	# embedded ConfirmationDialog visibility does not transition reliably there.
	print("STALE_RELOAD_SMOKE_OK silent_reload=plain type=preserved current=level conflict_dialog=headless_skipped")
