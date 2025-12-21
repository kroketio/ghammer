extends IOEntity
## logic_playerproxy: stands in for the player. The game's player code calls
## IO.player_event("spawned" / "killed" / anything); the inputs are forwarded
## to every node in the "player" group (input_<snake>() or <snake>()).

signal on_player_spawned(activator: Node, param: Variant)
signal on_player_killed(activator: Node, param: Variant)
signal on_player_event(activator: Node, param: Variant)


func player_event(event_name: String, player: Node) -> void:
	match event_name.to_lower():
		"spawned":
			fire_output("OnPlayerSpawned", player)
		"killed":
			fire_output("OnPlayerKilled", player)
	fire_output("OnPlayerEvent", player, event_name)


func receive_input(input: String, param: Variant, activator: Node, caller: Node) -> void:
	var common := ["kill", "fireuser1", "fireuser2", "fireuser3", "fireuser4", "addoutput"]
	if input.to_lower() in common:
		super.receive_input(input, param, activator, caller)
		return
	if input.to_lower() == "teleport" and io():
		var dest: Array = io().find(str(param), self, activator)
		for p in get_tree().get_nodes_in_group(&"player"):
			if not dest.is_empty() and p is Node3D and dest[0] is Node3D:
				(p as Node3D).global_position = (dest[0] as Node3D).global_position
		return
	io().fire("!player", input, param, 0.0, activator, self)
