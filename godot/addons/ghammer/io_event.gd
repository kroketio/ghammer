class_name IOEvent
extends RefCounted
## One queued input: target, input, parameter, who started the chain
## (activator) and who fired the output (caller). A delayed event lives in its
## own object with its own timer, so rapid re-fires never step on each other
## and the event still arrives when the sender was freed in the meantime.

var target: String
var input: String
var param: Variant
var activator: Node
var caller: Node
var cancelled := false
var due := 0.0


func _init(p_target: String, p_input: String, p_param: Variant, p_activator: Node, p_caller: Node) -> void:
	target = p_target
	input = p_input
	param = p_param
	activator = p_activator if is_instance_valid(p_activator) else null
	caller = p_caller if is_instance_valid(p_caller) else null


func describe() -> String:
	var from := "console"
	if is_instance_valid(caller):
		from = caller.targetname if "targetname" in caller and caller.targetname != "" else caller.name
	return "%s → %s.%s(%s)" % [from, target, input, "" if param == null else str(param)]
