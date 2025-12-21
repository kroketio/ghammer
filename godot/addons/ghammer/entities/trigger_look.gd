extends "trigger_base.gd"
## trigger_look: OnTrigger when the player inside looks at look_target for
## look_time seconds (within field_of_view, a dot product: 1 = dead ahead).
## OnTimeout when `timeout` seconds pass after entering without that.

signal on_timeout(activator: Node, param: Variant)

var _looked := 0.0
var _entered_at := -1.0
var _done := false


func try_trigger(_body: Node) -> void:
	pass # driven from _physics_process below


func _on_enter(body: Node3D) -> void:
	super._on_enter(body)
	if inside.size() == 1:
		_entered_at = Time.get_ticks_msec() / 1000.0
		_looked = 0.0


func _physics_process(delta: float) -> void:
	if not enabled or _done or inside.is_empty():
		return
	var body := inside[0]
	var cam := get_viewport().get_camera_3d()
	var targets: Array[Node] = []
	if io():
		targets = io().find(str(prop("look_target", "")), self, body)
	if cam == null or targets.is_empty() or not targets[0] is Node3D:
		return
	var to := ((targets[0] as Node3D).global_position - cam.global_position).normalized()
	var facing := -cam.global_transform.basis.z
	var looking := facing.dot(to) >= float(prop("field_of_view", 0.9))
	if looking and prop("use_los", false):
		var q := PhysicsRayQueryParameters3D.create(cam.global_position, (targets[0] as Node3D).global_position)
		q.exclude = [body.get_rid()] if body is CollisionObject3D else []
		var hit := cam.get_world_3d().direct_space_state.intersect_ray(q)
		looking = hit.is_empty() or hit.collider == targets[0]
	_looked = _looked + delta if looking else 0.0
	if _looked >= float(prop("look_time", 0.5)):
		fire_output("OnTrigger", body)
		_looked = 0.0
		if prop("fire_once", true):
			_done = true
		return
	var timeout := float(prop("timeout", 0.0))
	if timeout > 0.0 and _entered_at >= 0.0 and Time.get_ticks_msec() / 1000.0 - _entered_at > timeout:
		fire_output("OnTimeout", body)
		_entered_at = -1.0
