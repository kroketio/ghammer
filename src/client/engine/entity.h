#pragma once

#include <QColor>
#include <QHash>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantMap>
#include <QVector>

#include <glm/glm.hpp>

#include "io_types.h"

namespace eng {

// Point entities: the class (EntityClass) declares typed properties with defaults; an entity stores only
// values that differ. PropType::Target is another entity's targetname (drawn as a dashed line).
enum class PropType { Float, Int, Bool, Color, String, Choice, File, Angle, Target };

struct PropDef {
  QString key;
  QString label;
  QString tooltip;
  PropType type = PropType::Float;
  QVariant defaultValue;
  double minValue = 0.0;
  double maxValue = 0.0;
  QStringList choices;
  QString suffix; // unit shown after the value ("m", "°")
};

// Units: 1 world unit = 10 Source units. A 128-unit wall is 2.6 m (1 Source unit = 2.03125 cm, 1 m ≈ 49.23
// units). Every metre conversion (Godot export, light ranges, scene.frag, the gHammer addon) goes through these.
// Lengths that follow Godot properties (light ranges) are stored in metres.
inline constexpr double kMetersPerSourceUnit = 2.6 / 128.0;  // 0.0203125
inline constexpr double kSourceUnitsPerMeter = 1.0 / kMetersPerSourceUnit;
inline constexpr double kMetersPerWorldUnit = kMetersPerSourceUnit * 10.0;  // 0.203125
inline constexpr double kWorldUnitsPerMeter = 1.0 / kMetersPerWorldUnit;

struct EntityClass {
  QString name;        // "light"
  QString label;       // "Point Light"
  QString category;    // "Lights"
  QString icon;        // blender icon name
  QString description;
  QColor color {95, 211, 255};
  glm::vec3 size {1.6f}; // marker box edge, world units
  enum class Kind { Generic, PointLight, SpotLight, Sun, Prop, PlayerStart, WorldEnv, Brush, Camera } kind = Kind::Generic;
  // Brush entities own brushes; they are not placed from the Add menu.
  [[nodiscard]] bool isBrushEntity() const { return kind == Kind::Brush; }
  QVector<PropDef> props;
  // Offered in the Add menu / entity browser (not world_env or the legacy sun).
  bool placeable = true;
  // Has a facing (Pitch / Yaw / Roll). Logic and brush entities do not.
  bool oriented = true;
  // Godot icon used as the 3D billboard (":/icons_godot/Timer.svg"); empty = the kind's default.
  QString billboard;

  // False for classes that are not addressable at runtime (func_detail is merged, prop_static baked):
  // no outputs, never a target.
  bool io = true;
  QVector<io::IODef> inputs;
  QVector<io::IODef> outputs;

  [[nodiscard]] const PropDef *prop(const QString &key) const;
  [[nodiscard]] const io::IODef *input(const QString &name) const;   // case-insensitive
  [[nodiscard]] const io::IODef *output(const QString &name) const;  // case-insensitive
};

class EntityRegistry {
public:
  static EntityRegistry &instance();
  [[nodiscard]] const EntityClass *find(const QString &name) const;
  [[nodiscard]] const QVector<EntityClass> &classes() const { return m_classes; }
  [[nodiscard]] QStringList categories() const;
  // Extra classes from a JSON array of {name,label,category,icon,color,size,billboard,
  // props:[{key,label,type,default,min,max,choices}],inputs:[{name,type,description}],outputs:[...]}.
  // Same-named classes replace built-in ones.
  int loadJson(const QString &path);

private:
  EntityRegistry();
  void add(EntityClass c);
  void addIOClasses(); // entity_io_defs.cpp
  QVector<EntityClass> m_classes;
  QHash<QString, int> m_index;
};

struct Entity {
  quint32 id = 0;
  QString classname;
  QString name;
  glm::vec3 origin {0.0f};
  glm::vec3 angles {0.0f}; // pitch, yaw, roll in degrees
  QVariantMap props;       // only values that differ from the class default
  bool hidden = false;
  io::ConnectionList outputs; // I/O connections fired by this entity

  [[nodiscard]] const EntityClass *cls() const { return EntityRegistry::instance().find(classname); }
  // The class default when not set.
  [[nodiscard]] QVariant value(const QString &key) const;
  [[nodiscard]] double num(const QString &key, double fallback = 0.0) const;
  [[nodiscard]] QColor color(const QString &key, const QColor &fallback = Qt::white) const;
  [[nodiscard]] glm::vec3 forward() const; // from angles (yaw around Y, pitch)
};

// Orientation as a rotation matrix (local axes as columns) and back. Props (Kind::Prop) use their model's
// GameObject convention: X, then Y, then Z (degrees). Others follow forward(): local +Z is the facing (yaw about +Y,
// pitch up), roll about it. To rotate: angles from R * entityRotation(e).
[[nodiscard]] glm::mat3 entityRotation(const Entity &e);
[[nodiscard]] glm::vec3 entityAnglesFromRotation(const Entity &e, const glm::mat3 &r);

// A light's omni_range / spot_range in metres and world units; 0 for non-lights.
[[nodiscard]] double lightRangeMeters(const Entity &e);
[[nodiscard]] inline float lightRangeWorld(const Entity &e) { return float(lightRangeMeters(e) * kWorldUnitsPerMeter); }

// Own id range: never collides with brush stable ids.
inline constexpr quint32 kEntityIdBase = 0x40000000u;

// Exactly one per map; holds the map's sun, environment, shadows, AO and LUT.
inline const QString kWorldEnvClass = QStringLiteral("world_env");

// Godot's Camera3D (frustum lines; the 3D view can preview through it).
inline const QString kCameraClass = QStringLiteral("camera_3d");

// Clip brushes: invisible player collision (the stairs' ramp), a brush entity like func_detail.
inline const QString kClipClass = QStringLiteral("func_clip");
inline const QString kRigidBodyClass = QStringLiteral("func_rigidbody");

} // namespace eng
