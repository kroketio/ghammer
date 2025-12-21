extends IOEntity
## logic_timer: fires OnTimer every refire_time seconds, or at random intervals
## between the bounds; as an oscillator it alternates OnTimerLow / OnTimerHigh
## (Low first) instead.

signal on_timer(activator: Node, param: Variant)
signal on_timer_high(activator: Node, param: Variant)
signal on_timer_low(activator: Node, param: Variant)

var refire := 1.0
var use_random := false
var lower := 1.0
var upper := 5.0
var _left := -1.0
var _phase := 0


func _ready() -> void:
	enabled = not prop("start_disabled", false)
	refire = float(prop("refire_time", 1.0))
	use_random = prop("use_random_time", false)
	lower = float(prop("lower_random_bound", 1.0))
	upper = float(prop("upper_random_bound", 5.0))
	_arm()


func _interval() -> float:
	if use_random:
		return maxf(0.01, randf_range(minf(lower, upper), maxf(lower, upper)))
	return refire


func _arm() -> void:
	_left = -1.0
	if enabled:
		var dt := _interval()
		if dt > 0.0:
			_left = maxf(0.01, dt)


func _process(delta: float) -> void:
	if _left < 0.0 or not enabled:
		return
	_left -= delta
	if _left <= 0.0:
		_tick()
		_arm()


func _tick() -> void:
	if prop("oscillator", false):
		fire_output("OnTimerLow" if _phase == 0 else "OnTimerHigh")
		_phase ^= 1
	else:
		fire_output("OnTimer")


func input_refire_time(p) -> void:
	refire = IOParam.to_float(p, refire)
	_arm()


func input_reset_timer() -> void:
	_arm()


func input_fire_timer() -> void:
	_tick()
	_arm()


func input_enable() -> void:
	enabled = true
	_arm()


func input_disable() -> void:
	enabled = false
	_arm()


func input_toggle() -> void:
	enabled = not enabled
	_arm()


func input_lower_random_bound(p) -> void:
	lower = IOParam.to_float(p, lower)


func input_upper_random_bound(p) -> void:
	upper = IOParam.to_float(p, upper)


func input_use_random_time(p) -> void:
	use_random = IOParam.to_bool(p)
	if not use_random:
		refire = upper


func input_add_to_timer(p) -> void:
	if _left >= 0.0:
		_left += IOParam.to_float(p)


func input_subtract_from_timer(p) -> void:
	if _left >= 0.0:
		_left = maxf(0.0, _left - IOParam.to_float(p))
