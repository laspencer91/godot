@tool
extends EditorPlugin

# One step of the engine's 150 ms debounce window. Two steps must overrun it and one must not, so
# every assertion below keeps a 50 ms margin.
const DEBOUNCE_STEP := 0.1


func _enter_tree() -> void:
	_run_test()


func _visible_item_count(tree: Tree) -> int:
	# Walks collapsed subtrees too (unlike get_next_visible), so the count tracks filter visibility
	# alone and is not perturbed by the folder expansion a search triggers.
	var count := 0
	var item := tree.get_root()
	while item:
		if item.is_visible():
			count += 1
		item = item.get_next_in_tree()
	return count


func _type_search(search_box: LineEdit, text: String) -> void:
	# LineEdit.set_text() deliberately does not emit text_changed, so stand in for the keystroke.
	search_box.text = text
	search_box.text_changed.emit(text)


func _run_test() -> void:
	for frame in 30:
		await get_tree().process_frame

	var file_system_dock := EditorInterface.get_file_system_dock()

	var search_boxes: Array[LineEdit] = []
	for candidate in file_system_dock.find_children("*", "LineEdit", true, false):
		var line_edit := candidate as LineEdit
		if line_edit and line_edit.placeholder_text == "Filter Assets":
			search_boxes.push_back(line_edit)
	if search_boxes.size() != 2:
		push_error("Expected both synchronized Explore search fields.")
		return

	# Likewise, the asset tree is the one holding the res:// root item.
	var asset_tree: Tree
	for candidate in file_system_dock.find_children("*", "Tree", true, false):
		var tree := candidate as Tree
		if tree == null:
			continue
		var item := tree.get_root()
		while item:
			if item.get_metadata(0) == "res://":
				asset_tree = tree
				break
			item = item.get_next_in_tree()
		if asset_tree:
			break
	if asset_tree == null:
		push_error("Could not resolve the Explore asset tree.")
		return

	var initial_count := _visible_item_count(asset_tree)
	if initial_count < 3:
		push_error("Explore debounce fixture did not populate enough asset-tree items.")
		return

	# A later keystroke must restart the trailing-edge delay. At this checkpoint the first query's
	# original window has elapsed, but the final query's window has not.
	_type_search(search_boxes[0], "codex_search_debounce_first")
	if search_boxes[1].text != "codex_search_debounce_first":
		push_error("Explore search fields did not synchronize immediately.")
		return
	await get_tree().create_timer(DEBOUNCE_STEP).timeout
	_type_search(search_boxes[0], "codex_search_debounce_final")
	await get_tree().create_timer(DEBOUNCE_STEP).timeout
	if _visible_item_count(asset_tree) != initial_count:
		push_error("Explore filtering ran before the final debounce window elapsed.")
		return

	await get_tree().create_timer(DEBOUNCE_STEP * 2).timeout
	var filtered_count := _visible_item_count(asset_tree)
	if filtered_count >= initial_count:
		push_error("Explore filtering did not run after the debounce window elapsed.")
		return

	# Enter commits a pending query synchronously.
	_type_search(search_boxes[0], "codex_search_submit")
	search_boxes[0].text_submitted.emit("codex_search_submit")
	if _visible_item_count(asset_tree) >= initial_count:
		push_error("Submitting an Explore search did not apply it immediately.")
		return

	# The clear button goes through LineEdit.clear(), which is a distinct entry point; it must
	# restore and synchronize without waiting so the button stays responsive.
	search_boxes[0].clear()
	if _visible_item_count(asset_tree) != initial_count or not search_boxes[1].text.is_empty():
		push_error("Clearing Explore search did not restore and synchronize immediately.")
		return

	print("EXPLORE_SEARCH_DEBOUNCE_OK")
