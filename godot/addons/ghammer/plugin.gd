@tool
extends EditorPlugin
## Registers the IO autoload while the plugin is enabled.

const AUTOLOAD := "IO"


func _enter_tree() -> void:
	add_autoload_singleton(AUTOLOAD, "res://addons/ghammer/io.gd")


func _exit_tree() -> void:
	remove_autoload_singleton(AUTOLOAD)
