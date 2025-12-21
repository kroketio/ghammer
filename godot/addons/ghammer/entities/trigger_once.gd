extends "trigger_base.gd"
## trigger_once: fires OnTrigger the first time something touches it, then
## removes itself.


func try_trigger(body: Node) -> void:
	fire_output("OnTrigger", body)
	kill(body)
