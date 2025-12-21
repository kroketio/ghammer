extends IOEntity
## math_counter: a number with optional limits (min = max = 0: none). Every
## change fires OutValue(value); reaching a limit fires OnHitMin / OnHitMax,
## leaving one OnChangedFromMin / OnChangedFromMax.

signal out_value(activator: Node, param: Variant)
signal on_hit_min(activator: Node, param: Variant)
signal on_hit_max(activator: Node, param: Variant)
signal on_get_value(activator: Node, param: Variant)
signal on_changed_from_min(activator: Node, param: Variant)
signal on_changed_from_max(activator: Node, param: Variant)

var value := 0.0
var min_value := 0.0
var max_value := 0.0


func _ready() -> void:
	enabled = not prop("start_disabled", false)
	value = float(prop("start_value", 0.0))
	min_value = float(prop("min", 0.0))
	max_value = float(prop("max", 0.0))


func set_value(v: float, fire := true, activator: Node = null) -> void:
	var limited := max_value > min_value
	var old := value
	if limited:
		v = clampf(v, min_value, max_value)
	value = v
	if not fire or v == old:
		return
	fire_output("OutValue", activator, v)
	if not limited:
		return
	if v == max_value:
		fire_output("OnHitMax", activator)
	if v == min_value:
		fire_output("OnHitMin", activator)
	if old == max_value and v != max_value:
		fire_output("OnChangedFromMax", activator)
	if old == min_value and v != min_value:
		fire_output("OnChangedFromMin", activator)


func input_add(p, a: Node = null) -> void:
	if enabled:
		set_value(value + IOParam.to_float(p), true, a)


func input_subtract(p, a: Node = null) -> void:
	if enabled:
		set_value(value - IOParam.to_float(p), true, a)


func input_multiply(p, a: Node = null) -> void:
	if enabled:
		set_value(value * IOParam.to_float(p), true, a)


func input_divide(p, a: Node = null) -> void:
	var d := IOParam.to_float(p)
	if enabled and d != 0.0:
		set_value(value / d, true, a)


func input_set_value(p, a: Node = null) -> void:
	if enabled:
		set_value(IOParam.to_float(p), true, a)


func input_set_value_no_fire(p) -> void:
	if enabled:
		set_value(IOParam.to_float(p), false)


func input_get_value(_p, a: Node = null) -> void:
	fire_output("OnGetValue", a, value)


func input_set_hit_max(p) -> void:
	max_value = IOParam.to_float(p, max_value)


func input_set_hit_min(p) -> void:
	min_value = IOParam.to_float(p, min_value)


func input_enable() -> void:
	enabled = true


func input_disable() -> void:
	enabled = false
