@tool
extends EditorPlugin


func _enter_tree() -> void:
	_open_resource_after_startup()


func _open_resource_after_startup() -> void:
	# Plugin initialization precedes layout restore. Waiting a few frames ensures this exercises an
	# interactive edit request, rather than letting startup layout selection overwrite the result.
	for frame in 10:
		await get_tree().process_frame

	var resource := load("res://test_resource.tres")
	if resource == null:
		push_error("Failed to load the resource-tab smoke fixture.")
		return
	EditorInterface.edit_resource(resource)
