@tool
extends EditorPlugin


func _enter_tree() -> void:
	_run_test()


func _send_ctrl_f(pressed: bool) -> void:
	var event := InputEventKey.new()
	event.keycode = KEY_F
	event.ctrl_pressed = true
	event.pressed = pressed
	Input.parse_input_event(event)


func _find_visible_code_edit(node: Node) -> CodeEdit:
	if node is CodeEdit and node.is_visible_in_tree():
		return node
	for child in node.get_children(true):
		var code_edit := _find_visible_code_edit(child)
		if code_edit:
			return code_edit
	return null


func _run_test() -> void:
	for frame in 20:
		await get_tree().process_frame

	# A newly revealed script attaches its editor-specific menus after the shared menu strip has
	# already moved into the workspace DocumentView. The late menu must inherit that live chrome
	# context; otherwise SceneTree rejects Ctrl+F while focus is in the script's CodeEdit.
	EditorInterface.edit_script(get_script(), 1, 0, true)
	for frame in 15:
		await get_tree().process_frame
	var code_edit := _find_visible_code_edit(EditorInterface.get_base_control())
	if code_edit == null:
		push_error("Could not find the newly revealed script CodeEdit.")
		return

	code_edit.grab_focus()
	await get_tree().process_frame
	_send_ctrl_f(true)
	_send_ctrl_f(false)
	for frame in 3:
		await get_tree().process_frame

	var shortcut_focus := EditorInterface.get_base_control().get_viewport().gui_get_focus_owner()
	if not shortcut_focus is LineEdit or shortcut_focus.placeholder_text != "Find" or not shortcut_focus.is_visible_in_tree():
		push_error("Ctrl+F did not reveal and focus the current script's Find field.")
		return
	print("SCRIPT_CTRL_F_FIND_BAR_OK")
