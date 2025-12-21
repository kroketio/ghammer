extends IOEntity
## logic_multicompare: collects integer values; CompareValues fires OnEqual when
## they are all equal (to each other, or to reference_value when
## should_compare_to_value is on), else OnNotEqual.

signal on_equal(activator: Node, param: Variant)
signal on_not_equal(activator: Node, param: Variant)

var values: Array[int] = []


func _compare(activator: Node) -> void:
	var to_ref: bool = prop("should_compare_to_value", false)
	var ref := int(prop("reference_value", 0)) if to_ref else (values[0] if not values.is_empty() else 0)
	var equal := not values.is_empty()
	for v in values:
		equal = equal and v == ref
	fire_output("OnEqual" if equal else "OnNotEqual", activator)


func input_value(p) -> void:
	values.append(IOParam.to_int(p))


func input_value_compare(p, activator: Node = null) -> void:
	values.append(IOParam.to_int(p))
	_compare(activator)


func input_compare_values(_p, activator: Node = null) -> void:
	_compare(activator)
