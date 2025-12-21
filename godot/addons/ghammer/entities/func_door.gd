extends IOEntity
## func_door: slides along move_angle (yaw in degrees; -1 up, -2 down) by its
## size minus `lip`, at `speed` (Source units per second), then closes again
## after `wait` seconds (-1 stays open). Children of this node move with it:
## put the door's mesh and collision under it.

signal on_open(activator: Node, param: Variant)
signal on_close(activator: Node, param: Variant)
signal on_fully_open(activator: Node, param: Variant)
signal on_fully_closed(activator: Node, param: Variant)
signal on_locked_use(activator: Node, param: Variant)

const SOURCE_UNIT := 0.0203125 # metres (128 Source units = 2.6 m)

var is_open := false
var locked := false
var speed := 100.0
var _closed_pos: Vector3
var _tween: Tween


func _ready() -> void:
	locked = prop("locked", false)
	speed = float(prop("speed", 100.0))
	_closed_pos = position


func _direction() -> Vector3:
	var a := float(prop("move_angle", 0.0))
	if a == -1.0:
		return Vector3.UP
	if a == -2.0:
		return Vector3.DOWN
	return Vector3(sin(deg_to_rad(a)), 0.0, cos(deg_to_rad(a)))


func _travel(dir: Vector3) -> float:
	var lo := Vector3(INF, INF, INF)
	var hi := -lo
	for h in hulls:
		for p in h:
			lo = lo.min(p)
			hi = hi.max(p)
	if lo.x > hi.x:
		return 0.0
	var size := (hi - lo).abs()
	var along := absf(dir.x) * size.x + absf(dir.y) * size.y + absf(dir.z) * size.z
	return maxf(0.0, along - float(prop("lip", 8.0)) * SOURCE_UNIT)


func move(open: bool, activator: Node = null) -> void:
	if locked:
		fire_output("OnLockedUse", activator)
		return
	if open == is_open and not (_tween and _tween.is_running()):
		return
	is_open = open
	fire_output("OnOpen" if open else "OnClose", activator)
	var dir := _direction()
	var goal := _closed_pos + dir * _travel(dir) if open else _closed_pos
	var secs := position.distance_to(goal) / maxf(0.001, speed * SOURCE_UNIT)
	if _tween:
		_tween.kill()
	_tween = create_tween()
	_tween.tween_property(self, "position", goal, secs)
	_tween.finished.connect(func():
		fire_output("OnFullyOpen" if is_open else "OnFullyClosed", activator)
		var wait := float(prop("wait", 4.0))
		if is_open and wait >= 0.0:
			get_tree().create_timer(wait, false).timeout.connect(func(): move(false)))


func input_open(_p, a: Node = null) -> void:
	move(true, a)


func input_close(_p, a: Node = null) -> void:
	move(false, a)


func input_toggle(_p, a: Node = null) -> void:
	move(not is_open, a)


func input_lock() -> void:
	locked = true


func input_unlock() -> void:
	locked = false


func input_set_speed(p) -> void:
	speed = IOParam.to_float(p, speed)
