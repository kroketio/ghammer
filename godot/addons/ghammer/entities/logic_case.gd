extends IOEntity
## logic_case: compares InValue with case01..case16 (as strings, so "1.50" is
## not "1.5") and fires the matching OnCaseNN, else OnDefault; PickRandom fires
## a random connected case, PickRandomShuffle without repeats.

signal on_default(activator: Node, param: Variant)
signal on_used(activator: Node, param: Variant)

var _bag: Array[int] = []


func _case_output(i: int) -> String:
	return "OnCase%02d" % i


func input_in_value(p, activator: Node = null) -> void:
	var v := "" if p == null else str(p)
	var multi: bool = prop("multiple_cases_allowed", false)
	var hit := false
	for i in range(1, 17):
		var c: String = str(prop("case%02d" % i, ""))
		if c != "" and c == v:
			fire_output(_case_output(i), activator)
			hit = true
			if not multi:
				break
	if not hit:
		fire_output("OnDefault", activator, p)
	fire_output("OnUsed", activator, p)


func _wired() -> Array[int]:
	var out: Array[int] = []
	for i in range(1, 17):
		for c in connections:
			if String(c[0]).nocasecmp_to(_case_output(i)) == 0:
				out.append(i)
				break
	return out


func input_pick_random(_p, activator: Node = null) -> void:
	var w := _wired()
	if not w.is_empty():
		fire_output(_case_output(w.pick_random()), activator)


func input_pick_random_shuffle(_p, activator: Node = null) -> void:
	var w := _wired()
	if w.is_empty():
		return
	_bag.assign(_bag.filter(func(i): return w.has(i)))
	if _bag.is_empty():
		_bag = w.duplicate()
		_bag.shuffle()
	fire_output(_case_output(_bag.pop_back()), activator)
