@tool
extends EditorPlugin


func _enter_tree() -> void:
	_run_test()


func _frames(count: int) -> void:
	for frame in count:
		await get_tree().process_frame


func _send_ctrl_a(pressed: bool) -> void:
	var event := InputEventKey.new()
	event.keycode = KEY_A
	event.ctrl_pressed = true
	event.pressed = pressed
	Input.parse_input_event(event)


func _send_ctrl_z(pressed: bool) -> void:
	var event := InputEventKey.new()
	event.keycode = KEY_Z
	event.ctrl_pressed = true
	event.pressed = pressed
	Input.parse_input_event(event)


func _send_button(position: Vector2, pressed: bool) -> void:
	var event := InputEventMouseButton.new()
	event.position = position
	event.global_position = position
	event.button_index = MOUSE_BUTTON_LEFT
	event.button_mask = MOUSE_BUTTON_MASK_LEFT if pressed else 0
	event.pressed = pressed
	# Route through Input so WorkspacePane._input sees the press before GUI dispatch and updates the
	# focused-pane marker exactly as a real mouse click does.
	Input.parse_input_event(event)


func _find_item(tree: Tree, metadata: Variant) -> TreeItem:
	var item := tree.get_root()
	while item:
		if item.get_metadata(0) == metadata:
			return item
		item = item.get_next_in_tree()
	return null


func _scene_node_count(node: Node) -> int:
	var count := 1
	for child in node.get_children():
		count += _scene_node_count(child)
	return count


func _find_scene_tree(node: Node) -> Tree:
	for candidate in EditorInterface.get_base_control().find_children("*", "Tree", true, false):
		var tree := candidate as Tree
		if tree and tree.is_visible_in_tree() and _find_item(tree, node.get_path()):
			return tree
	return null


func _focus_scene_tree_node(tree: Tree, node: Node) -> bool:
	var item := _find_item(tree, node.get_path())
	if item == null:
		return false
	tree.scroll_to_item(item, true)
	await get_tree().process_frame
	var rect := tree.get_item_area_rect(item, 0)
	var position: Vector2 = tree.get_global_transform_with_canvas() * rect.get_center()
	var viewport := tree.get_viewport()
	viewport.warp_mouse(position)
	await get_tree().process_frame
	_send_button(position, true)
	_send_button(position, false)
	await _frames(2)
	# Inspector/selection synchronization runs deferred after release and may move GUI focus on some
	# display backends. Reassert the intended shortcut owner after those callbacks settle.
	tree.grab_focus()
	await get_tree().process_frame
	return true


func _visible_create_dialogs() -> Array[ConfirmationDialog]:
	var result: Array[ConfirmationDialog] = []
	for candidate in EditorInterface.get_base_control().find_children("*", "ConfirmationDialog", true, false):
		var dialog := candidate as ConfirmationDialog
		if dialog and dialog.visible and dialog.title.begins_with("Create New Node"):
			result.push_back(dialog)
	return result


func _create_node_from_shortcut() -> bool:
	_send_ctrl_a(true)
	_send_ctrl_a(false)
	await _frames(3)
	var dialogs := _visible_create_dialogs()
	if dialogs.size() != 1:
		push_error("Ctrl+A opened %d Create Node dialogs instead of exactly one." % dialogs.size())
		return false
	var dialog := dialogs[0]
	var search: LineEdit
	for candidate in dialog.find_children("*", "LineEdit", true, false):
		if candidate is LineEdit and candidate.is_visible_in_tree():
			search = candidate
			break
	if search == null:
		push_error("Create Node dialog has no visible search field.")
		return false
	search.text = "Node"
	search.text_changed.emit("Node")
	await _frames(2)
	dialog.confirmed.emit()
	await _frames(3)
	return true


func _find_section(tree: Tree) -> FoldableContainer:
	var node: Node = tree
	while node:
		if node is FoldableContainer:
			return node
		node = node.get_parent()
	return null


func _find_background_tab_bar(scene_tree: Tree) -> TabBar:
	# Resolve from the pane-local Scene Tree so the legacy global scene-tab strip cannot satisfy this
	# lookup merely because it also contains the fixture scene names.
	var scope: Node = scene_tree
	while scope:
		if scope.get_class() == "TabbedDocumentHost":
			for candidate in scope.find_children("*", "TabBar", true, false):
				var tabs := candidate as TabBar
				if tabs and tabs.tab_count > 1:
					return tabs
			return null
		scope = scope.get_parent()
	return null


func _find_visible_code_edit(node: Node) -> CodeEdit:
	if node is CodeEdit and node.is_visible_in_tree():
		return node
	for child in node.get_children(true):
		var code_edit := _find_visible_code_edit(child)
		if code_edit:
			return code_edit
	return null


