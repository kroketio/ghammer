#include "modifiers.h"

#include <algorithm>
#include <cmath>

#include <QJsonArray>

namespace eng::mods {

namespace {

QJsonArray vec3(const glm::dvec3 &v) { return {v.x, v.y, v.z}; }
glm::dvec3 vec3(const QJsonValue &v, const glm::dvec3 &def) {
  const QJsonArray a = v.toArray();
  return a.size() == 3 ? glm::dvec3(a[0].toDouble(), a[1].toDouble(), a[2].toDouble()) : def;
}

QJsonObject arrayDefaults() { return ArrayParams().toJson(); }
QJsonObject curveDefaults() { return CurveParams().toJson(); }

} // namespace

QJsonObject toJson(const Host &h) {
  QJsonObject o;
  o.insert(QStringLiteral("id"), double(h.id));
  o.insert(QStringLiteral("name"), h.name);
  o.insert(QStringLiteral("orientation"), QJsonArray {h.orientation.w, h.orientation.x, h.orientation.y, h.orientation.z});
  o.insert(QStringLiteral("nextModifierId"), double(h.nextModifierId));
  QJsonArray stack;
  for (const Modifier &m: h.stack)
    stack.append(QJsonObject {{QStringLiteral("id"), double(m.id)},
                              {QStringLiteral("kind"), m.kind},
                              {QStringLiteral("name"), m.name},
                              {QStringLiteral("enabled"), m.enabled},
                              {QStringLiteral("editSource"), m.showInEditSource},
                              {QStringLiteral("expanded"), m.expanded},
                              {QStringLiteral("params"), m.params}});
  o.insert(QStringLiteral("stack"), stack);
  return o;
}

Host hostFromJson(const QJsonObject &o) {
  Host h;
  h.id = quint32(o.value(QStringLiteral("id")).toDouble());
  h.name = o.value(QStringLiteral("name")).toString();
  const QJsonArray q = o.value(QStringLiteral("orientation")).toArray();
  if (q.size() == 4)
    h.orientation = glm::normalize(glm::dquat(q[0].toDouble(1.0), q[1].toDouble(), q[2].toDouble(), q[3].toDouble()));
  for (const QJsonValue &v: o.value(QStringLiteral("stack")).toArray()) {
    const QJsonObject m = v.toObject();
    Modifier mod;
    mod.id = quint32(m.value(QStringLiteral("id")).toDouble());
    mod.kind = m.value(QStringLiteral("kind")).toString();
    mod.name = m.value(QStringLiteral("name")).toString();
    mod.enabled = m.value(QStringLiteral("enabled")).toBool(true);
    mod.showInEditSource = m.value(QStringLiteral("editSource")).toBool(true);
    mod.expanded = m.value(QStringLiteral("expanded")).toBool(true);
    mod.params = m.value(QStringLiteral("params")).toObject();
    h.stack.push_back(mod);
  }
  h.nextModifierId = quint32(o.value(QStringLiteral("nextModifierId")).toDouble(1));
  for (const Modifier &m: h.stack)
    h.nextModifierId = std::max(h.nextModifierId, m.id + 1);
  return h;
}

const std::vector<KindInfo> &kinds() {
  static const std::vector<KindInfo> k = {
      {QStringLiteral("array"), QStringLiteral("Array"), QStringLiteral("mod_array"), Category::Geometry, arrayDefaults},
      {QStringLiteral("curve"), QStringLiteral("Curve"), QStringLiteral("mod_curve"), Category::Geometry, curveDefaults},
  };
  return k;
}

const KindInfo *kindInfo(const QString &id) {
  for (const KindInfo &k: kinds())
    if (k.id == id)
      return &k;
  return nullptr;
}

ArrayParams ArrayParams::fromJson(const QJsonObject &o) {
  ArrayParams p;
  const QString fit = o.value(QStringLiteral("fit")).toString();
  p.fit = fit == QLatin1String("length") ? Fit::Length : fit == QLatin1String("curve") ? Fit::Curve : Fit::Count;
  p.count = std::clamp(o.value(QStringLiteral("count")).toInt(p.count), 1, 1000);
  p.length = o.value(QStringLiteral("length")).toDouble(p.length);
  p.curvePath = quint32(o.value(QStringLiteral("curvePath")).toDouble(0));
  p.useRelative = o.value(QStringLiteral("useRelative")).toBool(p.useRelative);
  p.relative = vec3(o.value(QStringLiteral("relative")), p.relative);
  p.useConstant = o.value(QStringLiteral("useConstant")).toBool(p.useConstant);
  p.constant = vec3(o.value(QStringLiteral("constant")), p.constant);
  p.textureFollow = o.value(QStringLiteral("textures")).toString() != QLatin1String("world");
  return p;
}

QJsonObject ArrayParams::toJson() const {
  return {{QStringLiteral("v"), 1},
          {QStringLiteral("fit"), fit == Fit::Length  ? QStringLiteral("length")
                                  : fit == Fit::Curve ? QStringLiteral("curve")
                                                      : QStringLiteral("count")},
          {QStringLiteral("count"), count},
          {QStringLiteral("length"), length},
          {QStringLiteral("curvePath"), double(curvePath)},
          {QStringLiteral("useRelative"), useRelative},
          {QStringLiteral("relative"), vec3(relative)},
          {QStringLiteral("useConstant"), useConstant},
          {QStringLiteral("constant"), vec3(constant)},
          {QStringLiteral("textures"), textureFollow ? QStringLiteral("follow") : QStringLiteral("world")}};
}

CurveParams CurveParams::fromJson(const QJsonObject &o) {
  CurveParams p;
  p.path = quint32(o.value(QStringLiteral("path")).toDouble(0));
  p.axis = std::clamp(o.value(QStringLiteral("axis")).toInt(p.axis), 0, 5);
  p.mode = o.value(QStringLiteral("mode")).toString() == QLatin1String("rigid") ? Mode::Rigid : Mode::Bend;
  p.rigidUnit = o.value(QStringLiteral("rigidUnit")).toString() == QLatin1String("copy") ? Unit::Copy : Unit::Brush;
  p.frame = o.value(QStringLiteral("frame")).toString() == QLatin1String("follow") ? Frame::Follow : Frame::KeepVertical;
  const QString anchor = o.value(QStringLiteral("anchor")).toString();
  p.anchor = anchor == QLatin1String("center") ? Anchor::Center : anchor == QLatin1String("top") ? Anchor::Top : Anchor::Bottom;
  p.offsetAlong = o.value(QStringLiteral("offsetAlong")).toDouble(0.0);
  p.offsetSide = o.value(QStringLiteral("offsetSide")).toDouble(0.0);
  p.offsetHeight = o.value(QStringLiteral("offsetHeight")).toDouble(0.0);
  p.segAngle = std::clamp(o.value(QStringLiteral("segAngle")).toDouble(p.segAngle), 0.5, 90.0);
  p.segMaxLen = std::max(0.0, o.value(QStringLiteral("segMaxLen")).toDouble(p.segMaxLen));
  p.segMinLen = std::max(0.0, o.value(QStringLiteral("segMinLen")).toDouble(p.segMinLen));
  p.stretch = o.value(QStringLiteral("stretch")).toBool(false);
  p.reverse = o.value(QStringLiteral("reverse")).toBool(false);
  const QString tex = o.value(QStringLiteral("textures")).toString();
  p.texture = tex == QLatin1String("bend") ? Texture::Bend : tex == QLatin1String("world") ? Texture::World : Texture::Path;
  p.maxSlopeDeg = std::clamp(o.value(QStringLiteral("maxSlope")).toDouble(p.maxSlopeDeg), 1.0, 89.0);
  p.minJacobian = std::clamp(o.value(QStringLiteral("minJacobian")).toDouble(p.minJacobian), 0.01, 0.9);
  return p;
}

QJsonObject CurveParams::toJson() const {
  return {{QStringLiteral("v"), 1},
          {QStringLiteral("path"), double(path)},
          {QStringLiteral("axis"), axis},
          {QStringLiteral("mode"), mode == Mode::Rigid ? QStringLiteral("rigid") : QStringLiteral("bend")},
          {QStringLiteral("rigidUnit"), rigidUnit == Unit::Copy ? QStringLiteral("copy") : QStringLiteral("brush")},
          {QStringLiteral("frame"), frame == Frame::Follow ? QStringLiteral("follow") : QStringLiteral("vertical")},
          {QStringLiteral("anchor"), anchor == Anchor::Center ? QStringLiteral("center")
                                     : anchor == Anchor::Top  ? QStringLiteral("top")
                                                              : QStringLiteral("bottom")},
          {QStringLiteral("offsetAlong"), offsetAlong},
          {QStringLiteral("offsetSide"), offsetSide},
          {QStringLiteral("offsetHeight"), offsetHeight},
          {QStringLiteral("segAngle"), segAngle},
          {QStringLiteral("segMaxLen"), segMaxLen},
          {QStringLiteral("segMinLen"), segMinLen},
          {QStringLiteral("stretch"), stretch},
          {QStringLiteral("reverse"), reverse},
          {QStringLiteral("textures"), texture == Texture::Bend    ? QStringLiteral("bend")
                                       : texture == Texture::World ? QStringLiteral("world")
                                                                   : QStringLiteral("path")},
          {QStringLiteral("maxSlope"), maxSlopeDeg},
          {QStringLiteral("minJacobian"), minJacobian}};
}

void Context::error(const QString &text, const quint32 pathId, const double s0, const double s1) {
  out.diagnostics.push_back({Diagnostic::Severity::Error, mod.id, text, pathId, s0, s1});
  out.error = true;
}

void Context::warning(const QString &text, const quint32 pathId, const double s0, const double s1) {
  out.diagnostics.push_back({Diagnostic::Severity::Warning, mod.id, text, pathId, s0, s1});
}

void localBounds(const std::vector<Piece> &pieces, const glm::dvec3 axes[3], glm::dvec3 &mn, glm::dvec3 &mx) {
  mn = glm::dvec3(1e300);
  mx = glm::dvec3(-1e300);
  for (const Piece &p: pieces)
    for (const auto &poly: p.brush.polygons)
      for (const auto &v: poly)
        for (int i = 0; i < 3; ++i) {
          const double d = glm::dot(v, axes[i]);
          mn[i] = std::min(mn[i], d);
          mx[i] = std::max(mx[i], d);
        }
}

void curveAxes(const glm::dquat &q, const int axis, glm::dvec3 &f, glm::dvec3 &u, glm::dvec3 &r) {
  static const glm::dvec3 dirs[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
  f = glm::normalize(q * dirs[std::clamp(axis, 0, 5)]);
  // Up is the host's Y, unless the curve runs along Y (then its Z).
  const bool vertical = axis == 2 || axis == 3;
  u = glm::normalize(q * (vertical ? glm::dvec3(0, 0, 1) : glm::dvec3(0, 1, 0)));
  r = glm::cross(f, u);
}

EvalOutput evaluate(const EvalInput &in) {
  EvalOutput out;
  std::vector<Piece> pieces = in.sources;
  for (size_t i = 0; i < in.stack.size(); ++i) {
    const Modifier &m = in.stack[i];
    if (!m.enabled || (in.editSource && !m.showInEditSource))
      continue;
    Context ctx {in, out, m, int(i)};
    if (m.kind == QLatin1String("array")) {
      applyArray(ctx, pieces);
    } else if (m.kind == QLatin1String("curve")) {
      // An array before it repeating across the path lays copies side by side (too wide for the bend).
      const int axis = std::clamp(CurveParams::fromJson(m.params).axis, 0, 5);
      static const char *names[3] = {"X", "Y", "Z"};
      for (size_t j = 0; j < i; ++j) {
        const Modifier &a = in.stack[j];
        if (!a.enabled || a.kind != QLatin1String("array") || (in.editSource && !a.showInEditSource))
          continue;
        const ArrayParams ap = ArrayParams::fromJson(a.params);
        const glm::dvec3 dir = (ap.useRelative ? ap.relative : glm::dvec3(0.0)) + (ap.useConstant ? ap.constant : glm::dvec3(0.0));
        if (glm::length(dir) < 1e-9 || std::fabs(glm::normalize(dir)[axis / 2]) >= 0.5)
          continue;
        int along = 0;
        for (int k = 1; k < 3; ++k)
          if (std::fabs(dir[k]) > std::fabs(dir[along]))
            along = k;
        ctx.warning(QStringLiteral("\"%1\" repeats along %2, across the path: point its offset along %3 (the deform axis)")
                        .arg(a.name, QLatin1String(names[along]), QLatin1String(names[axis / 2])));
      }
      applyCurve(ctx, pieces);
    } else {
      ctx.warning(QStringLiteral("Unknown modifier \"%1\" (skipped)").arg(m.kind));
      continue;
    }
    out.active = true;
    if (!out.error && int(pieces.size()) > in.maxPieces)
      ctx.error(QStringLiteral("Too many pieces (%1, at most %2)").arg(pieces.size()).arg(in.maxPieces));
    if (out.error) {
      pieces.clear();
      break;
    }
  }
  if (in.editSource)
    pieces.erase(std::remove_if(pieces.begin(), pieces.end(), [](const Piece &p) { return p.identity; }),
                 pieces.end());
  out.pieces = std::move(pieces);
  return out;
}

} // namespace eng::mods
