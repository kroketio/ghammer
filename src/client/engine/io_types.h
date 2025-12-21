#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

namespace eng::io {

// Source-style entity I/O: a connection says "when my <output> fires, send <input> to the entities named
// <target>, with <param>, after <delay> seconds, at most <times> times". Plain data (see engine.md "Entity I/O").

// Names follow Godot's Variant types so the export can use them as-is.
enum class ParamType { Void, Bool, Int, Float, String, Vec3, Color, Target, Variant };

struct IODef {
  QString name;
  ParamType param = ParamType::Void;
  QString description;
  // Inputs only: outputs of the same entity this input fires at once (logic_relay Trigger -> OnTrigger).
  // Used to find zero-delay loops; empty = ends the chain.
  QStringList fires;
};

struct Connection {
  QString output; // on the entity that owns the connection ("OnTrigger")
  QString target; // targetname / wildcard / classname / !self / @group
  QString input;  // on the target ("Open")
  QString param;  // empty = <none>: pass the output's own value, if any
  float delay = 0.0f;
  int times = -1;        // -1 = unlimited, 1 = once, N
  bool disabled = false; // kept in the map, never fires

  bool operator==(const Connection &o) const = default;
};

using ConnectionList = QVector<Connection>;

} // namespace eng::io
