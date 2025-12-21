class_name IOEntity
extends Node3D
## Base of every level entity with I/O. The map data sets targetname,
## classname, props (snake_case keys) and connections; IOMap builds these.
##
## Outputs: fire_output("OnTrigger", activator, param) runs the connections and
## emits, for Godot code,
##   - the generic `output` signal,
##   - a named signal when the class declares one (on_trigger),
##   - IO.output_fired.
## Inputs: receive_input("Open", ...) calls input_open(param, activator, caller)
## (any leading subset of those arguments). Kill, FireUser1-4 and AddOutput
## are handled here for every entity.
##
## Behaviour must match the editor's simulator (engine/io_sim.cpp).

signal output(name: StringName, activator: Node, param: Variant)
signal on_user1(activator: Node, param: Variant)
signal on_user2(activator: Node, param: Variant)
signal on_user3(activator: Node, param: Variant)
signal on_user4(activator: Node, param: Variant)
signal on_killed(activator: Node, param: Variant)

@export var targetname := ""
@export var classname := ""
## Property values from the editor, snake_case keys ("start_disabled").
@export var props: Dictionary = {}
## [output, target, input, param, delay, times]; times -1 = unlimited.
@export var connections: Array = []
## Brush entities: one convex point set per brush, relative to the origin, metres.
@export var hulls: Array = []
## The level scene's own node for this entity (its light, its brush mesh), when
## the export put one there; set by IOMap before _ready.
var adopted: Node = null

var enabled := true
var killed := false
var _fired := PackedInt32Array()
var _io: Node


func _enter_tree() -> void:
	_io = get_tree().root.get_node_or_null(^"IO")
	if _io == null:
		push_error("gHammer: the IO autoload is missing (enable the gHammer plugin)")
		return
	_io.register(self, targetname)
	if classname != "":
		add_to_group(StringName("io_class_" + classname))


func prop(key: String, fallback: Variant = null) -> Variant:
	return props.get(key, fallback)


func io() -> Node:
	return _io


func fire_output(name: String, activator: Node = null, param: Variant = null) -> void:
	if killed or _io == null:
		return
	var sig := IOParam.snake(name)
	if has_signal(sig):
		emit_signal(sig, activator, param)
	output.emit(StringName(name), activator, param)
	_io.output_fired.emit(self, StringName(name), activator, param)
	if _io.debug:
		print("IO: %s.%s(%s)" % [_label(), name, "" if param == null else str(param)])
	if _fired.size() != connections.size():
		_fired.resize(connections.size())
	for i in connections.size():
		var c: Array = connections[i]
		if String(c[0]).nocasecmp_to(name) != 0:
			continue
		var times := int(c[5])
		if times >= 0 and _fired[i] >= times:
			continue
		_fired[i] += 1
		var p: Variant = param if String(c[3]) == "" else c[3]
		_io.fire(String(c[1]), String(c[2]), p, float(c[4]), activator, self)


func receive_input(input: String, param: Variant, activator: Node, caller: Node) -> void:
	if killed or _io == null:
		return
	_io.input_received.emit(self, StringName(input), param, activator, caller)
	if _io.debug:
		print("IO:   -> %s.%s(%s)" % [_label(), input, "" if param == null else str(param)])
	var lower := input.to_lower()
	if lower == "kill":
		kill(activator)
		return
	if lower.begins_with("fireuser") and lower.length() == 9:
		fire_output("OnUser" + lower.substr(8), activator, param)
		return
	if lower == "addoutput":
		add_output(str(param))
		return
	var m := "input_" + IOParam.snake(input)
	if has_method(m):
		IOParam.call_flexible(self, m, [param, activator, caller])
	elif _io.debug:
		push_warning("IO: %s (%s) has no input %s" % [_label(), classname, input])


## Removes the entity; OnKilled fires first. Delayed events it sent still arrive.
func kill(activator: Node = null) -> void:
	if killed:
		return
	fire_output("OnKilled", activator)
	killed = true
	queue_free()


## AddOutput: "OnTrigger target:Input:param:delay:times".
func add_output(spec: String) -> bool:
	var sp := spec.strip_edges().find(" ")
	if sp <= 0:
		return false
	var out_name := spec.strip_edges().substr(0, sp)
	var f := spec.strip_edges().substr(sp + 1).split(":")
	if f.is_empty() or f[0].strip_edges() == "":
		return false
	var input := f[1].strip_edges() if f.size() > 1 and f[1].strip_edges() != "" else "Use"
	var times := int(f[4]) if f.size() > 4 and f[4].strip_edges() != "" else -1
	connections.append([out_name, f[0].strip_edges(), input, f[2] if f.size() > 2 else "",
			f[3].to_float() if f.size() > 3 else 0.0, -1 if times == 0 else times])
	_fired.append(0)
	return true


func _label() -> String:
	return targetname if targetname != "" else "%s#%s" % [classname, name]
