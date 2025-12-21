extends IOEntity
## func_rigidbody: a physics object made of brushes. A RigidBody3D with one
## ConvexPolygonShape3D per collision hull the editor exported (the same few
## simplified hulls its Box3D world uses, relative to this node) and the
## entity's mass; its mesh moves with the body.

var body: RigidBody3D


func _ready() -> void:
	body = RigidBody3D.new()
	body.name = "Body"
	body.mass = maxf(0.01, float(prop("mass", 10.0)))
	add_child(body)
	for h in hulls:
		var cs := CollisionShape3D.new()
		var shape := ConvexPolygonShape3D.new()
		shape.points = PackedVector3Array(h)
		cs.shape = shape
		body.add_child(cs)
	# The level's mesh instance is reparented under this node after _ready
	# (io_map.gd); it goes one level further, under the body.
	_adopt.call_deferred()


func _adopt() -> void:
	if adopted is Node3D and is_instance_valid(adopted):
		adopted.reparent(body, true)
