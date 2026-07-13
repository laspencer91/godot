@tool
extends EditorPlugin


func _enter_tree() -> void:
	_run_test()


func _send_alt_f(pressed: bool) -> void:
	var event := InputEventKey.new()
	event.keycode = KEY_F
	event.alt_pressed = true
	event.pressed = pressed
	Input.parse_input_event(event)


func _run_test() -> void:
	for frame in 30:
		await get_tree().process_frame

	# The workspace fixture keeps test_3d visible in a background pane. Ask a synthetic gizmo for that
	# node's structural editability while another document owns global focus. Before the document-aware
	# fix this emits Node::is_editable_instance's !is_ancestor_of error.
	var active_root := EditorInterface.get_edited_scene_root()
	if active_root == null or active_root.scene_file_path == "res://test_3d.tscn":
		push_error("The 3D context probe must run with another document focused.")
		return
	var camera: Camera3D
	for root in EditorInterface.get_open_scene_roots():
		if root.scene_file_path == "res://test_3d.tscn":
			camera = root.get_node("Camera3D")
			break
	if camera == null:
		push_error("Could not resolve the background Camera3D fixture.")
		return
	var probe_gizmo := EditorNode3DGizmo.new()
	probe_gizmo.set_node_3d(camera)
	var probe_plugin := EditorNode3DGizmoPlugin.new()
	probe_plugin.create_material("context_probe", Color.WHITE)
	probe_plugin.get_material("context_probe", probe_gizmo)

	# "Show in FileSystem" semantics: reveal the unmanaged drawer, select the requested resource, and
	# do not redirect keyboard focus into search. The stock Alt+F action then toggles it closed/open and
	# keyboard opening must focus the active FileSystem filter.
	EditorInterface.get_file_system_dock().navigate_to_path("res://test_resource.tres")
	for frame in 3:
		await get_tree().process_frame
	if not EditorInterface.get_selected_paths().has("res://test_resource.tres"):
		push_error("Show in FileSystem did not select test_resource.tres.")
		return
	var navigation_focus := EditorInterface.get_base_control().get_viewport().gui_get_focus_owner()
	if navigation_focus is LineEdit and navigation_focus.placeholder_text == "Filter Files":
		push_error("Show in FileSystem incorrectly focused the FileSystem filter.")
		return

	_send_alt_f(true)
	_send_alt_f(false)
	await get_tree().process_frame
	_send_alt_f(true)
	_send_alt_f(false)
	for frame in 3:
		await get_tree().process_frame
	var shortcut_focus := EditorInterface.get_base_control().get_viewport().gui_get_focus_owner()
	if not shortcut_focus is LineEdit or shortcut_focus.placeholder_text != "Filter Files":
		push_error("Alt+F did not focus the FileSystem filter when opening the drawer.")
		return

	print("WORKSPACE_CONTEXT_ROUTES_OK")
