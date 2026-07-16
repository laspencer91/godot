@tool
extends EditorPlugin


func _enter_tree() -> void:
	_run_test()


func _find_preview_menu(root: Node) -> PopupMenu:
	for candidate in root.find_children("*", "PopupMenu", true, false):
		var popup := candidate as PopupMenu
		if popup == null:
			continue
		for index in popup.item_count:
			if popup.get_item_text(index) == "Floating Camera Preview":
				return popup
	return null


func _find_preview_panel(root: Node) -> PanelContainer:
	for candidate in root.find_children("*", "Label", true, false):
		var label := candidate as Label
		if label == null or label.text != "Camera Preview":
			continue
		var ancestor := label.get_parent()
		while ancestor:
			if ancestor is PanelContainer:
				return ancestor as PanelContainer
			ancestor = ancestor.get_parent()
	return null


func _find_visible_button(root: Node, name: StringName) -> Button:
	for candidate in root.find_children(String(name), "Button", true, false):
		var button := candidate as Button
		if button and button.is_visible_in_tree():
			return button
	return null


func _find_visible_control(name: StringName) -> Control:
	for candidate in EditorInterface.get_base_control().find_children(String(name), "Control", true, false):
		var control := candidate as Control
		if control and control.is_visible_in_tree():
			return control
	return null


func _find_inspector_section(root: Node) -> FoldableContainer:
	for candidate in root.find_children("*", "FoldableContainer", true, false):
		var section := candidate as FoldableContainer
		if section and section.is_visible_in_tree() and section.title == "Inspector":
			return section
	return null


func _run_test() -> void:
	# Plugin initialization precedes workspace construction and scene activation.
	for frame in 30:
		await get_tree().process_frame

	# Scope every lookup to the document that owns the Animation toggle. The editor also retains
	# a parked legacy 3D view whose View menu has the same item; choosing that global-first menu
	# would open a preview in a different surface and make the drawer assertion meaningless.
	var document_scope: Node = EditorInterface.get_inspector()
	var animation_toggle: Button
	while document_scope:
		animation_toggle = _find_visible_button(document_scope, &"AnimationBottomDockToggle")
		if animation_toggle and _find_preview_menu(document_scope) and _find_inspector_section(document_scope):
			break
		document_scope = document_scope.get_parent()
	if document_scope == null or animation_toggle == null:
		push_error("Could not resolve the active scene document containing the Animation toggle.")
		return

	var popup := _find_preview_menu(document_scope)
	if popup == null:
		push_error("Could not find the Floating Camera Preview menu item.")
		return

	var item_index := -1
	for index in popup.item_count:
		if popup.get_item_text(index) == "Floating Camera Preview":
			item_index = index
			break
	if item_index < 0:
		push_error("Could not resolve the Floating Camera Preview menu index.")
		return

	var item_id := popup.get_item_id(item_index)
	popup.id_pressed.emit(item_id)
	for frame in 3:
		await get_tree().process_frame
	if not popup.is_item_checked(item_index):
		push_error("Floating Camera Preview did not become checked.")
		return
	var preview := _find_preview_panel(document_scope)
	if preview == null:
		push_error("Could not find the open Floating Camera Preview panel.")
		return
	var panel_style := preview.get_theme_stylebox("panel") as StyleBoxFlat
	if panel_style == null or not is_equal_approx(panel_style.bg_color.a, 1.0):
		push_error("Floating Camera Preview panel is not opaque.")
		return
	for side in [SIDE_LEFT, SIDE_TOP, SIDE_RIGHT, SIDE_BOTTOM]:
		if not is_equal_approx(preview.get_anchor(side), 1.0):
			push_error("Floating Camera Preview is not anchored to the viewport's bottom-right corner.")
			return

	# The Animation drawer belongs to this document's viewport column. Opening it must lift the
	# bottom-anchored preview while leaving the right Scene Tree/Inspector column full-height.
	var inspector_section := _find_inspector_section(document_scope)
	if inspector_section == null:
		push_error("Could not find the document Inspector section.")
		return
	var right_column := inspector_section.get_parent() as Control
	var preview_bottom_before := preview.get_global_rect().end.y
	var right_height_before := right_column.size.y
	animation_toggle.toggled.emit(true)
	for frame in 3:
		await get_tree().process_frame
	var animation_panel := _find_visible_control(&"AnimationBottomDockPanel")
	if animation_panel == null:
		push_error("Document Animation drawer did not open.")
		return
	if preview.get_global_rect().end.y >= preview_bottom_before - 1.0:
		push_error("Animation drawer did not push the Camera Preview upward (before=%.1f, after=%.1f, drawer_top=%.1f)." % [preview_bottom_before, preview.get_global_rect().end.y, animation_panel.get_global_rect().position.y])
		return
	if not is_equal_approx(right_column.size.y, right_height_before):
		push_error("Animation drawer changed the right dock column height.")
		return
	if animation_panel.get_global_rect().end.x > right_column.get_global_rect().position.x + 8.0:
		push_error("Animation drawer overlaps the right dock column.")
		return
	animation_toggle.toggled.emit(false)
	await get_tree().process_frame

	popup.id_pressed.emit(item_id)
	await get_tree().process_frame
	if popup.is_item_checked(item_index):
		push_error("Floating Camera Preview did not become unchecked.")
		return

	popup.id_pressed.emit(item_id)
	for frame in 3:
		await get_tree().process_frame
	if not popup.is_item_checked(item_index):
		push_error("Floating Camera Preview did not reopen.")
		return

	popup.id_pressed.emit(item_id)
	await get_tree().process_frame
	print("FLOATING_CAMERA_PREVIEW_TOGGLE_OK")
