extends Node
## gHammer dispatcher: the "IO" autoload (added by the gHammer plugin).
##
## Entities address each other by targetname. Every IOEntity joins the group
## "io_<targetname>" (lowercase), so plain Godot code can find them too:
##     get_tree().get_nodes_in_group(&"io_door")
##
## Gameplay code hooks in without touching the map:
##     IO.output_fired.connect(func(e, output, activator, param): ...)
##     IO.event.connect(func(name, param, activator, source): ...)  # logic_event Emit
##     IO.fire("door", "Open")          # send an input, like ent_fire
##     IO.raise("generator_fixed")      # fires OnRaised on logic_event entities
##     IO.player_event("spawned")       # logic_playerproxy outputs
##
## Targets: a targetname, a wildcard (door_*), a classname, !self / !caller,
## !activator, !player (group "player"), or @group for any Godot node in that
## group (called through input_<snake>() or <snake>() if it has one).

signal output_fired(entity: Node, output: StringName, activator: Node, param: Variant)
signal input_received(entity: Node, input: StringName, param: Variant, activator: Node, caller: Node)
signal event(name: String, param: Variant, activator: Node, source: Node)
## Every map loaded through IOMap has spawned (logic_auto listens).
signal map_spawned(map: Node)

## Print every output and input (Source's "developer 2").
@export var debug := false
## A chain that delivers more than this many zero-delay inputs in one call is
## cut off: it is almost certainly a loop.
@export var max_chain := 2000

var _pending: Array[IOEvent] = []
var _globals := {}
var _chain := 0


func group_for(targetname: String) -> StringName:
	if targetname.begins_with("@"):
		return StringName(targetname.substr(1))
	return StringName("io_" + targetname.to_lower())


func register(e: Node, targetname: String) -> void:
	e.add_to_group(&"io_entities")
	if targetname != "":
		e.add_to_group(group_for(targetname))


## Everything `target` addresses, seen from `caller` with `activator`.
func find(target: String, caller: Node = null, activator: Node = null) -> Array[Node]:
	var out: Array[Node] = []
	var t := target.strip_edges()
	match t.to_lower():
		"":
			return out
		"!self", "!caller":
			if is_instance_valid(caller):
				out.append(caller)
			return out
		"!activator":
			if is_instance_valid(activator):
				out.append(activator)
			return out
		"!player":
			out.assign(get_tree().get_nodes_in_group(&"player"))
			return out
	if t.begins_with("@"):
		out.assign(get_tree().get_nodes_in_group(StringName(t.substr(1))))
		return out
	var wildcard := t.contains("*") or t.contains("?")
	if not wildcard:
		out.assign(get_tree().get_nodes_in_group(group_for(t)))
	for n in get_tree().get_nodes_in_group(&"io_entities"):
		if out.has(n):
			continue
		var tn: String = n.targetname if "targetname" in n else ""
		var cn: String = n.classname if "classname" in n else ""
		if wildcard and ((tn != "" and tn.matchn(t)) or cn.matchn(t)):
			out.append(n)
		elif not wildcard and cn.nocasecmp_to(t) == 0:
			out.append(n)
	return out


## Sends `input` to everything `target` names, now or after `delay` seconds
## (game time: pausing the tree pauses the delay).
func fire(target: String, input: String, param: Variant = null, delay := 0.0, activator: Node = null,
		caller: Node = null) -> void:
	var ev := IOEvent.new(target, input, param, activator, caller)
	if delay <= 0.0:
		_deliver(ev)
		return
	ev.due = Time.get_ticks_msec() / 1000.0 + delay
	_pending.append(ev)
	get_tree().create_timer(delay, false).timeout.connect(_on_due.bind(ev))


func _on_due(ev: IOEvent) -> void:
	_pending.erase(ev)
	if not ev.cancelled:
		_deliver(ev)


func _deliver(ev: IOEvent) -> void:
	_chain += 1
	if _chain > max_chain:
		if _chain == max_chain + 1:
			push_error("IO: more than %d inputs in one chain, stopping (zero-delay loop?): %s" % [max_chain, ev.describe()])
		_chain -= 1
		return
	var targets := find(ev.target, ev.caller, ev.activator)
	if targets.is_empty() and debug:
		print("IO: ", ev.describe(), ": no target")
	for n in targets:
		if not is_instance_valid(n):
			continue
		if n.has_method("receive_input"):
			n.receive_input(ev.input, ev.param, ev.activator, ev.caller)
		else:
			_call_plain_node(n, ev)
	_chain -= 1


## A Godot node outside the map (an @group target): input_<snake>() or
## <snake>() if it has one.
func _call_plain_node(n: Node, ev: IOEvent) -> void:
	input_received.emit(n, StringName(ev.input), ev.param, ev.activator, ev.caller)
	var s := IOParam.snake(ev.input)
	for m in ["input_" + s, s]:
		if n.has_method(m):
			IOParam.call_flexible(n, m, [ev.param, ev.activator, ev.caller])
			return
	if debug:
		print("IO: %s has no method for input %s" % [n.name, ev.input])


## logic_relay CancelPending: drops the delayed events `caller` sent.
func cancel_pending(caller: Node) -> int:
	var n := 0
	for ev in _pending:
		if ev.caller == caller and not ev.cancelled:
			ev.cancelled = true
			n += 1
	return n


func pending_from(caller: Node) -> int:
	var n := 0
	for ev in _pending:
		if ev.caller == caller and not ev.cancelled:
			n += 1
	return n


## Source's dumpeventqueue.
func dump_queue() -> PackedStringArray:
	var out := PackedStringArray()
	var now := Time.get_ticks_msec() / 1000.0
	for ev in _pending:
		if not ev.cancelled:
			out.append("in %.2fs  %s" % [ev.due - now, ev.describe()])
	return out


## Godot code -> map: fires OnRaised(param) on every enabled logic_event
## whose event name matches.
func raise(event_name: String, param: Variant = null, activator: Node = null) -> int:
	var n := 0
	for e in get_tree().get_nodes_in_group(&"io_class_logic_event"):
		if e.has_method("raise") and e.raise(event_name, param, activator):
			n += 1
	return n


## Player code -> map: "spawned", "killed" or any name (OnPlayerEvent).
func player_event(event_name: String, player: Node = null) -> void:
	for e in get_tree().get_nodes_in_group(&"io_class_logic_playerproxy"):
		e.player_event(event_name, player)


## logic_auto "Global state to read".
func set_global(name: String, on: bool) -> void:
	_globals[name] = on


func get_global(name: String) -> bool:
	return _globals.get(name, false)
