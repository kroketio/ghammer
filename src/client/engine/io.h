#pragma once

// Entity I/O: name resolution, validation and Godot naming. Pure functions over the entity list.

#include <QVariant>

#include <vector>

#include "entity.h"
#include "io_types.h"

namespace eng::io {

// "void", "bool", "int", "float", "string", "vector3", "color", "target", "variant"
QString paramTypeName(ParamType t);
ParamType paramTypeFromString(const QString &s);
// An empty string is valid for every type ("<none>"). Accepted forms:
//   Bool   1 0 true false yes no on off
//   Vec3   "x y z" or "x,y,z"
//   Color  "#rrggbb", "#rrggbbaa" or "r g b [a]" (0-255, like Source color255)
bool parseParam(ParamType t, const QString &s, QVariant *out = nullptr);

// CamelCase -> snake_case: OnTrigger -> on_trigger, FireUser1 -> fire_user1,
// startDisabled -> start_disabled, OnCase01 -> on_case01.
QString snake(const QString &s);
inline QString godotMethod(const QString &input) { return QStringLiteral("input_") + snake(input); }
// Group of every node with this targetname ("io_door"); `@hud` maps to the plain group "hud".
QString godotGroup(const QString &targetname);

enum class TargetKind {
  Resolved,   // one or more entities in the map
  Unresolved, // matches nothing: a broken connection
  Runtime,    // !activator / !caller / !player: only known while the game runs
  External,   // @group: a Godot-side node that is not part of the map
};

struct Resolution {
  TargetKind kind = TargetKind::Unresolved;
  std::vector<quint32> ids;
};

// "!self" resolves in the editor; the others at runtime.
bool isRuntimeTarget(const QString &target); // !activator !caller !player
bool isExternalTarget(const QString &target); // @name
bool isWildcard(const QString &target);
// Case-insensitive like Source; classes without I/O are never addressed.
bool addresses(const QString &target, const Entity &e);
Resolution resolve(const std::vector<Entity> &ents, const QString &target, quint32 self);

// Inputs accepted by at least one entity `target` resolves to, in class order, unique. Runtime / external
// targets: every input of every I/O class.
std::vector<IODef> inputsForTarget(const std::vector<Entity> &ents, const QString &target, quint32 self);

struct IncomingRef {
  quint32 source = 0;
  int connection = -1; // index into the source's outputs
};
// Connections in the map that resolve to entity `id`.
std::vector<IncomingRef> incoming(const std::vector<Entity> &ents, quint32 id);
// Connections and Target props whose target is exactly `name` (no wildcards).
int countNameReferences(const std::vector<Entity> &ents, const QString &name);
// Rewrites exact references (connection targets, Target props). True when something changed.
bool renameReferences(Entity &e, const QString &from, const QString &to);

// "OnTrigger target:Input:param:delay:times" (Source AddOutput syntax); connection form only, no keyvalue edits.
bool parseAddOutput(const QString &s, Connection *out);
QString formatAddOutput(const Connection &c);

// The inputs / outputs every I/O entity has (Kill, FireUser1-4, AddOutput; OnUser1-4, OnKilled).
void addCommonIO(EntityClass &c);

struct Issue {
  enum class Severity { Error, Warning, Info } severity = Severity::Warning;
  quint32 entity = 0;
  int connection = -1; // -1 = the entity itself (e.g. a Target prop)
  QString text;
};

// Unknown output / input, unresolved target, bad parameter, negative delay, times == 0, zero-delay loops.
std::vector<Issue> validate(const std::vector<Entity> &ents);
std::vector<Issue> validateConnection(const std::vector<Entity> &ents, const Entity &owner, int index);

} // namespace eng::io
