@tool
extends EditorPlugin


func _enter_tree() -> void:
	_run_test()


func _edited_object() -> Object:
	var inspector := EditorInterface.get_inspector()
	return inspector.get_edited_object() if inspector else null


func _set_selection(node: Node) -> void:
	var selection := EditorInterface.get_selection()
	selection.clear()
	selection.add_node(node)


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


func _find_item(tree: Tree, metadata: Variant) -> TreeItem:
	var item := tree.get_root()
	while item:
		if item.get_metadata(0) == metadata:
			return item
		item = item.get_next_in_tree()
	return null


func _find_scene_tree(node: Node) -> Tree:
	for candidate in EditorInterface.get_base_control().find_children("*", "Tree", true, false):
		var tree := candidate as Tree
		if tree and tree.is_visible_in_tree() and _find_item(tree, node.get_path()):
			return tree
	return null


func _run_test() -> void:
	# Plugin initialization precedes workspace construction and scene activation.
	for frame in 30:
		await get_tree().process_frame

	var root := EditorInterface.get_edited_scene_root()
	if root == null or root.scene_file_path != "res://test_3d.tscn":
		push_error("Scene Tree drag smoke did not activate test_3d.tscn.")
		return
	var original := root.get_node_or_null("MeshInstance3D")
	var dragged := root.get_node_or_null("Camera3D")
	if original == null or dragged == null:
		push_error("Scene Tree drag smoke fixtures are missing.")
		return

	var tree := _find_scene_tree(dragged)
	if tree == null:
		push_error("Could not find the visible pane-hosted SceneTree.")
		return
	var dragged_item := _find_item(tree, dragged.get_path())
	var item_rect := tree.get_item_area_rect(dragged_item, 0)
	var click_position: Vector2 = tree.get_global_transform_with_canvas() * item_rect.get_center()
	var viewport := tree.get_viewport()
	# SceneTreeDock's stock click deferral checks Control.get_local_mouse_position(). On a root Window
	# that comes from the OS cursor, not the coordinates supplied to Viewport.push_input(). Keep both
	# halves of the synthetic gesture in agreement.
	viewport.warp_mouse(click_position)
	await get_tree().process_frame

	# A normal click changes the Tree selection on press, but the Inspector must retain the old object
	# until release. This is what permits the same gesture to become a property drag instead.
	_set_selection(original)
	await get_tree().process_frame
	if _edited_object() != original:
		push_error("Pane Inspector did not start on the original node.")
		return
	_send_button(viewport, click_position, true)
	await get_tree().process_frame
	if _edited_object() != original:
		push_error("Pane Inspector changed on mouse press instead of release.")
		return
	_send_button(viewport, click_position, false)
	await get_tree().process_frame
	if _edited_object() != dragged:
		push_error("Pane Inspector did not commit the clicked node on mouse release.")
		return

	# Repeat the press, then cross the GUI drag threshold. The Inspector must remain a stable drop
	# target for the complete drag even though the SceneTree selection already contains dragged.
	_set_selection(original)
	await get_tree().process_frame
	_send_button(viewport, click_position, true)
	await get_tree().process_frame
	var previous_position := click_position
	for offset in [Vector2(8, 1), Vector2(24, 2), Vector2(48, 3)]:
		var motion_position: Vector2 = click_position + offset
		_send_motion(viewport, motion_position, motion_position - previous_position)
		previous_position = motion_position
		await get_tree().process_frame
		if viewport.gui_is_dragging():
			break
	if not viewport.gui_is_dragging():
		push_error("Synthetic SceneTree gesture did not begin a drag.")
		_send_button(viewport, previous_position, false)
		return
	if _edited_object() != original:
		push_error("Pane Inspector lost its original target during a SceneTree drag.")
		viewport.gui_cancel_drag()
		_send_button(viewport, previous_position, false)
		return

	viewport.gui_cancel_drag()
	_send_button(viewport, previous_position, false)
	print("SCENE_TREE_DRAG_SELECTION_OK")
