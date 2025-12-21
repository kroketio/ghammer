extends IOEntity
## func_wall: Enable / Disable / Toggle show or hide (and enable or disable the
## collision of) everything under this node.


func _ready() -> void:
	enabled = not prop("start_disabled", false)
	_apply.call_deferred()


func _apply() -> void:
	visible = enabled
	for n in find_children("*", "CollisionShape3D", true, false):
		(n as CollisionShape3D).set_deferred("disabled", not enabled)


func input_enable() -> void:
	enabled = true
	_apply()


func input_disable() -> void:
	enabled = false
	_apply()


func input_toggle() -> void:
	enabled = not enabled
	_apply()
