# gHammer (Godot runtime)

Runs the entity I/O (Source-style inputs and outputs) of levels exported from gHammer.

## Setup

1. File › Export to Godot writes each level with its I/O data (`levels/<map>/<map>_io.gd`, wired to an IOMap node in
   the level scene) and copies this addon to `res://addons/ghammer` (Project Settings › Godot Export).
2. Enable **gHammer** in Project Settings › Plugins (adds the `IO` autoload).
3. Put the player in the group `player` (triggers filter on it; `!player` targets it).

## From gameplay code

```gdscript
IO.output_fired.connect(func(entity, output, activator, param): pass)  # every output
IO.event.connect(func(name, param, activator, source): pass)           # logic_event "Emit"
IO.fire("door_*", "Unlock")      # like ent_fire
IO.raise("generator_fixed")      # OnRaised on logic_event entities of that name
IO.player_event("spawned")       # logic_playerproxy OnPlayerSpawned
```

An output targeting `@group` calls `input_<snake>(param)` or `<snake>(param)` on every node in that group.

| Editor | Godot |
|---|---|
| targetname `door` | group `io_door` |
| output `OnFullyOpen` | signal `on_fully_open(activator, param)` |
| input `SetValueTest` | `input_set_value_test(param, activator, caller)` (any leading subset) |
| property `startDisabled` | `props["start_disabled"]` |

## Own entity classes

Declare the class in an entity JSON file in the editor and add `res://ghammer_entities/<classname>.gd` extending
`IOEntity`; it wins over the addon's. Without a script a class still has Kill, FireUser1-4 and AddOutput.

## Debugging

`IO.debug = true` logs every output and input; `IO.dump_queue()` lists delayed events; a zero-delay chain longer
than `IO.max_chain` is stopped with an error. Built-in classes behave as the editor's simulator (Entity › Run I/O
Simulation, `ent_fire`).
