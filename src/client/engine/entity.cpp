#include "entity.h"
#include "io.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

#include <algorithm>
#include <cmath>

#ifndef GLM_ENABLE_EXPERIMENTAL
#define GLM_ENABLE_EXPERIMENTAL
#endif
#include <glm/gtx/euler_angles.hpp>

namespace eng {

const PropDef *EntityClass::prop(const QString &key) const {
  for (const auto &p: props)
    if (p.key == key)
      return &p;
  return nullptr;
}

const io::IODef *EntityClass::input(const QString &name) const {
  for (const auto &d: inputs)
    if (d.name.compare(name, Qt::CaseInsensitive) == 0)
      return &d;
  return nullptr;
}

const io::IODef *EntityClass::output(const QString &name) const {
  for (const auto &d: outputs)
    if (d.name.compare(name, Qt::CaseInsensitive) == 0)
      return &d;
  return nullptr;
}

namespace {

PropDef fp(const char *key, const char *label, double def, double lo, double hi, const char *tip = "") {
  return PropDef{QString::fromLatin1(key), QString::fromUtf8(label), QString::fromUtf8(tip), PropType::Float, def, lo, hi, {}};
}
PropDef fu(const char *key, const char *label, double def, double lo, double hi, const char *suffix,
           const char *tip = "", PropType t = PropType::Float) {
  return PropDef{QString::fromLatin1(key), QString::fromUtf8(label), QString::fromUtf8(tip), t, def, lo, hi, {},
                 QString::fromUtf8(suffix)};
}
PropDef bp(const char *key, const char *label, bool def, const char *tip = "") {
  return PropDef{QString::fromLatin1(key), QString::fromUtf8(label), QString::fromUtf8(tip), PropType::Bool, def, 0, 0, {}};
}
PropDef cp(const char *key, const char *label, const char *def) {
  return PropDef{QString::fromLatin1(key), QString::fromUtf8(label), {}, PropType::Color, QString::fromLatin1(def), 0, 0, {}};
}
PropDef sp(const char *key, const char *label, const char *def, PropType t = PropType::String, const char *tip = "") {
  return PropDef{QString::fromLatin1(key), QString::fromUtf8(label), QString::fromUtf8(tip), t, QString::fromUtf8(def), 0, 0, {}};
}

PropType typeFromString(const QString &s) {
  static const QHash<QString, PropType> map = {
      {"float", PropType::Float}, {"int", PropType::Int},       {"bool", PropType::Bool},
      {"color", PropType::Color}, {"string", PropType::String}, {"choice", PropType::Choice},
      {"file", PropType::File},   {"angle", PropType::Angle},   {"target", PropType::Target}};
  return map.value(s.toLower(), PropType::String);
}

} // namespace

EntityRegistry &EntityRegistry::instance() {
  static EntityRegistry r;
  return r;
}

void EntityRegistry::add(EntityClass c) {
  if (auto it = m_index.constFind(c.name); it != m_index.constEnd()) {
    m_classes[*it] = std::move(c);
    return;
  }
  m_index.insert(c.name, int(m_classes.size()));
  m_classes.push_back(std::move(c));
}

EntityRegistry::EntityRegistry() {
  using K = EntityClass::Kind;
  {
    EntityClass c{"light", "Omni Light", "Lights", "light_point",
                  "Omnidirectional light. Shadows come from the light atlas (Preferences › Rendering › Shadows).",
                  QColor(255, 216, 77), glm::vec3(1.2f), K::PointLight, {}};
    // Godot OmniLight3D: same names, ranges and defaults; omni_attenuation is Godot's default 1.
    c.props = {bp("enabled", "Enabled", true), cp("light_color", "Colour", "#ffffff"),
               fp("light_energy", "Energy", 2.0, 0.0, 16.0, "Godot light_energy"),
               fp("light_specular", "Specular", 1.0, 0.0, 16.0, "Godot light_specular: strength of the highlight"),
               fu("omni_range", "Range", 6.0, 0.0, 4096.0, "m", "Godot omni_range, metres (1 m ≈ 49.2 Source units; a 128-unit wall is 2.6 m)"),
               bp("castShadows", "Cast shadows", true),
               fu("shadow_bias", "Shadow bias", 0.1, 0.0, 10.0, "m",
                  "Godot shadow_bias: how far behind a surface its own shadow starts (more: less acne, shadows detach)"),
               fp("shadow_normal_bias", "Normal bias", 1.0, 0.0, 10.0,
                  "Godot shadow_normal_bias: pushes the shadow lookup off slanted faces (more: less acne on angled walls)")};
    add(c);
  }
  // Brush entities: brushes tied to an entity (Hammer "tie to entity").
  {
    struct B {
      const char *name, *label, *icon, *desc;
      QColor color;
    };
    for (const B &b: {B{"func_detail", "Detail", "mod_wireframe",
                        "Brushes that do not seal the level or block visibility (small trim, props made of brushes).",
                        QColor(120, 200, 255)},
                      B{"func_wall", "Wall", "mod_build", "A brush that is drawn and collides but can be toggled or tinted.",
                        QColor(170, 170, 255)},
                      B{"func_door", "Door", "mod_mask", "Brushes that slide open when used or touched.",
                        QColor(255, 170, 90)},
                      B{"func_illusionary", "Illusionary", "ghost_enabled", "Visible brushes the player walks through.",
                        QColor(200, 140, 255)},
                      B{"func_rigidbody", "Rigid Body", "rigid_body",
                        "A physics object made of brushes: falls and can be pushed (Godot RigidBody3D).",
                        QColor(255, 210, 90)},
                      B{"trigger_multiple", "Trigger", "physics", "An invisible volume that fires its target on touch.",
                        QColor(255, 120, 160)},
                      B{"func_clip", "Clip", "mod_physics",
                        "Invisible brushes the player collides with (e.g. the walkable ramp over stairs). Drawn "
                        "see-through in the 3D view.",
                        QColor(60, 210, 180)}}) {
      EntityClass c{b.name, b.label, "Brush Entities", b.icon, b.desc, b.color, glm::vec3(1.0f), K::Brush, {}};
      c.placeable = false;
      if (QLatin1String(b.name) == QLatin1String("func_door"))
        c.props = {fp("speed", "Speed", 100.0, 0.0, 10000.0, "Source units per second"),
                   fp("moveAngle", "Move direction", 0.0, -360.0, 360.0, "Yaw in degrees; -1 = up, -2 = down"),
                   fp("lip", "Lip", 8.0, -1024.0, 1024.0, "How much of the door stays in the frame"),
                   fp("wait", "Close after", 4.0, -1.0, 1000.0, "Seconds; -1 stays open"),
                   bp("locked", "Starts locked", false)};
      else if (QLatin1String(b.name) == QLatin1String("trigger_multiple"))
        c.props = {sp("target", "Target", ""), fp("wait", "Re-trigger delay", 1.0, 0.0, 1000.0, "Seconds"),
                   bp("startDisabled", "Start disabled", false)};
      else if (QLatin1String(b.name) == QLatin1String("func_rigidbody"))
        c.props = {fu("mass", "Mass", 10.0, 0.01, 100000.0, " kg"),
                   fu("max_hulls", "Collision hulls", 4.0, 1.0, 16.0, "", "Most convex shapes its collision may use",
                      PropType::Int)};
      else if (QLatin1String(b.name) == QLatin1String("func_wall"))
        c.props = {cp("tint", "Tint", "#ffffff"), bp("startDisabled", "Start hidden", false)};
      add(c);
    }
  }
  {
    EntityClass c{"world_env", "World Environment", "World", "world",
                  "The map's sun, sky, fog, exposure, shadows, ambient occlusion and colour grading. Every map has "
                  "exactly one; it cannot be deleted.",
                  QColor(140, 170, 255), glm::vec3(2.4f), K::WorldEnv, {}};
    c.placeable = false;
    add(c);
  }
  {
    EntityClass c{"sun", "Sun", "Lights", "light_sun",
                  "The map's directional light. Aim it with Pitch / Yaw; it drives the sun and its shadows "
                  "(only the first sun in a map is used).",
                  QColor(255, 236, 160), glm::vec3(2.4f), K::Sun, {}};
    c.props = {bp("enabled", "Enabled", true), cp("color", "Colour", "#fff7e6"),
               fp("intensity", "Intensity", 3.7, 0.0, 50.0), bp("castShadows", "Cast shadows", true)};
    c.placeable = false; // superseded by world_env; still loads from older maps
    add(c);
  }
  {
    EntityClass c{"light_spot", "Spot Light", "Lights", "light_spot",
                  "Cone light. Aim it with Pitch / Yaw.", QColor(255, 200, 60), glm::vec3(1.2f), K::SpotLight, {}};
    // Godot SpotLight3D: same names; spot_angle is half the cone; both attenuations are Godot's default 1.
    c.props = {bp("enabled", "Enabled", true), cp("light_color", "Colour", "#ffffff"),
               fp("light_energy", "Energy", 1.0, 0.0, 16.0, "Godot light_energy"),
               fp("light_specular", "Specular", 1.0, 0.0, 16.0, "Godot light_specular: strength of the highlight"),
               fu("spot_range", "Range", 5.0, 0.0, 4096.0, "m", "Godot spot_range, metres (1 m ≈ 49.2 Source units; a 128-unit wall is 2.6 m)"),
               fu("spot_angle", "Angle", 45.0, 0.0, 180.0, "°", "Godot spot_angle: half the cone, degrees", PropType::Angle),
               bp("castShadows", "Cast shadows", true),
               fp("shadow_bias", "Shadow bias", 0.03, 0.0, 10.0,
                  "Godot shadow_bias (a spot's is a fraction of its range, in hundredths)"),
               fp("shadow_normal_bias", "Normal bias", 1.0, 0.0, 10.0,
                  "Godot shadow_normal_bias: pushes the shadow lookup off slanted faces (more: less acne on angled walls)")};
    add(c);
  }
  {
    EntityClass c{"info_player_start", "Player Start", "Gameplay", "armature_data",
                  "Where the player spawns. Yaw sets the facing.", QColor(80, 220, 120), glm::vec3(3.2f, 7.2f, 3.2f),
                  K::PlayerStart, {}};
    c.props = {sp("team", "Team", "")};
    add(c);
  }
  {
    EntityClass c{"camera_3d", "Camera3D", "Gameplay", "outliner_ob_camera",
                  "A camera (Godot Camera3D). Aim it with Pitch / Yaw; select it and tick Preview in the 3D view "
                  "to look through it.",
                  QColor(235, 235, 245), glm::vec3(1.6f), K::Camera, {}};
    c.billboard = QStringLiteral(":/icons_godot/GizmoCamera3D.svg");
    c.props = {fp("fov", "FOV", 75.0, 1.0, 179.0, "Vertical field of view in degrees (Godot Camera3D.fov)")};
    c.io = false;
    add(c);
  }
  {
    EntityClass c{"info_target", "Target", "Gameplay", "empty_axis", "A named point other entities can aim at.",
                  QColor(200, 120, 255), glm::vec3(0.8f), K::Generic, {}};
    add(c);
  }
  {
    // One end of a Connect path, placed where brushes end, facing into the gap.
    EntityClass c{"path_anchor", "Path Anchor", "Editor", "con_followpath",
                  "One end of a connection: face it into the gap; the brushes leave along its arrow.",
                  QColor(190, 150, 255), glm::vec3(0.8f), K::Generic, {}};
    add(c);
  }
  {
    EntityClass c{"path_node", "Path Node", "Gameplay", "empty_arrows",
                  "Waypoint; link with Next to build a route.", QColor(120, 200, 255), glm::vec3(0.8f), K::Generic, {}};
    c.props = {sp("next", "Next node", "", PropType::Target), fp("wait", "Wait", 0.0, 0.0, 600.0, "seconds")};
    add(c);
  }
  {
    EntityClass c{"prop_static", "Static Prop", "Props", "mesh_monkey",
                  "A model placed in the level (OBJ, FBX, glTF, DAE…).", QColor(255, 140, 90), glm::vec3(1.6f), K::Prop, {}};
    c.props = {sp("model", "Model", "", PropType::File, "Path to the model file"), fp("scale", "Scale", 1.0, 0.001, 1000.0),
               bp("solid", "Solid", true), bp("castShadows", "Cast shadows", true)};
    add(c);
  }
  {
    EntityClass c{"ambient_sound", "Sound Emitter", "Audio", "speaker", "Plays a sound at this point.",
                  QColor(255, 120, 200), glm::vec3(1.0f), K::Generic, {}};
    c.props = {sp("sound", "Sound file", "", PropType::File), fp("volume", "Volume", 1.0, 0.0, 4.0),
               fp("radius", "Radius", 512.0, 1.0, 100000.0, "Source units"), bp("loop", "Loop", true)};
    add(c);
  }
  {
    EntityClass c{"env_fog_volume", "Fog Volume", "Environment", "mod_fluid", "Local fog inside its radius.",
                  QColor(170, 190, 210), glm::vec3(1.2f), K::Generic, {}};
    c.props = {cp("color", "Colour", "#a0aab8"), fp("density", "Density", 0.01, 0.0, 1.0), fp("radius", "Radius", 512.0, 1.0, 100000.0)};
    add(c);
  }
  addIOClasses();
}

int EntityRegistry::loadJson(const QString &path) {
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly))
    return 0;
  const QJsonArray arr = QJsonDocument::fromJson(f.readAll()).array();
  int n = 0;
  for (const auto &v: arr) {
    const QJsonObject o = v.toObject();
    EntityClass c;
    c.name = o.value("name").toString();
    if (c.name.isEmpty())
      continue;
    c.label = o.value("label").toString(c.name);
    c.category = o.value("category").toString(QStringLiteral("Custom"));
    c.icon = o.value("icon").toString(QStringLiteral("empty_data"));
    c.description = o.value("description").toString();
    c.color = QColor(o.value("color").toString(QStringLiteral("#5fd3ff")));
    c.size = glm::vec3(float(o.value("size").toDouble(1.2)));
    for (const auto &pv: o.value("props").toArray()) {
      const QJsonObject po = pv.toObject();
      PropDef p;
      p.key = po.value("key").toString();
      p.label = po.value("label").toString(p.key);
      p.tooltip = po.value("tooltip").toString();
      p.type = typeFromString(po.value("type").toString());
      p.defaultValue = po.value("default").toVariant();
      p.minValue = po.value("min").toDouble(0.0);
      p.maxValue = po.value("max").toDouble(0.0);
      for (const auto &ch: po.value("choices").toArray())
        p.choices << ch.toString();
      c.props.push_back(p);
    }
    c.billboard = o.value("billboard").toString();
    c.io = o.value("io").toBool(true);
    auto readIO = [](const QJsonArray &a, QVector<io::IODef> &into) {
      for (const auto &iv: a) {
        const QJsonObject io = iv.toObject();
        io::IODef d;
        d.name = io.value("name").toString();
        if (d.name.isEmpty())
          continue;
        d.param = io::paramTypeFromString(io.value("type").toString(QStringLiteral("void")));
        d.description = io.value("description").toString();
        for (const auto &f: io.value("fires").toArray())
          d.fires << f.toString();
        into.push_back(d);
      }
    };
    readIO(o.value("inputs").toArray(), c.inputs);
    readIO(o.value("outputs").toArray(), c.outputs);
    if (c.io)
      io::addCommonIO(c);
    add(c);
    ++n;
  }
  return n;
}

