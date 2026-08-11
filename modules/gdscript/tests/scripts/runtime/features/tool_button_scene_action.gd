@tool
extends Node

# The third argument opts the button into the editor's Scene Actions menu. It is
# stored as member metadata, never in the hint string, which the inspector
# splits as `<text>[,<icon>]`.
@export_tool_button("Bake Field", "Bake", true) var bake_action: Callable
@export_tool_button("Clear Field", "Remove") var clear_action: Callable
@export_tool_button("Rebuild", "Reload", false) var rebuild_action: Callable

# The equivalent long form, for a button that wants other metadata anyway.
@field_meta("scene_action")
@field_meta("since", "0.5")
@export_tool_button("Bake Probes", "Bake") var probe_action: Callable

func test():
	@warning_ignore("unsafe_cast")
	var script := get_script() as GDScript
	print(script.get_member_metadata(&"bake_action").get("scene_action", false))
	print(script.get_member_metadata(&"clear_action").is_empty())
	print(script.get_member_metadata(&"rebuild_action").is_empty())
	print(script.get_member_metadata(&"probe_action").get("scene_action", false))
	print(script.get_member_metadata(&"probe_action").get("since", ""))
