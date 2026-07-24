@tool
extends EditorPlugin


func _enter_tree() -> void:
	_run_test()


func _run_test() -> void:
	# Begin on test_3d.tscn, where the editor preview environment is active, then take the
	# ordinary open-scene path into a document with its own WorldEnvironment. Scene plugin state
	# restoration runs before edited_scene_changed() recounts the incoming document, which is the
	# ordering that previously overlaid and then cleared the scene resource.
	for frame in 20:
		await get_tree().process_frame

	EditorInterface.open_scene_from_path("res://test_3d_environment.tscn")
	for frame in 20:
		await get_tree().process_frame

	var root := EditorInterface.get_edited_scene_root() as Node3D
	if root == null or root.scene_file_path != "res://test_3d_environment.tscn":
		push_error("Preview environment smoke did not activate the environment scene document.")
		return

	var world_environment := root.get_node_or_null("WorldEnvironment") as WorldEnvironment
	if world_environment == null:
		push_error("Could not resolve the WorldEnvironment fixture.")
		return
	if root.get_world_3d().environment != world_environment.environment:
		push_error("The editor preview shadowed or cleared the document's scene environment.")
		return

	print("PREVIEW_ENVIRONMENT_HANDOFF_OK")
