extends "trigger_base.gd"
## trigger_proximity: every physics frame while something is inside, fires
## NearestEntityDistance with the nearest one's distance to measure_target
## (or this trigger), mapped to 0 at the point and 1 at `radius`.

signal nearest_entity_distance(activator: Node, param: Variant)

const SOURCE_UNIT := 0.0203125 # metres (128 Source units = 2.6 m)


func _physics_process(delta: float) -> void:
	super._physics_process(delta)
	if not enabled or inside.is_empty():
		return
	var centre := global_position
	var targets: Array[Node] = []
	if io():
		targets = io().find(str(prop("measure_target", "")), self)
	if not targets.is_empty() and targets[0] is Node3D:
		centre = (targets[0] as Node3D).global_position
	var radius := maxf(0.001, float(prop("radius", 256.0)) * SOURCE_UNIT)
	var best: Node3D = null
	var best_d := INF
	for b in inside:
		var d := b.global_position.distance_to(centre)
		if d < best_d:
			best_d = d
			best = b
	if best and best_d <= radius:
		fire_output("NearestEntityDistance", best, clampf(best_d / radius, 0.0, 1.0))
