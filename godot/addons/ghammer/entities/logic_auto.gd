extends IOEntity
## logic_auto: fires when the map starts. OnMapSpawn waits for IOMap to finish
## building (plus one frame), so the player and every entity exist.

signal on_map_spawn(activator: Node, param: Variant)
signal on_new_game(activator: Node, param: Variant)
signal on_load_game(activator: Node, param: Variant)
signal on_map_transition(activator: Node, param: Variant)


func _ready() -> void:
	if io():
		io().map_spawned.connect(_on_map_spawned, CONNECT_ONE_SHOT)


func _on_map_spawned(_map: Node) -> void:
	await get_tree().process_frame
	var gs: String = prop("global_state", "")
	if gs != "" and not io().get_global(gs):
		return
	fire_output("OnMapSpawn")
	fire_output("OnNewGame")
	if prop("remove_on_fire", false):
		kill()
