class_name IOParam
extends RefCounted
## Parameter helpers for level I/O. Connection parameters arrive as strings
## (typed in the editor) or as values passed on by an output (math_counter's
## OutValue). These convert either to what an input wants. They mirror
## eng::io::parseParam in the editor.


static func to_float(p: Variant, fallback := 0.0) -> float:
	if p == null:
		return fallback
	if p is float or p is int:
		return float(p)
	var s := str(p).strip_edges()
	return s.to_float() if s.is_valid_float() or s.is_valid_int() else fallback


static func to_int(p: Variant, fallback := 0) -> int:
	if p == null:
		return fallback
	if p is int:
		return p
	if p is float:
		return roundi(p)
	var s := str(p).strip_edges()
	if s.is_valid_int():
		return s.to_int()
	if s.is_valid_float():
		return roundi(s.to_float())
	return fallback


static func to_bool(p: Variant, fallback := false) -> bool:
	if p == null:
		return fallback
	if p is bool:
		return p
	if p is int or p is float:
		return p != 0
	match str(p).strip_edges().to_lower():
		"1", "true", "yes", "on":
			return true
		"0", "false", "no", "off":
			return false
	return fallback


## "x y z" or "x,y,z" (editor units are converted by the caller if needed).
static func to_vector3(p: Variant, fallback := Vector3.ZERO) -> Vector3:
	if p is Vector3:
		return p
	if p == null:
		return fallback
	var parts := str(p).replace(",", " ").split(" ", false)
	if parts.size() != 3:
		return fallback
	return Vector3(parts[0].to_float(), parts[1].to_float(), parts[2].to_float())


## "#rrggbb", "#rrggbbaa" or "r g b [a]" with 0-255 components (Source color255).
static func to_color(p: Variant, fallback := Color.WHITE) -> Color:
	if p is Color:
		return p
	if p == null:
		return fallback
	var s := str(p).strip_edges()
	if s.begins_with("#"):
		return Color.html(s) if Color.html_is_valid(s) else fallback
	var parts := s.replace(",", " ").split(" ", false)
	if parts.size() < 3:
		return fallback
	var a := parts[3].to_float() if parts.size() > 3 else 255.0
	return Color8(parts[0].to_int(), parts[1].to_int(), parts[2].to_int(), int(a))


## OnTrigger -> on_trigger, FireUser1 -> fire_user1, OnCase01 -> on_case01.
## Same rule as eng::io::snake in the editor.
static func snake(s: String) -> String:
	var out := ""
	for i in s.length():
		var c := s[i]
		var upper := c != c.to_lower()
		if upper:
			var prev_lower_or_digit := i > 0 and (s[i - 1] != s[i - 1].to_upper() or s[i - 1].is_valid_int())
			var acronym_end := i > 0 and s[i - 1] != s[i - 1].to_lower() and i + 1 < s.length() \
					and s[i + 1] != s[i + 1].to_upper()
			if (prev_lower_or_digit or acronym_end) and not out.ends_with("_"):
				out += "_"
			out += c.to_lower()
		elif c == " " or c == "-" or c == ".":
			if not out.ends_with("_"):
				out += "_"
		else:
			out += c
	return out


## Calls `method` with as many of `args` as it declares, so an input handler
## may take (), (param), (param, activator) or (param, activator, caller).
static func call_flexible(obj: Object, method: String, args: Array) -> Variant:
	var n := args.size()
	for m in obj.get_method_list():
		if m.name == method:
			n = mini(n, (m.args as Array).size())
			break
	return obj.callv(method, args.slice(0, n))
