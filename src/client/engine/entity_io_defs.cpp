// Entity I/O definitions: logic entities, trigger brush entities and the I/O of the existing classes.
// Source/Hammer names; the Godot export converts them with io::snake() (OnTrigger -> on_trigger,
// Trigger -> input_trigger). Every input's behaviour must match in io_sim.cpp and
// godot/addons/ghammer/entities/*.gd.

#include "entity.h"
#include "io.h"

namespace eng {

namespace {

using io::IODef;
using io::ParamType;
using P = ParamType;

IODef in(const char *name, const P t = P::Void, const char *desc = "", QStringList fires = {}) {
  return IODef{QString::fromLatin1(name), t, QString::fromUtf8(desc), std::move(fires)};
}
IODef out(const char *name, const P t = P::Void, const char *desc = "") {
  return IODef{QString::fromLatin1(name), t, QString::fromUtf8(desc), {}};
}

PropDef fp(const char *key, const char *label, double def, double lo, double hi, const char *tip = "") {
  return PropDef{QString::fromLatin1(key), QString::fromUtf8(label), QString::fromUtf8(tip), PropType::Float, def, lo, hi, {}};
}
PropDef ip(const char *key, const char *label, int def, double lo, double hi, const char *tip = "") {
  return PropDef{QString::fromLatin1(key), QString::fromUtf8(label), QString::fromUtf8(tip), PropType::Int, def, lo, hi, {}};
}
PropDef bp(const char *key, const char *label, bool def, const char *tip = "") {
  return PropDef{QString::fromLatin1(key), QString::fromUtf8(label), QString::fromUtf8(tip), PropType::Bool, def, 0, 0, {}};
}
PropDef sp(const char *key, const char *label, const char *def, PropType t = PropType::String, const char *tip = "") {
  return PropDef{QString::fromLatin1(key), QString::fromUtf8(label), QString::fromUtf8(tip), t, QString::fromUtf8(def), 0, 0, {}};
}

QString icon(const char *name) { return QStringLiteral(":/icons_godot/%1.svg").arg(QLatin1String(name)); }

EntityClass logicClass(const char *name, const char *label, const char *blenderIcon, const char *godotIcon,
                       const char *desc, const QColor color) {
  EntityClass c{name, label, "Logic", blenderIcon, desc, color, glm::vec3(1.0f), EntityClass::Kind::Generic, {}};
  c.billboard = icon(godotIcon);
  c.oriented = false;
  return c;
}

void appendInputs(EntityClass &c, std::initializer_list<IODef> l) {
  for (const auto &d: l)
    if (!c.input(d.name))
      c.inputs.push_back(d);
}
void appendOutputs(EntityClass &c, std::initializer_list<IODef> l) {
  for (const auto &d: l)
    if (!c.output(d.name))
      c.outputs.push_back(d);
}

// Enable / Disable / Toggle, shared by many classes.
void addEnableIO(EntityClass &c, const char *what = "the entity") {
  const QByteArray w(what);
  appendInputs(c, {in("Enable", P::Void, ("Enable " + w).constData()), in("Disable", P::Void, ("Disable " + w).constData()),
                   in("Toggle", P::Void, ("Toggle " + w + " between enabled and disabled").constData())});
}

// Everything a trigger volume has (Source's BaseTrigger).
void addTriggerIO(EntityClass &c) {
  c.billboard = icon("Area3D");
  c.props = {bp("startDisabled", "Start disabled", false),
             bp("onlyPlayers", "Only players", true, "Only the player can touch it (Godot group \"player\")"),
             fp("wait", "Re-trigger delay", 0.2, -1.0, 1000.0, "Seconds before OnTrigger can fire again; -1 = once")};
  addEnableIO(c, "the trigger");
  appendInputs(c, {in("TouchTest", P::Void, "Fire OnTouching if anything is inside, else OnNotTouching")});
  appendOutputs(c, {out("OnStartTouch", P::Void, "Something that passes the filter started touching"),
                    out("OnEndTouch", P::Void, "Something stopped touching"),
                    out("OnTrigger", P::Void, "Touched (repeats every re-trigger delay while touching)"),
                    out("OnStartTouchAll", P::Void, "The first thing started touching"),
                    out("OnEndTouchAll", P::Void, "The last thing stopped touching"),
                    out("OnTouching", P::Void, "Answer to TouchTest: something is inside"),
                    out("OnNotTouching", P::Void, "Answer to TouchTest: nothing is inside")});
}

} // namespace

void io::addCommonIO(EntityClass &c) {
  appendInputs(c, {in("Kill", P::Void, "Remove the entity from the world", {"OnKilled"}),
                   in("FireUser1", P::Variant, "Fire this entity's OnUser1", {"OnUser1"}),
                   in("FireUser2", P::Variant, "Fire this entity's OnUser2", {"OnUser2"}),
                   in("FireUser3", P::Variant, "Fire this entity's OnUser3", {"OnUser3"}),
                   in("FireUser4", P::Variant, "Fire this entity's OnUser4", {"OnUser4"}),
                   in("AddOutput", P::String, "Add a connection: \"OnX target:Input:param:delay:times\"")});
  appendOutputs(c, {out("OnUser1", P::Variant, "Fired by FireUser1"), out("OnUser2", P::Variant, "Fired by FireUser2"),
                    out("OnUser3", P::Variant, "Fired by FireUser3"), out("OnUser4", P::Variant, "Fired by FireUser4"),
                    out("OnKilled", P::Void, "The entity was removed with Kill")});
}

void EntityRegistry::addIOClasses() {
  using K = EntityClass::Kind;
  auto existing = [this](const char *name) -> EntityClass * {
    const auto it = m_index.constFind(QString::fromLatin1(name));
    return it == m_index.constEnd() ? nullptr : &m_classes[*it];
  };

  // Logic entities
  {
    EntityClass c = logicClass("logic_auto", "Auto", "play", "AutoPlay",
                               "Fires its outputs when the map starts. In Godot, OnMapSpawn waits one frame so the "
                               "player and the rest of the map exist.",
                               QColor(120, 230, 140));
    c.props = {sp("globalState", "Global state", "", PropType::String,
                  "Only fire when this global state is on (IO.set_global in Godot); empty = always"),
               bp("removeOnFire", "Remove on fire", false, "Delete the entity after firing (delayed outputs still arrive)")};
    appendOutputs(c, {out("OnMapSpawn", P::Void, "The map started (new game, load or transition)"),
                      out("OnNewGame", P::Void, "The map started as a new game"),
                      out("OnLoadGame", P::Void, "The map was loaded from a saved game"),
                      out("OnMapTransition", P::Void, "The map was entered through a level transition")});
    add(c);
  }
  {
    EntityClass c = logicClass("logic_relay", "Relay", "decorate_linked", "Signal",
                               "Fires OnTrigger when triggered: one input fanned out to many outputs, or a switch "
                               "that breaks a chain while disabled. It cannot fire again until its delayed outputs "
                               "have arrived, unless Fast retrigger is on.",
                               QColor(95, 211, 255));
    c.props = {bp("startDisabled", "Start disabled", false), bp("onlyOnce", "Only trigger once", false),
               bp("fastRetrigger", "Fast retrigger", false, "Allow triggering again before the delayed outputs arrive")};
    appendInputs(c, {in("Trigger", P::Void, "Fire OnTrigger (if enabled)", {"OnTrigger"}),
                     in("TriggerWithParameter", P::Variant, "Fire OnTriggerParameter with the parameter",
                        {"OnTriggerParameter"}),
                     in("CancelPending", P::Void, "Cancel this relay's outputs that are still waiting on a delay")});
    addEnableIO(c, "the relay");
    appendOutputs(c, {out("OnSpawn", P::Void, "The relay spawned"), out("OnTrigger", P::Void, "The relay was triggered"),
                      out("OnTriggerParameter", P::Variant, "Triggered with a parameter (passed on)")});
    add(c);
  }
  {
    EntityClass c = logicClass("logic_timer", "Timer", "time", "Timer",
                               "Fires OnTimer at a fixed or random interval; as an oscillator it alternates "
                               "OnTimerLow and OnTimerHigh.",
                               QColor(255, 210, 90));
    c.props = {fp("refireTime", "Refire interval", 1.0, 0.0, 3600.0, "Seconds between fires"),
               bp("useRandomTime", "Random interval", false),
               fp("lowerRandomBound", "Min random interval", 1.0, 0.0, 3600.0, "Seconds"),
               fp("upperRandomBound", "Max random interval", 5.0, 0.0, 3600.0, "Seconds"),
               bp("oscillator", "Oscillator", false, "Alternate OnTimerLow / OnTimerHigh (starts with Low)"),
               bp("startDisabled", "Start disabled", false)};
    appendInputs(c, {in("RefireTime", P::Float, "Set a new refire interval"),
                     in("ResetTimer", P::Void, "Restart the countdown"),
                     in("FireTimer", P::Void, "Fire now", {"OnTimer", "OnTimerHigh", "OnTimerLow"}),
                     in("LowerRandomBound", P::Float, "Set the minimum random interval"),
                     in("UpperRandomBound", P::Float, "Set the maximum random interval"),
                     in("AddToTimer", P::Float, "Add seconds to the running countdown"),
                     in("SubtractFromTimer", P::Float, "Take seconds off the running countdown"),
                     in("UseRandomTime", P::Bool, "Switch random intervals on or off")});
    addEnableIO(c, "the timer (enabling restarts the countdown)");
    appendOutputs(c, {out("OnTimer", P::Void, "The interval elapsed"),
                      out("OnTimerHigh", P::Void, "Every other fire (oscillator)"),
                      out("OnTimerLow", P::Void, "Every other fire, starting with this one (oscillator)")});
    add(c);
  }
  {
    EntityClass c = logicClass("logic_branch", "Branch", "split_vertical", "bool",
                               "Holds a boolean and fires OnTrue or OnFalse when tested.", QColor(160, 230, 120));
    c.props = {bp("initialValue", "Initial value", false)};
    appendInputs(c, {in("SetValue", P::Bool, "Set the value without testing"),
                     in("SetValueTest", P::Bool, "Set the value and test it", {"OnTrue", "OnFalse"}),
                     in("Toggle", P::Void, "Flip the value without testing"),
                     in("ToggleTest", P::Void, "Flip the value and test it", {"OnTrue", "OnFalse"}),
                     in("Test", P::Void, "Fire OnTrue or OnFalse for the current value", {"OnTrue", "OnFalse"})});
    appendOutputs(c, {out("OnTrue", P::Void, "Tested while true"), out("OnFalse", P::Void, "Tested while false")});
    add(c);
  }
  {
    EntityClass c = logicClass("logic_case", "Case", "sort_desc", "Sort",
                               "Compares its input with up to 16 values and fires the matching OnCase, or picks a "
                               "random case. Values compare as strings (\"1.50\" is not \"1.5\").",
                               QColor(230, 160, 255));
    for (int i = 1; i <= 16; ++i) {
      const QByteArray key = QStringLiteral("case%1").arg(i, 2, 10, QChar('0')).toLatin1();
      const QByteArray label = QStringLiteral("Case %1").arg(i, 2, 10, QChar('0')).toLatin1();
      c.props.push_back(sp(key.constData(), label.constData(), ""));
    }
    c.props.push_back(bp("multipleCasesAllowed", "Multiple matches", false, "Keep testing after the first match"));
    QStringList allCases;
    for (int i = 1; i <= 16; ++i)
      allCases << QStringLiteral("OnCase%1").arg(i, 2, 10, QChar('0'));
    appendInputs(c, {in("InValue", P::Variant, "Compare the parameter with the cases",
                        allCases + QStringList{"OnDefault", "OnUsed"}),
                     in("PickRandom", P::Void, "Fire a random OnCase that has a connection", allCases),
                     in("PickRandomShuffle", P::Void, "Like PickRandom, without repeats until all were picked", allCases)});
    for (int i = 1; i <= 16; ++i) {
      const QByteArray n = QStringLiteral("OnCase%1").arg(i, 2, 10, QChar('0')).toLatin1();
      c.outputs.push_back(out(n.constData(), P::Void, "The input matched this case"));
    }
    appendOutputs(c, {out("OnDefault", P::Variant, "The input matched no case"),
                      out("OnUsed", P::Variant, "Any InValue arrived (passes the value on)")});
    add(c);
  }
  {
    EntityClass c = logicClass("logic_compare", "Compare", "driver_distance", "float",
                               "Compares a value with a compare value and fires the matching outputs.",
                               QColor(255, 180, 120));
    c.props = {fp("initialValue", "Initial value", 0.0, -1e9, 1e9), fp("compareValue", "Compare value", 0.0, -1e9, 1e9)};
    const QStringList cmp{"OnLessThan", "OnEqualTo", "OnNotEqualTo", "OnGreaterThan"};
    appendInputs(c, {in("SetValue", P::Float, "Set the value without comparing"),
                     in("SetValueCompare", P::Float, "Set the value and compare", cmp),
                     in("SetCompareValue", P::Float, "Set the value to compare against"),
                     in("Compare", P::Void, "Compare now", cmp)});
    appendOutputs(c, {out("OnLessThan", P::Float, "Value < compare value (passes the value)"),
                      out("OnEqualTo", P::Float, "Value == compare value"),
                      out("OnNotEqualTo", P::Float, "Value != compare value"),
                      out("OnGreaterThan", P::Float, "Value > compare value")});
    add(c);
  }
  {
    EntityClass c = logicClass("logic_multicompare", "Multi-compare", "driver", "int",
                               "Collects values and fires OnEqual when they all match (or match the reference value).",
                               QColor(255, 160, 160));
    c.props = {ip("referenceValue", "Reference value", 0, -1e9, 1e9),
               bp("shouldCompareToValue", "Compare to reference", false,
                  "Every value must equal the reference value, not just each other")};
    appendInputs(c, {in("Value", P::Int, "Add a value"),
                     in("ValueCompare", P::Int, "Add a value and compare", {"OnEqual", "OnNotEqual"}),
                     in("CompareValues", P::Void, "Compare the collected values", {"OnEqual", "OnNotEqual"})});
    appendOutputs(c, {out("OnEqual", P::Void, "All values are equal"), out("OnNotEqual", P::Void, "Some value differs")});
    add(c);
  }
  {
    EntityClass c = logicClass("math_counter", "Counter", "sequence", "Range",
                               "A number with optional limits: add, subtract, multiply... Fires OutValue on every "
                               "change and OnHitMin / OnHitMax at the limits.",
                               QColor(255, 220, 140));
    c.props = {fp("startValue", "Initial value", 0.0, -1e9, 1e9), fp("min", "Minimum", 0.0, -1e9, 1e9),
               fp("max", "Maximum", 0.0, -1e9, 1e9, "Min = max = 0 means no limits"),
               bp("startDisabled", "Start disabled", false)};
    const QStringList change{"OutValue", "OnHitMin", "OnHitMax", "OnChangedFromMin", "OnChangedFromMax"};
    appendInputs(c, {in("Add", P::Float, "Add to the value", change), in("Subtract", P::Float, "Subtract from the value", change),
                     in("Multiply", P::Float, "Multiply the value", change), in("Divide", P::Float, "Divide the value", change),
                     in("SetValue", P::Float, "Set the value", change),
                     in("SetValueNoFire", P::Float, "Set the value without firing anything"),
                     in("GetValue", P::Void, "Fire OnGetValue with the value", {"OnGetValue"}),
                     in("SetHitMax", P::Float, "Set the maximum"), in("SetHitMin", P::Float, "Set the minimum")});
    addEnableIO(c, "the counter");
    appendOutputs(c, {out("OutValue", P::Float, "The value changed (passes the new value)"),
                      out("OnHitMin", P::Void, "The value reached the minimum"),
                      out("OnHitMax", P::Void, "The value reached the maximum"),
                      out("OnGetValue", P::Float, "Answer to GetValue"),
                      out("OnChangedFromMin", P::Void, "The value left the minimum"),
                      out("OnChangedFromMax", P::Void, "The value left the maximum")});
    add(c);
  }
  {
    EntityClass c = logicClass("logic_playerproxy", "Player Proxy", "user", "CharacterBody3D",
                               "Stands in for the player: its outputs are raised by the game's player code "
                               "(IO.player_event in Godot), its inputs are forwarded to the \"player\" group.",
                               QColor(80, 220, 120));
    appendInputs(c, {in("SetHealth", P::Int, "Forwarded to the player"), in("Freeze", P::Void, "Forwarded to the player"),
                     in("Unfreeze", P::Void, "Forwarded to the player"),
                     in("Teleport", P::Target, "Move the player to the named entity")});
    appendOutputs(c, {out("OnPlayerSpawned", P::Void, "The player spawned"),
                      out("OnPlayerKilled", P::Void, "The player died"),
                      out("OnPlayerEvent", P::String, "The game raised a named player event (passes the name)")});
    add(c);
  }
  {
    EntityClass c = logicClass("logic_event", "Godot Event", "export", "Signals",
                               "The bridge to Godot gameplay code. Emit makes the runtime emit IO.event(name, param, "
                               "activator, source); Godot code calling IO.raise(name, param) fires OnRaised on every "
                               "logic_event with that name.",
                               QColor(140, 200, 255));
    c.props = {sp("eventName", "Event name", "", PropType::String, "Shared with Godot code, e.g. \"boss_intro\""),
               bp("startDisabled", "Start disabled", false)};
    appendInputs(c, {in("Emit", P::Variant, "Emit the event to Godot code (with the parameter)")});
    addEnableIO(c, "the event");
    appendOutputs(c, {out("OnRaised", P::Variant, "Godot code raised this event (passes its parameter)")});
    add(c);
  }

  // Triggers (brush entities)
  if (EntityClass *c = existing("trigger_multiple")) {
    c->description = QStringLiteral("An invisible volume that fires outputs when something enters, stays in or leaves it.");
    addTriggerIO(*c);
  }
  auto triggerClass = [](const char *name, const char *label, const char *icon, const char *desc) {
    EntityClass c{name, label, "Brush Entities", icon, desc, QColor(255, 120, 160), glm::vec3(1.0f), K::Brush, {}};
    c.placeable = false;
    addTriggerIO(c);
    return c;
  };
  {
    EntityClass c = triggerClass("trigger_once", "Trigger Once", "physics",
                                 "A volume that fires OnTrigger the first time something touches it, then removes itself.");
    c.props.removeIf([](const PropDef &p) { return p.key == QLatin1String("wait"); });
    add(c);
  }
  {
    EntityClass c = triggerClass("trigger_look", "Trigger Look", "hide_off",
                                 "Fires OnTrigger when the player inside it looks at the look target for long enough.");
    c.props += {sp("lookTarget", "Look target", "", PropType::Target, "The entity the player must look at"),
                fp("lookTime", "Look time", 0.5, 0.0, 60.0, "Seconds of looking needed"),
                fp("fieldOfView", "Field of view", 0.9, -1.0, 1.0, "Dot product: 1 = dead ahead, 0 = within 90°"),
                fp("timeout", "Timeout", 0.0, 0.0, 600.0, "Seconds after entering before OnTimeout; 0 = never"),
                bp("useLOS", "Need line of sight", false), bp("fireOnce", "Fire once", true)};
    appendOutputs(c, {out("OnTimeout", P::Void, "The timeout ran out before the player looked")});
    add(c);
  }
  {
    EntityClass c = triggerClass("trigger_proximity", "Trigger Proximity", "light_hemi",
                                 "Reports how close the nearest touching thing is to a point: 0 at the point, 1 at "
                                 "the radius.");
    c.props += {sp("measureTarget", "Measure from", "", PropType::Target, "Entity whose origin is the centre"),
                fp("radius", "Radius", 256.0, 1.0, 100000.0, "Source units")};
    appendOutputs(c, {out("NearestEntityDistance", P::Float, "Distance of the nearest thing (0..1), every frame")});
    add(c);
  }

  // Existing classes
  for (const char *n: {"func_detail", "func_clip", "world_env", "sun", "prop_static", "info_player_start"})
    if (EntityClass *c = existing(n))
      c->io = false;

  for (const char *n: {"light", "light_spot"})
    if (EntityClass *c = existing(n)) {
      appendInputs(*c, {in("TurnOn", P::Void, "Switch the light on", {"OnTurnedOn"}),
                        in("TurnOff", P::Void, "Switch the light off", {"OnTurnedOff"}),
                        in("Toggle", P::Void, "Switch the light on or off", {"OnTurnedOn", "OnTurnedOff"}),
                        in("SetColor", P::Color, "Set the colour (#rrggbb or \"r g b\")"),
                        in("SetEnergy", P::Float, "Set the energy (Godot light_energy)")});
      appendOutputs(*c, {out("OnTurnedOn", P::Void, "The light was switched on"),
                         out("OnTurnedOff", P::Void, "The light was switched off")});
    }
  if (EntityClass *c = existing("ambient_sound")) {
    c->props.push_back(bp("startSilent", "Start silent", false, "Wait for PlaySound"));
    appendInputs(*c, {in("PlaySound", P::Void, "Start playing"), in("StopSound", P::Void, "Stop playing"),
                      in("ToggleSound", P::Void, "Start or stop"), in("Volume", P::Float, "Set the volume (0-4)")});
  }
  if (EntityClass *c = existing("func_door")) {
    appendInputs(*c, {in("Open", P::Void, "Open the door", {"OnOpen"}), in("Close", P::Void, "Close the door", {"OnClose"}),
                      in("Toggle", P::Void, "Open or close", {"OnOpen", "OnClose"}), in("Lock", P::Void, "Lock it"),
                      in("Unlock", P::Void, "Unlock it"), in("SetSpeed", P::Float, "Set the speed (Source units / s)")});
    appendOutputs(*c, {out("OnOpen", P::Void, "Started opening"), out("OnClose", P::Void, "Started closing"),
                       out("OnFullyOpen", P::Void, "Finished opening"), out("OnFullyClosed", P::Void, "Finished closing"),
                       out("OnLockedUse", P::Void, "Used while locked")});
  }
  if (EntityClass *c = existing("func_wall"))
    addEnableIO(*c, "the wall (shown and solid)");
  if (EntityClass *c = existing("func_illusionary"))
    addEnableIO(*c, "the brushes (shown)");
  if (EntityClass *c = existing("path_node"))
    appendOutputs(*c, {out("OnPass", P::Void, "Something following the path passed this node")});

  for (const char *n: {"info_target", "ambient_sound", "env_fog_volume", "world_env"})
    if (EntityClass *c = existing(n))
      c->oriented = false;
  for (auto &c: m_classes)
    if (c.isBrushEntity())
      c.oriented = false;

  // Every class with I/O gets the common set.
  for (auto &c: m_classes)
    if (c.io)
      io::addCommonIO(c);
}

} // namespace eng
