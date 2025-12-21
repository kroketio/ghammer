extends IOEntity
## logic_event: the bridge to gameplay code.
##   Emit (map -> Godot): IO.event(event_name, param, activator, self) fires,
##     and this node's `emitted` signal.
##   OnRaised (Godot -> map): IO.raise(event_name, param) fires it.

signal emitted(event_name: String, param: Variant, activator: Node)
signal on_raised(activator: Node, param: Variant)


func _ready() -> void:
	enabled = not prop("start_disabled", false)


func event_name() -> String:
	return str(prop("event_name", ""))


func input_emit(p, activator: Node = null) -> void:
	if not enabled:
		return
	emitted.emit(event_name(), p, activator)
	io().event.emit(event_name(), p, activator, self)


func raise(name: String, param: Variant, activator: Node) -> bool:
	if not enabled or killed or name.nocasecmp_to(event_name()) != 0:
		return false
	fire_output("OnRaised", activator, param)
	return true


func input_enable() -> void:
	enabled = true


func input_disable() -> void:
	enabled = false


func input_toggle() -> void:
	enabled = not enabled
