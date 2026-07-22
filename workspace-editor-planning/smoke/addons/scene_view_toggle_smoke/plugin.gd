@tool
extends EditorPlugin


func _enter_tree() -> void:
	_run_test()


func _document_scope() -> Node:
	var scope: Node = EditorInterface.get_inspector()
	while scope:
		if scope.get_class() == "DocumentView":
			return scope
		scope = scope.get_parent()
	return null


func _surface(stack: Node, p_class_name: String, p_visible: bool) -> Control:
	for candidate in stack.get_children():
		var control := candidate as Control
		if control and control.get_class() == p_class_name and control.is_visible_in_tree() == p_visible:
			return control
	return null


func _visible_mode_button(scope: Node, text: String) -> Button:
	for candidate in scope.find_children("*", "Button", true, false):
		var button := candidate as Button
		if button and button.text == text and button.is_visible_in_tree():
			return button
	return null


func _visible_menu_button(scope: Node, text: String) -> MenuButton:
	for candidate in scope.find_children("*", "MenuButton", true, false):
		var button := candidate as MenuButton
		if button and button.text == text and button.is_visible_in_tree():
			return button
	return null


func _run_test() -> void:
	# Plugin initialization precedes workspace construction and scene activation.
	for frame in 40:
		await get_tree().process_frame

	var scope := _document_scope()
	if scope == null:
		push_error("Could not resolve the active scene DocumentView.")
		return
	var stack := scope.find_child("SceneSurfaceStack", true, false)
	if stack == null:
		push_error("Scene DocumentView has no 2D/3D surface stack.")
		return

	var spatial := _surface(stack, "Node3DEditorView", true)
	if spatial == null:
		push_error("3D scene did not start on its 3D document surface.")
		return
	var button_2d := _visible_mode_button(scope, "2D")
	var button_3d := _visible_mode_button(scope, "3D")
	if button_2d == null or button_3d == null or not button_3d.button_pressed:
		push_error("3D toolbar does not expose the expected 2D/3D selector state.")
		return
	var transform_menu := _visible_menu_button(scope, "Transform")
	var view_menu := _visible_menu_button(scope, "View")
	if transform_menu == null or view_menu == null or button_2d.get_parent() != transform_menu.get_parent() or button_3d.get_index() + 2 != transform_menu.get_index() or transform_menu.get_index() >= view_menu.get_index():
		push_error("3D toolbar selector is not positioned immediately before Transform and View.")
		return

	button_2d.pressed.emit()
	for frame in 5:
		await get_tree().process_frame
	var canvas := _surface(stack, "CanvasItemEditorView", true)
	if canvas == null or spatial.is_visible_in_tree():
		push_error("2D selector did not swap the scene to its 2D canvas surface.")
		return
	button_2d = _visible_mode_button(scope, "2D")
	button_3d = _visible_mode_button(scope, "3D")
	if button_2d == null or button_3d == null or not button_2d.button_pressed:
		push_error("2D toolbar does not retain the expected 2D/3D selector state.")
		return
	view_menu = _visible_menu_button(scope, "View")
	if view_menu == null or button_2d.get_parent() != view_menu.get_parent() or button_3d.get_index() + 2 != view_menu.get_index():
		push_error("2D toolbar selector is not positioned immediately before View.")
		return

	button_3d.pressed.emit()
	for frame in 5:
		await get_tree().process_frame
	if _surface(stack, "Node3DEditorView", true) != spatial or canvas.is_visible_in_tree():
		push_error("3D selector did not restore the retained 3D document surface.")
		return
	print("SCENE_VIEW_TOGGLE_OK")
