extends IOEntity
## logic_branch: holds a boolean; Test fires OnTrue or OnFalse.

signal on_true(activator: Node, param: Variant)
signal on_false(activator: Node, param: Variant)

var value := false


func _ready() -> void:
	value = prop("initial_value", false)


func _test(activator: Node) -> void:
	fire_output("OnTrue" if value else "OnFalse", activator)


func input_set_value(p) -> void:
	value = IOParam.to_bool(p)


func input_set_value_test(p, activator: Node = null) -> void:
	value = IOParam.to_bool(p)
	_test(activator)


func input_toggle() -> void:
	value = not value


func input_toggle_test(_p, activator: Node = null) -> void:
	value = not value
	_test(activator)


func input_test(_p, activator: Node = null) -> void:
	_test(activator)
