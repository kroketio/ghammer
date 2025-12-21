#include "stair_gen.h"

#include <QJsonArray>

#include <algorithm>
#include <cmath>

#include "face_groups.h"

namespace eng::stairs {

namespace {
constexpr double kSu = 10.0; // Source units per world unit

QJsonArray vec(const glm::dvec3 &v) { return QJsonArray {v.x, v.y, v.z}; }
glm::dvec3 vecFrom(const QJsonValue &v) {
  const QJsonArray a = v.toArray();
  return a.size() == 3 ? glm::dvec3(a[0].toDouble(), a[1].toDouble(), a[2].toDouble()) : glm::dvec3(0.0);
}
} // namespace

int climbAxis(const int direction) { return direction <= 1 ? 0 : 2; }

bool isIdentity(const glm::dmat4 &m) {
  for (int c = 0; c < 4; ++c)
    for (int r = 0; r < 4; ++r)
      if (std::fabs(m[c][r] - (c == r ? 1.0 : 0.0)) > 1e-9)
        return false;
  return true;
}

void applyTransform(Params &p, const glm::dmat4 &m) {
  const glm::dmat4 x = m * p.xform;
  const glm::dmat3 l(x);
  // A signed axis permutation keeping up as up is a quarter turn or mirror in the horizontal plane: stairs are
  // symmetric across their width, so it bakes into a new box and climb direction.
  bool bakeable = glm::length(l[1] - glm::dvec3(0.0, 1.0, 0.0)) < 1e-6;
  for (int c = 0; c < 3 && bakeable; ++c) {
    int ones = 0;
    for (int r = 0; r < 3; ++r) {
      const double v = std::fabs(l[c][r]);
      if (std::fabs(v - 1.0) < 1e-6)
        ++ones;
      else if (v > 1e-6)
        bakeable = false;
    }
    bakeable = bakeable && ones == 1;
  }
  if (!bakeable) {
    p.xform = x;
    return;
  }
  glm::dvec3 mn(1e300), mx(-1e300);
  for (int i = 0; i < 8; ++i) {
    const glm::dvec3 c((i & 1) ? p.mx.x : p.mn.x, (i & 2) ? p.mx.y : p.mn.y, (i & 4) ? p.mx.z : p.mn.z);
    const glm::dvec3 w(x * glm::dvec4(c, 1.0));
    mn = glm::min(mn, w);
    mx = glm::max(mx, w);
  }
  static const glm::dvec3 kDir[4] = {{1, 0, 0}, {-1, 0, 0}, {0, 0, 1}, {0, 0, -1}};
  const glm::dvec3 climb = l * kDir[std::clamp(p.direction, 0, 3)];
  for (int d = 0; d < 4; ++d)
    if (glm::length(climb - kDir[d]) < 1e-6)
      p.direction = d;
  p.mn = mn;
  p.mx = mx;
  p.xform = glm::dmat4(1.0);
}

double climbLength(const Params &p) {
  const int a = climbAxis(p.direction);
  return p.mx[a] - p.mn[a];
}

int stepCount(const Params &p) {
  if (p.sizing == Sizing::Count)
    return std::clamp(p.steps, 1, 256);
  const double h = (p.mx.y - p.mn.y) * kSu;
  return std::clamp(int(std::lround(h / std::max(1.0, p.stepHeight))), 1, 256);
}

namespace {

// Climb space: c along the climb from the low end, h up, w across.
struct Frame {
  int n = 0, dir = 0, ca = 0, xa = 0;
  double D = 0.0, H = 0.0, W = 0.0, run = 0.0, rise = 0.0;
  glm::dvec3 mn {0.0}, mx {0.0}, climbDir {0.0};
  glm::dvec3 world(double c, double h, double w) const {
    glm::dvec3 v;
    v[ca] = (dir == 0 || dir == 2) ? mn[ca] + c : mx[ca] - c;
    v[xa] = mn[xa] + w;
    v.y = mn.y + h;
    return v;
  }
};

bool frameOf(const Params &p, Frame &f) {
  const glm::dvec3 ext = p.mx - p.mn;
  if (ext.x <= 1e-4 || ext.y <= 1e-4 || ext.z <= 1e-4)
    return false;
  f.n = stepCount(p);
  f.dir = std::clamp(p.direction, 0, 3);
  f.ca = climbAxis(f.dir);
  f.xa = f.ca == 0 ? 2 : 0;
  f.D = ext[f.ca];
  f.H = ext.y;
  f.W = ext[f.xa];
  f.run = f.D / f.n;
  f.rise = f.H / f.n;
  f.mn = p.mn;
  f.mx = p.mx;
  f.climbDir[f.ca] = (f.dir == 0 || f.dir == 2) ? 1.0 : -1.0;
  return true;
}

// Every step brush as a side profile (counter-clockwise).
std::vector<std::vector<glm::dvec2>> stepProfiles(const Params &p, const Frame &f) {
  std::vector<std::vector<glm::dvec2>> out;
  const double slopeDrop = std::max(0.0, p.thickness) / kSu;
  for (int s = 0; s < f.n; ++s) {
    const double c0 = f.run * s, c1 = f.run * (s + 1);
    const double top = f.rise * (s + 1);
    std::vector<glm::dvec2> profile;
    switch (p.underside) {
      case Underside::Solid:
        profile = {{c0, 0.0}, {c1, 0.0}, {c1, top}, {c0, top}};
        break;
      case Underside::Planks: {
        const double t = p.thinPlanks ? std::clamp(p.plankThickness / kSu, 1e-3, f.rise) : f.rise;
        profile = {{c0, top - t}, {c1, top - t}, {c1, top}, {c0, top}};
        break;
      }
      case Underside::Slope: {
        // The soffit runs `thickness` below the nosing line, never below the floor.
        auto under = [&](double c) { return std::max(0.0, c * f.H / f.D - slopeDrop); };
        const double u0 = std::min(under(c0), top - 1e-3), u1 = std::min(under(c1), top - 1e-3);
        profile.push_back({c0, u0});
        const double knee = slopeDrop * f.D / f.H;
        if (knee > c0 + 1e-6 && knee < c1 - 1e-6)
          profile.push_back({knee, 0.0});
        profile.push_back({c1, u1});
        profile.push_back({c1, top});
        profile.push_back({c0, top});
        break;
      }
    }
    out.push_back(std::move(profile));
    if (p.nosing) {
      const double nd = std::max(0.0, p.nosingDepth) / kSu;
      const double nh = std::clamp(p.nosingHeight / kSu, 1e-3, f.rise);
      if (nd > 1e-4)
        out.push_back({{c0 - nd, top - nh}, {c0, top - nh}, {c0, top}, {c0 - nd, top}});
    }
  }
  return out;
}

std::optional<ConvexBrush> extrude(const std::vector<glm::dvec2> &profile, const Frame &f,
                                   const MaterialPtr &material, const double tilesPerUnit, double w0 = 0.0,
                                   double w1 = -1.0) {
  if (w1 < 0.0)
    w1 = f.W;
  std::vector<glm::dvec3> pts;
  for (const double w: {w0, w1})
    for (const auto &q: profile)
      pts.push_back(f.world(q.x, q.y, w));
  return ConvexBrush::fromPoints(pts, material, tilesPerUnit);
}

// Keeps the part of a convex polygon where a*c + b*h <= d.
std::vector<glm::dvec2> clipHalf(const std::vector<glm::dvec2> &poly, double a, double b, double d) {
  std::vector<glm::dvec2> out;
  for (size_t i = 0; i < poly.size(); ++i) {
    const glm::dvec2 &p = poly[i], &q = poly[(i + 1) % poly.size()];
    const double sp = a * p.x + b * p.y - d, sq = a * q.x + b * q.y - d;
    if (sp <= 1e-9)
      out.push_back(p);
    if ((sp < -1e-9 && sq > 1e-9) || (sp > 1e-9 && sq < -1e-9))
      out.push_back(p + (q - p) * (sp / (sp - sq)));
  }
  return out;
}

// 0 without sides (or too narrow to keep a step between them).
double sideWidthOf(const Params &p, const Frame &f) {
  if (!p.sides)
    return 0.0;
  const double sw = std::max(0.0, p.sideWidth) / kSu;
  return sw > 1e-4 && f.W - 2.0 * sw > 1e-3 ? sw : 0.0;
}

// h = k*c + top over every step corner, h = k*c + bottom under them (k = the slope).
void stepLines(const Params &p, const Frame &f, double &k, double &top, double &bottom) {
  k = f.H / f.D;
  top = -1e300;
  bottom = 1e300;
  for (const auto &piece: stepProfiles(p, f))
    for (const auto &q: piece) {
      top = std::max(top, q.y - k * q.x);
      bottom = std::min(bottom, q.y - k * q.x);
    }
}

// Under the nosing line raised by sideHeight (flat at H + sideHeight over the top step), down to the floor
// (solid) or the line under the steps (planks / slope).
std::vector<glm::dvec2> sideProfile(const Params &p, const Frame &f) {
  double k, top, bottom;
  stepLines(p, f, k, top, bottom);
  const double sh = std::max(0.0, p.sideHeight) / kSu;
  std::vector<glm::dvec2> poly {{0.0, 0.0}, {f.D, 0.0}, {f.D, f.H + sh}, {0.0, f.H + sh}};
  poly = clipHalf(poly, -k, 1.0, top + sh);
  if (p.underside != Underside::Solid)
    poly = clipHalf(poly, k, -1.0, -bottom);
  return poly;
}

} // namespace

std::vector<ConvexBrush> build(const Params &p, const MaterialPtr &material, const double tilesPerUnit) {
  std::vector<ConvexBrush> out;
  Frame f;
  if (!frameOf(p, f))
    return out;
  const double sw = sideWidthOf(p, f);
  for (const auto &piece: stepProfiles(p, f)) {
    auto b = extrude(piece, f, material, tilesPerUnit, sw, f.W - sw);
    if (!b)
      continue;
    for (BrushFace &face: b->faces) {
      const double up = face.normal.y, along = glm::dot(face.normal, f.climbDir);
      if (std::fabs(up) > 0.7)
        face.groups = {facegroup::kStairFlat};
      else if (std::fabs(along) > 0.7)
        face.groups = {facegroup::kStairWall};
      else if (std::fabs(face.normal[f.xa]) > 0.7)
        face.groups = {facegroup::kStairSide};
      else
        face.groups = {facegroup::kStairUnder};
    }
    out.push_back(std::move(*b));
  }
  if (sw > 0.0) {
    const std::vector<glm::dvec2> side = sideProfile(p, f);
    if (side.size() >= 3)
      for (const auto &[w0, w1]: {std::pair {0.0, sw}, std::pair {f.W - sw, f.W}})
        if (auto b = extrude(side, f, material, tilesPerUnit, w0, w1)) {
          for (BrushFace &face: b->faces)
            face.groups = {facegroup::kStairStringer};
          out.push_back(std::move(*b));
        }
  }
  if (!isIdentity(p.xform))
    for (auto &b: out)
      b.transform(p.xform, true);
  return out;
}

std::optional<ConvexBrush> buildCollision(const Params &p, const MaterialPtr &material, const double tilesPerUnit) {
  Frame f;
  if (!p.collision || !frameOf(p, f))
    return std::nullopt;
  double k, top, bottom;
  stepLines(p, f, k, top, bottom);
  // The ramp starts where its top line meets the floor in front of the first step, outside the box.
  const double front = std::min(0.0, -top / k);
  std::vector<glm::dvec2> poly {{front, 0.0}, {f.D, 0.0}, {f.D, f.H}, {front, f.H}};
  poly = clipHalf(poly, -k, 1.0, top);
  if (p.underside != Underside::Solid)
    poly = clipHalf(poly, k, -1.0, -bottom);
  if (poly.size() < 3)
    return std::nullopt;
  auto b = extrude(poly, f, material, tilesPerUnit);
  if (!b)
    return std::nullopt;
  if (!isIdentity(p.xform))
    b->transform(p.xform, true);
  return b;
}

QJsonObject toJson(const Params &p) {
  QJsonObject o;
  o[QStringLiteral("min")] = vec(p.mn);
  o[QStringLiteral("max")] = vec(p.mx);
  o[QStringLiteral("direction")] = p.direction;
  o[QStringLiteral("sizing")] = int(p.sizing);
  o[QStringLiteral("stepHeight")] = p.stepHeight;
  o[QStringLiteral("stepDepth")] = p.stepDepth;
  o[QStringLiteral("steps")] = p.steps;
  o[QStringLiteral("underside")] = int(p.underside);
  o[QStringLiteral("thickness")] = p.thickness;
  o[QStringLiteral("thinPlanks")] = p.thinPlanks;
  o[QStringLiteral("plankThickness")] = p.plankThickness;
  o[QStringLiteral("nosing")] = p.nosing;
  o[QStringLiteral("nosingDepth")] = p.nosingDepth;
  o[QStringLiteral("nosingHeight")] = p.nosingHeight;
  o[QStringLiteral("collision")] = p.collision;
  o[QStringLiteral("sides")] = p.sides;
  o[QStringLiteral("sideWidth")] = p.sideWidth;
  o[QStringLiteral("sideHeight")] = p.sideHeight;
  if (!isIdentity(p.xform)) {
    QJsonArray m;
    for (int c = 0; c < 4; ++c)
      for (int r = 0; r < 4; ++r)
        m.append(p.xform[c][r]);
    o[QStringLiteral("xform")] = m;
  }
  return o;
}

Params fromJson(const QJsonObject &o) {
  Params p;
  p.mn = vecFrom(o.value(QStringLiteral("min")));
  p.mx = vecFrom(o.value(QStringLiteral("max")));
  p.direction = std::clamp(o.value(QStringLiteral("direction")).toInt(2), 0, 3);
  p.sizing = Sizing(std::clamp(o.value(QStringLiteral("sizing")).toInt(0), 0, 1));
  p.stepHeight = std::max(1.0, o.value(QStringLiteral("stepHeight")).toDouble(8.0));
  p.stepDepth = std::max(1.0, o.value(QStringLiteral("stepDepth")).toDouble(8.0));
  p.steps = std::clamp(o.value(QStringLiteral("steps")).toInt(8), 1, 256);
  p.underside = Underside(std::clamp(o.value(QStringLiteral("underside")).toInt(0), 0, 2));
  p.thickness = std::max(1.0, o.value(QStringLiteral("thickness")).toDouble(16.0));
  p.thinPlanks = o.value(QStringLiteral("thinPlanks")).toBool(false);
  p.plankThickness = std::max(0.1, o.value(QStringLiteral("plankThickness")).toDouble(4.0));
  p.nosing = o.value(QStringLiteral("nosing")).toBool(false);
  p.nosingDepth = std::max(0.1, o.value(QStringLiteral("nosingDepth")).toDouble(6.0));
  p.nosingHeight = std::max(0.1, o.value(QStringLiteral("nosingHeight")).toDouble(2.0));
  p.collision = o.value(QStringLiteral("collision")).toBool(true);
  p.sides = o.value(QStringLiteral("sides")).toBool(false);
  p.sideWidth = std::max(0.1, o.value(QStringLiteral("sideWidth")).toDouble(4.0));
  p.sideHeight = std::max(0.0, o.value(QStringLiteral("sideHeight")).toDouble(8.0));
  const QJsonArray m = o.value(QStringLiteral("xform")).toArray();
  if (m.size() == 16)
    for (int c = 0; c < 4; ++c)
      for (int r = 0; r < 4; ++r)
        p.xform[c][r] = m[c * 4 + r].toDouble();
  return p;
}

const char *partKey(const Part part) {
  switch (part) {
    case Part::Flat: return "flat";
    case Part::Wall: return "wall";
    case Part::Stringer: return "stringer";
    case Part::Count: break;
  }
  return "";
}

Part partOfGroup(const int groupId) {
  switch (groupId) {
    case facegroup::kStairFlat: return Part::Flat;
    case facegroup::kStairWall: return Part::Wall;
    case facegroup::kStairStringer: return Part::Stringer;
    default: return Part::Count;
  }
}

int groupOfPart(const Part part) {
  switch (part) {
    case Part::Flat: return facegroup::kStairFlat;
    case Part::Wall: return facegroup::kStairWall;
    case Part::Stringer: return facegroup::kStairStringer;
    case Part::Count: break;
  }
  return 0;
}

QJsonObject stepTexToJson(const StepTex &t) {
  QJsonObject o;
  o[QStringLiteral("on")] = t.on;
  o[QStringLiteral("rotation")] = t.rotation;
  o[QStringLiteral("offsetU")] = t.offsetU;
  o[QStringLiteral("offsetV")] = t.offsetV;
  o[QStringLiteral("tile")] = t.tile;
  if (!t.material.isEmpty())
    o[QStringLiteral("material")] = t.material;
  return o;
}

StepTex stepTexFromJson(const QJsonObject &o) {
  StepTex t;
  t.on = o.value(QStringLiteral("on")).toBool(false);
  t.rotation = ((o.value(QStringLiteral("rotation")).toInt(0) % 4) + 4) % 4;
  t.offsetU = o.value(QStringLiteral("offsetU")).toDouble(0.0);
  t.offsetV = o.value(QStringLiteral("offsetV")).toDouble(0.0);
  t.tile = std::max(0.01, o.value(QStringLiteral("tile")).toDouble(64.0));
  t.material = o.value(QStringLiteral("material")).toString();
  return t;
}

StepTex stepTexOf(const QJsonObject &procParams, const Part part) {
  return stepTexFromJson(
      procParams.value(QStringLiteral("tex")).toObject().value(QLatin1String(partKey(part))).toObject());
}

void setStepTex(QJsonObject &procParams, const Part part, const StepTex &t) {
  QJsonObject tex = procParams.value(QStringLiteral("tex")).toObject();
  tex[QLatin1String(partKey(part))] = stepTexToJson(t);
  procParams[QStringLiteral("tex")] = tex;
}

FaceTexBasis stepTexBasis(const Params &p, const glm::dvec3 &normal, const std::vector<glm::dvec3> &polygon,
                          const StepTex &t, const int texW, const int texH) {
  const glm::dvec3 n = glm::normalize(normal);
  // Climb and up in world space (xform included).
  const glm::dmat3 l(p.xform);
  static const glm::dvec3 kDir[4] = {{1, 0, 0}, {-1, 0, 0}, {0, 0, 1}, {0, 0, -1}};
  const glm::dvec3 climb = glm::normalize(l * kDir[std::clamp(p.direction, 0, 3)]);
  const glm::dvec3 up = glm::normalize(l * glm::dvec3(0.0, 1.0, 0.0));
  // Treads look "up the stairs"; everything else is upright.
  glm::dvec3 screenUp = std::fabs(glm::dot(n, up)) > 0.7 ? climb : up;
  glm::dvec3 u = glm::cross(screenUp, n);
  if (glm::length(u) < 1e-6)
    u = glm::cross(glm::dvec3(1.0, 0.0, 0.0), n);
  u = glm::normalize(u);
  for (int r = 0; r < ((t.rotation % 4) + 4) % 4; ++r)
    u = glm::normalize(glm::cross(n, u));  // a quarter turn about n
  const glm::dvec3 v = glm::normalize(glm::cross(u, n));

  glm::dvec3 centre(0.0);
  for (const auto &q: polygon)
    centre += q;
  if (!polygon.empty())
    centre /= double(polygon.size());

  const double w = std::max(1, texW), h = std::max(1, texH);
  const double tileU = std::max(0.01, t.tile) / kSu;  // world units
  const double tileV = tileU * h / w;
  FaceTexBasis b;
  b.uAxis = glm::vec3(u);
  b.vAxis = glm::vec3(v);
  b.tilesPerUnitU = float(1.0 / tileU);
  b.tilesPerUnitV = float(1.0 / tileV);
  b.offsetU = float(0.5 + t.offsetU / w - glm::dot(centre, u) / tileU);
  b.offsetV = float(0.5 + t.offsetV / h - glm::dot(centre, v) / tileV);
  b.valid = true;
  return b;
}

} // namespace eng::stairs