func _run_test() -> void:
	await _frames(35)

	var left_root: Node
	var right_root: Node
	for root in EditorInterface.get_open_scene_roots():
		if root.scene_file_path == "res://test_2d.tscn":
			left_root = root
		elif root.scene_file_path == "res://test_3d.tscn":
			right_root = root
	if left_root == null or right_root == null:
		push_error("The shortcut routing fixture requires both visible scene documents.")
		return

	var left_target := left_root.get_node_or_null("Sprite2D")
	var right_target := right_root.get_node_or_null("Camera3D")
	if left_target == null or right_target == null:
		push_error("Could not resolve both scene target nodes.")
		return
	var left_tree := _find_scene_tree(left_target)
	var right_tree := _find_scene_tree(right_target)
	if left_tree == null or right_tree == null:
		push_error("Could not resolve both pane-hosted Scene Trees.")
		return

	# Focus each visible pane and prove Ctrl+A mutates only the selected document. Undo immediately
	# also proves the action landed in that same document's history.
	var left_before := left_target.get_child_count()
	var right_before := right_target.get_child_count()
	if not await _focus_scene_tree_node(left_tree, left_target):
		push_error("Could not focus the left Scene Tree target.")
		return
	if EditorInterface.get_edited_scene_root() != left_root or not await _create_node_from_shortcut():
		push_error("Ctrl+A did not route to the focused left scene.")
		return
	if left_target.get_child_count() != left_before + 1 or right_target.get_child_count() != right_before:
		push_error("Left-pane creation changed the wrong scene document.")
		return
	_send_ctrl_z(true)
	_send_ctrl_z(false)
	await _frames(2)
	if left_target.get_child_count() != left_before or right_target.get_child_count() != right_before:
		push_error("Undo did not remove the node from the originating left document.")
		return

	if not await _focus_scene_tree_node(right_tree, right_target):
		push_error("Could not focus the right Scene Tree target.")
		return
	if EditorInterface.get_edited_scene_root() != right_root or not await _create_node_from_shortcut():
		push_error("Ctrl+A did not route to the focused right scene.")
		return
	if right_target.get_child_count() != right_before + 1 or left_target.get_child_count() != left_before:
		push_error("Right-pane creation changed the wrong scene document.")
		return
	_send_ctrl_z(true)
	_send_ctrl_z(false)
	await _frames(2)
	if right_target.get_child_count() != right_before or left_target.get_child_count() != left_before:
		push_error("Undo did not remove the node from the originating right document.")
		return

	# Folding hides the shortcut-bearing Add button. The dock's direct handler must still use the
	# focused DocumentView context, and exactly one dialog must be created.
	var section := _find_section(right_tree)
	if section == null:
		push_error("Could not find the right Scene Tree accordion section.")
		return
	section.fold()
	section.grab_focus()
	await _frames(2)
	var folded_left_before := _scene_node_count(left_root)
	var folded_right_before := _scene_node_count(right_root)
	if not await _create_node_from_shortcut():
		return
	if _scene_node_count(right_root) != folded_right_before + 1 or _scene_node_count(left_root) != folded_left_before:
		push_error("Folded Scene Tree shortcut did not stay in the focused right document.")
		return
	_send_ctrl_z(true)
	_send_ctrl_z(false)
	await _frames(2)
	if _scene_node_count(right_root) != folded_right_before or _scene_node_count(left_root) != folded_left_before:
		push_error("Folded Scene Tree undo did not stay in the originating right document.")
		return
	section.expand()
	await _frames(2)

	# A programmatic tab switch in the background pane must not steal the active scene.
	if not await _focus_scene_tree_node(right_tree, right_target) or EditorInterface.get_edited_scene_root() != right_root:
		push_error("Could not establish right-pane focus for the background-tab probe.")
		return
	var background_tabs := _find_background_tab_bar(left_tree)
	if background_tabs == null:
		push_error("Could not find the multi-tab background pane.")
		return
	var original_tab := background_tabs.current_tab
	var alternate_tab := 1 if original_tab == 0 else 0
	background_tabs.current_tab = alternate_tab
	await _frames(2)
	if EditorInterface.get_edited_scene_root() != right_root:
		push_error("A programmatic background tab change stole the active scene.")
		return
	background_tabs.current_tab = original_tab
	await _frames(2)

	# Ctrl+A belongs to CodeEdit while a script tab is focused; no Scene Tree dialog may appear.
	EditorInterface.edit_script(get_script(), 1, 0, true)
	await _frames(12)
	var code_edit := _find_visible_code_edit(EditorInterface.get_base_control())
	if code_edit == null:
		push_error("Could not find the focused script CodeEdit.")
		return
	code_edit.grab_focus()
	_send_ctrl_a(true)
	_send_ctrl_a(false)
	await _frames(3)
	if not code_edit.has_selection() or code_edit.get_selected_text().is_empty():
		push_error("Ctrl+A did not select script text in CodeEdit.")
		return
	if not _visible_create_dialogs().is_empty():
		push_error("Ctrl+A opened a Create Node dialog while CodeEdit owned focus.")
		return

	print("SCENE_TREE_SHORTCUT_ROUTING_OK")
