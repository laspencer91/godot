extends Node

# Every `@setup` function in the hierarchy runs, base-first, without `super()`.
# The annotation is the identity of the hook, so the names need not match.

class Base extends Node:
	var trace: Array = []

	@setup
	func _base_setup() -> void:
		trace.append("base")

class Middle extends Base:
	@setup
	func _middle_setup() -> void:
		trace.append("middle")

class Leaf extends Middle:
	@onready var deferred_value := "onready-was-assigned"
	var seen_during_setup := "onready-was-missing"

	@setup
	func _leaf_setup() -> void:
		trace.append("leaf")
		# `@onready` assignments happen before the setup chain.
		seen_during_setup = deferred_value

	func _ready() -> void:
		trace.append("ready")

# A hierarchy where only the base opts in still runs it.
class BaseOnly extends Node:
	var trace: Array = []

	@setup
	func _base_only_setup() -> void:
		trace.append("base-only")

class DerivedWithoutSetup extends BaseOnly:
	pass

func test():
	# Drive readiness the way the engine does, through the notification, rather
	# than by calling `_ready` directly: `Object.call()` requires the method to
	# exist, while ready dispatch reaches the script unconditionally.
	var leaf := Leaf.new()
	leaf.notification(NOTIFICATION_READY)
	print(leaf.trace)
	print(leaf.seen_during_setup)
	leaf.free()

	# The derived class defines neither `@setup` nor `_ready`, but the base
	# hook must still run.
	var derived := DerivedWithoutSetup.new()
	derived.notification(NOTIFICATION_READY)
	print(derived.trace)
	derived.free()
