extends Node

class BaseConfig:
	@field_meta("origin", "base")
	var inherited_value := 1

class DerivedConfig extends BaseConfig:
	pass

@field_meta("simulated")
@field_meta("since", "0.4")
@field_meta("limits", { "min": 0.0, "max": 10.0 })
@export_range(0.0, 10.0, 0.1) var spread_base := 1.2

@field_meta("native", false)
static var cached_value := 4

func test():
	@warning_ignore("unsafe_cast")
	var script := get_script() as GDScript
	var metadata: Dictionary = script.get_member_metadata(&"spread_base")
	print(metadata["simulated"])
	print(metadata["since"])
	print(metadata["limits"]["max"])

	# Callers receive a deep copy, so runtime inspection cannot mutate
	# metadata compiled into the script.
	metadata["simulated"] = false
	metadata["limits"]["max"] = 20.0
	var fresh_metadata: Dictionary = script.get_member_metadata(&"spread_base")
	print(fresh_metadata["simulated"])
	print(fresh_metadata["limits"]["max"])

	print(script.get_member_metadata(&"cached_value")["native"])
	print(script.get_member_metadata(&"missing").is_empty())

	var derived := DerivedConfig.new()
	@warning_ignore("unsafe_cast")
	var derived_script := derived.get_script() as GDScript
	print(derived_script.get_member_metadata(&"inherited_value")["origin"])
