@tool
class_name IOMap
extends Node3D
## Builds a map's entities from the data script the level editor exports
## (File › Export I/O to Godot: res://maps/<map>_io.gd). Drop one IOMap in a
## scene, point map_data at that script, and the logic runs.
##
## Each entity becomes a node with the script entities/<classname>.gd from this
## addon, or res://ghammer_entities/<classname>.gd when the project provides
## one (that is how a game adds or overrides entity classes), or plain IOEntity
## (common inputs only) when neither exists.

## The exported <map>_io.gd.
@export var map_data: Script:
	set(v):
		map_data = v
		update_configuration_warnings()
## Build when the scene starts.
@export var build_on_ready := true
## Folder with the game's own entity scripts.
@export_dir var project_entities_dir := "res://ghammer_entities"

const ADDON_ENTITIES := "res://addons/ghammer/entities"


func _get_configuration_warnings() -> PackedStringArray:
	return PackedStringArray() if map_data else PackedStringArray(["Set map_data to the exported <map>_io.gd"])


func _ready() -> void:
	if Engine.is_editor_hint():
		return
	if build_on_ready:
		build()


func build() -> void:
	if map_data == null:
		push_error("IOMap: no map_data")
		return
	var consts := map_data.get_script_constant_map()
	var entities: Array = consts.get("ENTITIES", [])
	for rec in entities:
		var n := _make(rec)
		# Set before add_child: the entity's _ready looks at it.
		n.adopted = _existing_node(rec)
		add_child(n, true)
		# Brush geometry moves with its entity (doors): it becomes a child.
		if n.adopted is Node3D and not (n.adopted is Light3D):
			n.adopted.reparent(n, true)
	var io := get_tree().root.get_node_or_null(^"IO")
	if io:
		# After everything is in the tree; logic_auto waits for this.
		io.map_spawned.emit.call_deferred(self)


func _script_for(classname: String) -> Script:
	for dir in [project_entities_dir, ADDON_ENTITIES]:
		var path := "%s/%s.gd" % [dir, classname]
		if ResourceLoader.exists(path):
			return load(path)
	return IOEntity


# Lights and brush entity meshes already exist as nodes of the level scene
# (File › Export to Godot); "node" is their path below the level root.
func _existing_node(rec: Dictionary) -> Node:
	var path: String = rec.get("node", "")
	if path == "" or get_parent() == null:
		return null
	return get_parent().get_node_or_null(NodePath(path))


func _make(rec: Dictionary) -> Node3D:
	var n: Node3D = _script_for(rec.get("class", "")).new()
	n.classname = rec.get("class", "")
	n.targetname = rec.get("targetname", "")
	n.props = rec.get("props", {})
	n.connections = rec.get("outputs", [])
	n.hulls = rec.get("hulls", [])
	n.name = (n.targetname if n.targetname != "" else n.classname).validate_node_name()
	n.position = rec.get("origin", Vector3.ZERO)
	var a: Vector3 = rec.get("angles", Vector3.ZERO)
	# Editor angles are pitch / yaw / roll in degrees, Y up, yaw 0 facing +Z.
	n.rotation = Vector3(deg_to_rad(-a.x), deg_to_rad(a.y), deg_to_rad(a.z))
	return n
