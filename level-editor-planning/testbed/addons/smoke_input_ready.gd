@tool
extends RefCounted


const QUIET_FRAMES := 20
const MAX_WAIT_FRAMES := 240


static func _blocking_progress_visible(window: Window) -> bool:
	for candidate in window.find_children("*", "Control", true, false):
		if candidate.get_class() == "ProgressDialog" and candidate.is_visible_in_tree():
			return true
	return false


static func wait_for_level_view(tree: SceneTree, view: Control, container: Control,
		expected_scene_root: Node) -> String:
	if view == null or container == null or expected_scene_root == null:
		return "The level view, viewport container, or expected active scene root is missing."

	var stable_frames := 0
	var waited_frames := 0
	var last_rect := Rect2()
	while waited_frames < MAX_WAIT_FRAMES:
		await tree.process_frame
		waited_frames += 1
		if not is_instance_valid(view) or not is_instance_valid(container) or not is_instance_valid(expected_scene_root):
			return "The level view, viewport container, or expected scene root was freed while waiting for input readiness."

		var window := view.get_window()
		var rect := container.get_global_rect()
		var ready := window != null and view.is_visible_in_tree() and container.is_visible_in_tree() and rect.has_area()
		ready = ready and view.get_last_exclusive_window() == window
		ready = ready and not _blocking_progress_visible(window)
		ready = ready and EditorInterface.get_edited_scene_root() == expected_scene_root
		if ready and rect == last_rect:
			stable_frames += 1
		else:
			stable_frames = 0
		last_rect = rect

		if stable_frames < QUIET_FRAMES:
			continue

		container.grab_focus()
		await tree.process_frame
		waited_frames += 1
		window = view.get_window()
		if window != null and view.is_visible_in_tree() and container.is_visible_in_tree() and \
				container.has_focus() and container.get_global_rect() == last_rect and \
				view.get_last_exclusive_window() == window and not _blocking_progress_visible(window) and \
				EditorInterface.get_edited_scene_root() == expected_scene_root:
			return ""
		stable_frames = 0
		last_rect = container.get_global_rect()

	return "Level viewport input did not become ready: blocking progress/layout work remained or the Level document was not active."
