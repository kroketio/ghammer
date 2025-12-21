extends IOEntity
## ambient_sound: an AudioStreamPlayer3D. `sound` should be a res:// path in
## the Godot project (an absolute editor path is tried by file name under
## res://sounds/ as a fallback).

const SOURCE_UNIT := 0.0203125 # metres (128 Source units = 2.6 m)

var player: AudioStreamPlayer3D


func _ready() -> void:
	player = AudioStreamPlayer3D.new()
	add_child(player)
	var path := str(prop("sound", ""))
	if path != "" and not path.begins_with("res://"):
		path = "res://sounds/" + path.get_file()
	if path != "" and ResourceLoader.exists(path):
		player.stream = load(path)
	player.max_distance = float(prop("radius", 512.0)) * SOURCE_UNIT
	player.volume_db = linear_to_db(maxf(0.0001, float(prop("volume", 1.0))))
	if player.stream and prop("loop", true) and "loop" in player.stream:
		player.stream.loop = true
	if not prop("start_silent", false):
		player.play.call_deferred()


func input_play_sound() -> void:
	player.play()


func input_stop_sound() -> void:
	player.stop()


func input_toggle_sound() -> void:
	if player.playing:
		player.stop()
	else:
		player.play()


func input_volume(p) -> void:
	player.volume_db = linear_to_db(maxf(0.0001, IOParam.to_float(p, 1.0)))
