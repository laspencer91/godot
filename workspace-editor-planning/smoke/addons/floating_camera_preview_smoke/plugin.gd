@tool
extends EditorPlugin


func _enter_tree() -> void:
	_run_test()


func _find_preview_menu() -> PopupMenu:
	for candidate in EditorInterface.get_base_control().find_children("*", "PopupMenu", true, false):
		var popup := candidate as PopupMenu
		if popup == null:
			continue
		for index in popup.item_count:
			if popup.get_item_text(index) == "Floating Camera Preview":
				return popup
	return null


func _find_preview_panel() -> PanelContainer:
	for candidate in EditorInterface.get_base_control().find_children("*", "Label", true, false):
		var label := candidate as Label
		if label == null or label.text != "Camera Preview":
			continue
		var ancestor := label.get_parent()
		while ancestor:
			if ancestor is PanelContainer:
				return ancestor as PanelContainer
			ancestor = ancestor.get_parent()
	return null


func _run_test() -> void:
	# Plugin initialization precedes workspace construction and scene activation.
	for frame in 30:
		await get_tree().process_frame

	var popup := _find_preview_menu()
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
	var preview := _find_preview_panel()
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
