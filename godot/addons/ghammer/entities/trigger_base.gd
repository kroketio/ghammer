extends IOEntity
## Shared trigger volume (trigger_multiple, trigger_once, trigger_look,
## trigger_proximity): an Area3D built from the brush hulls the editor
## exported. only_players filters on the "player" group.

signal on_start_touch(activator: Node, param: Variant)
signal on_end_touch(activator: Node, param: Variant)
signal on_trigger(activator: Node, param: Variant)
signal on_start_touch_all(activator: Node, param: Variant)
signal on_end_touch_all(activator: Node, param: Variant)
signal on_touching(activator: Node, param: Variant)
signal on_not_touching(activator: Node, param: Variant)

var area: Area3D
var inside: Array[Node3D] = []
var _next_trigger := 0.0


func _ready() -> void:
	enabled = not prop("start_disabled", false)
	area = Area3D.new()
	area.name = "Volume"
	add_child(area)
	for h in hulls:
		var cs := CollisionShape3D.new()
		var shape := ConvexPolygonShape3D.new()
		shape.points = PackedVector3Array(h)
		cs.shape = shape
		area.add_child(cs)
	area.body_entered.connect(_on_enter)
	area.body_exited.connect(_on_exit)
	area.monitoring = enabled


func passes(body: Node) -> bool:
	return not prop("only_players", true) or body.is_in_group(&"player")


func _on_enter(body: Node3D) -> void:
	if not enabled or killed or not passes(body):
		return
	inside.append(body)
	if inside.size() == 1:
		fire_output("OnStartTouchAll", body)
	fire_output("OnStartTouch", body)
	try_trigger(body)


func _on_exit(body: Node3D) -> void:
	if not inside.has(body):
		return
	inside.erase(body)
	fire_output("OnEndTouch", body)
	if inside.is_empty():
		fire_output("OnEndTouchAll", body)


func _physics_process(_delta: float) -> void:
	if enabled and not inside.is_empty():
		try_trigger(inside[0])


## OnTrigger, at most once per `wait` seconds (0 means 0.2, like Source;
## -1 fires once and removes the trigger).
func try_trigger(body: Node) -> void:
	var now := Time.get_ticks_msec() / 1000.0
	if now < _next_trigger:
		return
	var wait := float(prop("wait", 0.2))
	fire_output("OnTrigger", body)
	if wait < 0.0:
		kill(body)
	else:
		_next_trigger = now + (wait if wait > 0.0 else 0.2)


func input_enable() -> void:
	enabled = true
	if area:
		area.monitoring = true


func input_disable() -> void:
	enabled = false
	inside.clear()
	if area:
		area.monitoring = false


func input_toggle() -> void:
	if enabled:
		input_disable()
	else:
		input_enable()


func input_touch_test(_p, activator: Node = null) -> void:
	fire_output("OnTouching" if not inside.is_empty() else "OnNotTouching", activator)
