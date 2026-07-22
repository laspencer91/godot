@tool
extends EditorPlugin


var registrations: Array[EditorViewportChromeRegistration] = []
var controls: Dictionary = {
	"2d_low": [],
	"2d_high": [],
	"3d_view": [],
	"3d_subviewport": [],
}


func _enter_tree() -> void:
	registrations.append(add_control_to_viewport_chrome(
		&"2d",
		VIEWPORT_CHROME_TOP_LEFT,
		Callable(self, "_make_2d_low"),
		0,
	))
	registrations.append(add_control_to_viewport_chrome(
		&"2d",
		VIEWPORT_CHROME_TOP_LEFT,
		Callable(self, "_make_2d_high"),
		100,
	))
	registrations.append(add_control_to_viewport_chrome(
		&"3d",
		VIEWPORT_CHROME_TOP_LEFT,
		Callable(self, "_make_3d_view"),
	))
	registrations.append(add_control_to_viewport_chrome(
		&"3d",
		VIEWPORT_CHROME_TOP_RIGHT,
		Callable(self, "_make_3d_subviewport"),
		0,
		VIEWPORT_CHROME_SCOPE_SUBVIEWPORT,
	))
	_run_test.call_deferred()


func _exit_tree() -> void:
	for registration in registrations:
		if registration and registration.is_registered():
			remove_control_from_viewport_chrome(registration)
	registrations.clear()


func _make_button(text: String) -> Button:
	var button := Button.new()
	button.text = text
	button.theme_type_variation = &"ViewportButton"
	return button


func _record(kind: String, context: Dictionary, control: Control) -> Control:
	if context.get("editor_id", &"") == &"":
		push_error("Viewport chrome context has no editor_id.")
	if not context.has("scope") or not context.has("view"):
		push_error("Viewport chrome context is missing scope or view.")
	controls[kind].append(weakref(control))
	return control


func _make_2d_low(context: Dictionary) -> Control:
	if not context.has("overlay_control"):
		push_error("2D viewport chrome context has no overlay_control.")
	return _record("2d_low", context, _make_button("Chrome 2D Low"))


func _make_2d_high(context: Dictionary) -> Control:
	return _record("2d_high", context, _make_button("Chrome 2D High"))


func _make_3d_view(context: Dictionary) -> Control:
	return _record("3d_view", context, _make_button("Chrome 3D View"))


func _make_3d_subviewport(context: Dictionary) -> Control:
	if not context.has("viewport") or not context.has("viewport_index"):
		push_error("3D subviewport chrome context is incomplete.")
	return _record("3d_subviewport", context, _make_button("Chrome 3D Subviewport"))


func _live_controls(kind: String) -> Array[Control]:
	var result: Array[Control] = []
	for reference: WeakRef in controls[kind]:
		var control := reference.get_ref() as Control
		if control:
			result.append(control)
	return result


func _run_test() -> void:
	for frame in 40:
		await get_tree().process_frame

	for registration in registrations:
		if registration == null or not registration.is_registered():
			push_error("Viewport chrome registration was not retained by its EditorPlugin.")
			return

	var low := _live_controls("2d_low")
	var high := _live_controls("2d_high")
	var view_3d := _live_controls("3d_view")
	var subviewports_3d := _live_controls("3d_subviewport")
	if low.is_empty() or high.is_empty() or view_3d.is_empty() or subviewports_3d.size() < 4:
		push_error("Viewport chrome factories did not cover every expected view tier.")
		return
	if low[0].get_parent() != high[0].get_parent() or low[0].get_index() >= high[0].get_index():
		push_error("Viewport chrome order is not deterministic.")
		return
	for control in low + high + view_3d + subviewports_3d:
		if control.theme_type_variation != &"ViewportButton" or control.position.x < 0.0 or control.position.y < 0.0:
			push_error("Viewport chrome styling or safe-area layout was not applied.")
			return

	var removed_controls: Array[WeakRef] = []
	removed_controls.assign(controls["2d_high"])
	remove_control_from_viewport_chrome(registrations[1])
	await get_tree().process_frame
	if registrations[1].is_registered():
		push_error("Viewport chrome registration did not unregister.")
		return
	for reference in removed_controls:
		if reference.get_ref() != null:
			push_error("Unregistered viewport chrome control was not freed.")
			return

	print("VIEWPORT_CHROME_OK")
