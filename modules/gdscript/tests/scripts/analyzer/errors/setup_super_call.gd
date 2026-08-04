extends Node

class Base extends Node:
	@setup
	func _shared_setup() -> void:
		pass

class Derived extends Base:
	@setup
	func _shared_setup() -> void:
		# The chain already ran the base implementation, so this would run it twice.
		super._shared_setup()

func test():
	pass
