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


func _has_selected_gizmo(node: Node3D) -> bool:
	for gizmo in node.get_gizmos():
		if gizmo.is_selected():
			return true
	return false


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
	# Multiple workspace panes may display instances of the same scene, so their TreeItem metadata
	# contains identical NodePaths. Resolve outward from the active document Inspector and only accept
	# the Scene Tree in that same document composite.
	var scope: Node = EditorInterface.get_inspector()
	while scope:
		for candidate in scope.find_children("*", "Tree", true, false):
			var tree := candidate as Tree
			if tree and tree.is_visible_in_tree() and _find_item(tree, node.get_path()):
				return tree
		scope = scope.get_parent()
	return null


func _find_inspector_lock() -> Button:
	# Resolve outward from the active document Inspector, just like the paired Scene Tree above.
	# A split workspace may have several visible lock buttons; a base-control-wide first match can
	# toggle a different document's Inspector and make the active one appear to ignore the lock.
	var scope: Node = EditorInterface.get_inspector()
	while scope:
		for candidate in scope.find_children("InspectorLock", "Button", true, false):
			var button := candidate as Button
			if button and button.is_visible_in_tree():
				return button
		scope = scope.get_parent()
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

	# SceneTreeDock's paste path adds the duplicate, assigns its edited-scene owner, and then changes
	# EditorSelection. In the pane workspace the document view remains active while the shared 3D
	# main surface stays hidden, so guard changes that never pass through the retired screen switcher.
	var pasted := dragged.duplicate() as Node3D
	pasted.name = "PastedCamera3D"
	root.add_child(pasted)
	pasted.owner = root
	_set_selection(pasted)
	for frame in 3:
		await get_tree().process_frame
	if pasted.get_gizmos().is_empty() or not _has_selected_gizmo(pasted):
		push_error("Pasted Node3D did not receive a selected gizmo in the active 3D pane.")
		return
	_set_selection(original)
	root.remove_child(pasted)
	pasted.queue_free()
	await get_tree().process_frame

	var tree := _find_scene_tree(dragged)
	if tree == null:
		push_error("Could not find the visible pane-hosted SceneTree.")
		return

	# Viewport picking changes EditorSelection directly. The pane-hosted SceneTree must promote that
	# model change to its own cursor/reveal state as well: otherwise a collapsed node stays hidden and
	# Add Child / Instantiate still use whichever row the user last clicked in the tree.
	var reveal_parent := Node3D.new()
	reveal_parent.name = "SelectionRevealParent"
	root.add_child(reveal_parent)
	reveal_parent.owner = root
	for index in 20:
		var filler := Node3D.new()
		filler.name = "RevealFiller%02d" % index
		reveal_parent.add_child(filler)
		filler.owner = root
	var reveal_target := Node3D.new()
	reveal_target.name = "SelectionRevealTarget"
	reveal_parent.add_child(reveal_target)
	reveal_target.owner = root
	for frame in 3:
		await get_tree().process_frame

	var reveal_parent_item := _find_item(tree, reveal_parent.get_path())
	var reveal_target_item := _find_item(tree, reveal_target.get_path())
	if reveal_parent_item == null or reveal_target_item == null:
		push_error("Scene Tree did not display the nested selection-reveal fixtures.")
		return
	reveal_parent_item.set_collapsed(true)
	tree.scroll_to_item(tree.get_root(), true)
	await get_tree().process_frame

	_set_selection(reveal_target)
	for frame in 3:
		await get_tree().process_frame
	reveal_parent_item = _find_item(tree, reveal_parent.get_path())
	reveal_target_item = _find_item(tree, reveal_target.get_path())
	if reveal_parent_item == null or reveal_target_item == null:
		push_error("Scene Tree lost the nested selection-reveal fixtures after selection.")
		return
	if reveal_parent_item.is_collapsed():
		push_error("Viewport-style selection did not expand the selected node's Scene Tree ancestors.")
		return
	if not reveal_target_item.is_selected(0):
		push_error("Viewport-style selection did not highlight the selected Scene Tree row.")
		return
	if tree.get_selected() != reveal_target_item:
		push_error("Pane SceneTree cursor did not follow viewport-style selection; child creation would use a stale parent.")
		return
	var reveal_rect := tree.get_item_area_rect(reveal_target_item, 0)
	if reveal_rect.position.y < 0.0 or reveal_rect.end.y > tree.size.y:
		push_error("Viewport-style selection did not scroll the selected Scene Tree row into view.")
		return

	_set_selection(original)
	for frame in 2:
		await get_tree().process_frame
	root.remove_child(reveal_parent)
	reveal_parent.queue_free()
	for frame in 2:
		await get_tree().process_frame

	# Establish the Inspector's starting object before deriving click geometry. Selecting it can
	# auto-scroll or re-layout a short pane-hosted Tree, invalidating coordinates captured earlier.
	_set_selection(original)
	await get_tree().process_frame
	if _edited_object() != original:
		push_error("Pane Inspector did not start on the original node.")
		return

	var dragged_item := _find_item(tree, dragged.get_path())
	# A restored multi-pane layout can leave the target row outside this pane's short Tree viewport.
	# Bring it into view before deriving screen coordinates for the synthetic click.
	tree.scroll_to_item(dragged_item, true)
	await get_tree().process_frame
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
	_send_button(viewport, click_position, true)
	await get_tree().process_frame
	if _edited_object() != original:
		push_error("Pane Inspector changed on mouse press instead of release.")
		return
	_send_button(viewport, click_position, false)
	await get_tree().process_frame
	if _edited_object() != dragged:
		push_error("Pane Inspector did not commit the clicked node on mouse release (tree=%s, inspector=%s)." % [tree.get_selected().get_metadata(0) if tree.get_selected() else "<none>", _edited_object()])
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

	# Locking is intentionally narrower than pinning/history: it freezes only the Scene Tree ->
	# Inspector route. Unlocking must immediately catch up to the selection made while locked.
	_set_selection(original)
	await get_tree().process_frame
	var inspector_lock := _find_inspector_lock()
	if inspector_lock == null:
		push_error("Could not find the document Inspector lock button.")
		return
	inspector_lock.toggled.emit(true)
	_set_selection(dragged)
	await get_tree().process_frame
	if _edited_object() != original:
		push_error("Locked Inspector followed a Scene Tree selection change.")
		return
	inspector_lock.toggled.emit(false)
	await get_tree().process_frame
	if _edited_object() != dragged:
		push_error("Unlocked Inspector did not catch up to the Scene Tree selection.")
		return
	print("SCENE_TREE_DRAG_SELECTION_OK")
