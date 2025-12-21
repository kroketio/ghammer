extends IOEntity
## logic_compare: compares value with compare_value; fires OnLessThan /
## OnGreaterThan / OnEqualTo / OnNotEqualTo, passing the value.

signal on_less_than(activator: Node, param: Variant)
signal on_equal_to(activator: Node, param: Variant)
signal on_not_equal_to(activator: Node, param: Variant)
signal on_greater_than(activator: Node, param: Variant)

var value := 0.0
var compare_value := 0.0


func _ready() -> void:
	value = float(prop("initial_value", 0.0))
	compare_value = float(prop("compare_value", 0.0))


func _compare(activator: Node) -> void:
	if value < compare_value:
		fire_output("OnLessThan", activator, value)
	elif value > compare_value:
		fire_output("OnGreaterThan", activator, value)
	if value == compare_value:
		fire_output("OnEqualTo", activator, value)
	else:
		fire_output("OnNotEqualTo", activator, value)


func input_set_value(p) -> void:
	value = IOParam.to_float(p, value)


func input_set_value_compare(p, activator: Node = null) -> void:
	value = IOParam.to_float(p, value)
	_compare(activator)


func input_set_compare_value(p) -> void:
	compare_value = IOParam.to_float(p, compare_value)


func input_compare(_p, activator: Node = null) -> void:
	_compare(activator)
