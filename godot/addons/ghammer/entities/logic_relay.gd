extends IOEntity
## logic_relay: one input fanned out to many outputs, or a switch that breaks
## a chain while disabled. It cannot fire again while its own delayed outputs
## are still on the way, unless fast_retrigger is on.

signal on_spawn(activator: Node, param: Variant)
signal on_trigger(activator: Node, param: Variant)
signal on_trigger_parameter(activator: Node, param: Variant)


func _ready() -> void:
	enabled = not prop("start_disabled", false)
	fire_output.call_deferred("OnSpawn")


func _can_fire(input: String) -> bool:
	if not enabled:
		return false
	if not prop("fast_retrigger", false) and io().pending_from(self) > 0:
		if io().debug:
			print("IO: %s still waiting for its delayed outputs; %s ignored" % [targetname, input])
		return false
	return true


func input_trigger(_param, activator: Node = null) -> void:
	if _can_fire("Trigger"):
		fire_output("OnTrigger", activator)
		if prop("only_once", false):
			kill(activator)


func input_trigger_with_parameter(param, activator: Node = null) -> void:
	if _can_fire("TriggerWithParameter"):
		fire_output("OnTriggerParameter", activator, param)
		if prop("only_once", false):
			kill(activator)


func input_enable() -> void:
	enabled = true


func input_disable() -> void:
	enabled = false


func input_toggle() -> void:
	enabled = not enabled


func input_cancel_pending() -> void:
	io().cancel_pending(self)