QStringList EntityRegistry::categories() const {
  QStringList out;
  for (const auto &c: m_classes)
    if (!out.contains(c.category))
      out << c.category;
  return out;
}

const EntityClass *EntityRegistry::find(const QString &name) const {
  const auto it = m_index.constFind(name);
  return it == m_index.constEnd() ? nullptr : &m_classes[*it];
}

QVariant Entity::value(const QString &key) const {
  if (auto it = props.constFind(key); it != props.constEnd())
    return *it;
  if (const EntityClass *c = cls())
    if (const PropDef *p = c->prop(key))
      return p->defaultValue;
  return {};
}

double lightRangeMeters(const Entity &e) {
  const EntityClass *c = e.cls();
  if (!c)
    return 0.0;
  if (c->kind == EntityClass::Kind::PointLight)
    return e.num(QStringLiteral("omni_range"), 6.0);
  if (c->kind == EntityClass::Kind::SpotLight)
    return e.num(QStringLiteral("spot_range"), 5.0);
  return 0.0;
}

double Entity::num(const QString &key, double fallback) const {
  const QVariant v = value(key);
  bool ok = false;
  const double d = v.toDouble(&ok);
  return ok ? d : fallback;
}

QColor Entity::color(const QString &key, const QColor &fallback) const {
  const QVariant v = value(key);
  const QColor c = v.typeId() == QMetaType::QColor ? v.value<QColor>() : QColor(v.toString());
  return c.isValid() ? c : fallback;
}

