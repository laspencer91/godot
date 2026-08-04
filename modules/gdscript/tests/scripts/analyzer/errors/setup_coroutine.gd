extends Node

signal my_signal

@setup
func _my_setup() -> void:
	await my_signal

func test():
	pass
