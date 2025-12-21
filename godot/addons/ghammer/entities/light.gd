extends IOEntity
## light / light_spot: an OmniLight3D or SpotLight3D child built from the
## editor's properties; TurnOn / TurnOff / Toggle switch it.

signal on_turned_on(activator: Node, param: Variant)
signal on_turned_off(activator: Node, param: Variant)

var lamp: Light3D


# The editor stores these under Godot's own property names (ranges in metres).
func _ready() -> void:
	if adopted is Light3D:
		# The level scene already has this light (and its settings).
		lamp = adopted
		return
	if classname == "light_spot":
		var s := SpotLight3D.new()
		s.spot_range = float(prop("spot_range", 5.0))
		s.spot_angle = float(prop("spot_angle", 45.0))
		lamp = s
	else:
		var o := OmniLight3D.new()
		o.omni_range = float(prop("omni_range", 6.0))
		lamp = o
	lamp.light_color = IOParam.to_color(prop("light_color", "#ffffff"))
	lamp.light_energy = float(prop("light_energy", 2.0 if classname != "light_spot" else 1.0))
	lamp.light_specular = float(prop("light_specular", 1.0))
	lamp.shadow_enabled = prop("cast_shadows", true)
	lamp.visible = prop("enabled", true)
	add_child(lamp)


func _set_on(on: bool, activator: Node) -> void:
	if lamp.visible == on:
		return
	lamp.visible = on
	fire_output("OnTurnedOn" if on else "OnTurnedOff", activator)


func input_turn_on(_p, a: Node = null) -> void:
	_set_on(true, a)


func input_turn_off(_p, a: Node = null) -> void:
	_set_on(false, a)


func input_toggle(_p, a: Node = null) -> void:
	_set_on(not lamp.visible, a)


func input_set_color(p) -> void:
	lamp.light_color = IOParam.to_color(p, lamp.light_color)


func input_set_energy(p) -> void:
	lamp.light_energy = IOParam.to_float(p, lamp.light_energy)