namespace {
bool usesModelRotation(const Entity &e) {
  const EntityClass *c = e.cls();
  return c && c->kind == EntityClass::Kind::Prop;
}
} // namespace

glm::mat3 entityRotation(const Entity &e) {
  const glm::vec3 a = glm::radians(e.angles);
  if (usesModelRotation(e))
    return glm::mat3(glm::eulerAngleXYZ(a.x, a.y, a.z));
  // Ry(yaw) · Rx(-pitch) · Rz(roll): maps local +Z onto forward().
  return glm::mat3(glm::eulerAngleY(a.y) * glm::eulerAngleX(-a.x) * glm::eulerAngleZ(a.z));
}

glm::vec3 entityAnglesFromRotation(const Entity &e, const glm::mat3 &r) {
  if (usesModelRotation(e)) {
    float x = 0.0f, y = 0.0f, z = 0.0f;
    glm::extractEulerAngleXYZ(glm::mat4(r), x, y, z);
    return glm::degrees(glm::vec3(x, y, z));
  }
  const glm::vec3 f = glm::normalize(r[2]);
  const float pitch = std::asin(std::clamp(f.y, -1.0f, 1.0f));
  // Straight up or down there is no yaw: take it from the right axis (roll comes out 0).
  const float yaw = std::sqrt(f.x * f.x + f.z * f.z) > 1e-5f ? std::atan2(f.x, f.z) : std::atan2(-r[0].z, r[0].x);
  const glm::mat3 noRoll(glm::eulerAngleY(yaw) * glm::eulerAngleX(-pitch));
  const glm::mat3 local = glm::transpose(noRoll) * r;  // = Rz(roll)
  const float roll = std::atan2(local[0].y, local[0].x);
  return glm::degrees(glm::vec3(pitch, yaw, roll));
}

glm::vec3 Entity::forward() const {
  const float pitch = glm::radians(angles.x);
  const float yaw = glm::radians(angles.y);
  // Yaw 0 looks down +Z, pitch positive looks up (Y-up world).
  return glm::normalize(glm::vec3(std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch)));
}

} // namespace eng
