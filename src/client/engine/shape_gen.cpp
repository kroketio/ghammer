#include "shape_gen.h"

#include <QJsonArray>

#include <algorithm>
#include <cmath>
#include <optional>

namespace eng {

namespace {

constexpr double kPi = 3.14159265358979323846;

// Unit space: the axis is Y, the cross section X / Z, all in [0, 1]; placed into the box with the axis along `axis`.
glm::dvec3 place(const glm::dvec3 &u, const glm::dvec3 &mn, const glm::dvec3 &mx, const int axis) {
  glm::dvec3 q = u;
  if (axis == 0)
    q = {u.y, u.x, u.z};
  else if (axis == 2)
    q = {u.x, u.z, u.y};
  return mn + q * (mx - mn);
}

// The cross-section axes' world extents (first maps unit X, second unit Z) and the length along the axis.
void extents(const glm::dvec3 &mn, const glm::dvec3 &mx, const int axis, double &across, double &deep, double &len) {
  const glm::dvec3 e = mx - mn;
  len = e[axis];
  if (axis == 0) {
    across = e.y;
    deep = e.z;
  } else if (axis == 1) {
    across = e.x;
    deep = e.z;
  } else {
    across = e.x;
    deep = e.y;
  }
}

// n points round the cross section, stretched so they span the whole box.
std::vector<glm::dvec2> ring(const int n) {
  std::vector<glm::dvec2> pts;
  glm::dvec2 lo(1e30), hi(-1e30);
  for (int i = 0; i < n; ++i) {
    const double a = 2.0 * kPi * (i + 0.5) / n;
    const glm::dvec2 p(std::cos(a), std::sin(a));
    pts.push_back(p);
    lo = glm::min(lo, p);
    hi = glm::max(hi, p);
  }
  for (auto &p: pts)
    p = (p - lo) / (hi - lo);
  return pts;
}

std::optional<ConvexBrush> hull(const std::vector<glm::dvec3> &unit, const glm::dvec3 &mn, const glm::dvec3 &mx,
                                const int axis, const MaterialPtr &mat, const double tpu) {
  std::vector<glm::dvec3> pts;
  pts.reserve(unit.size());
  for (const auto &u: unit)
    pts.push_back(place(u, mn, mx, axis));
  return ConvexBrush::fromPoints(pts, mat, tpu);
}

} // namespace

QString shapeTypeName(const ShapeKind kind) {
  switch (kind) {
    case ShapeKind::Capsule: return QStringLiteral("capsule");
    case ShapeKind::Pyramid: return QStringLiteral("pyramid");
    case ShapeKind::Cylinder:
    default: return QStringLiteral("cylinder");
  }
}

bool shapeKindOf(const QString &type, ShapeKind &kind) {
  for (const ShapeKind k: {ShapeKind::Cylinder, ShapeKind::Capsule, ShapeKind::Pyramid})
    if (type == shapeTypeName(k)) {
      kind = k;
      return true;
    }
  return false;
}

std::vector<ConvexBrush> buildShape(const glm::dvec3 &mn, const glm::dvec3 &mx, const ShapeParams &prm,
                                    const MaterialPtr &mat, const double tpu) {
  std::vector<ConvexBrush> out;
  if (glm::any(glm::lessThanEqual(mx - mn, glm::dvec3(1e-6))))
    return out;
  const int axis = std::clamp(prm.axis, 0, 2);
  double across = 0.0, deep = 0.0, len = 0.0;
  extents(mn, mx, axis, across, deep, len);
  const auto push = [&out](std::optional<ConvexBrush> b) {
    if (b)
      out.push_back(std::move(*b));
  };

  switch (prm.kind) {
    case ShapeKind::Cylinder: {
      const int n = std::clamp(prm.sides, 3, 64);
      const auto outer = ring(n);
      const double wa = prm.wall / across, wd = prm.wall / deep;
      if (prm.wall <= 1e-6 || wa >= 0.5 || wd >= 0.5) {
        std::vector<glm::dvec3> pts;
        for (const auto &p: outer) {
          pts.push_back({p.x, 0.0, p.y});
          pts.push_back({p.x, 1.0, p.y});
        }
        push(hull(pts, mn, mx, axis, mat, tpu));
        break;
      }
      // Hollow: one convex piece per side, between the outer ring and the same ring shrunk by the wall.
      const glm::dvec2 c(0.5);
      const glm::dvec2 shrink(1.0 - 2.0 * wa, 1.0 - 2.0 * wd);
      for (int i = 0; i < n; ++i) {
        const glm::dvec2 o0 = outer[size_t(i)], o1 = outer[size_t((i + 1) % n)];
        const glm::dvec2 i0 = c + (o0 - c) * shrink, i1 = c + (o1 - c) * shrink;
        std::vector<glm::dvec3> pts;
        for (const glm::dvec2 &p: {o0, o1, i1, i0}) {
          pts.push_back({p.x, 0.0, p.y});
          pts.push_back({p.x, 1.0, p.y});
        }
        push(hull(pts, mn, mx, axis, mat, tpu));
      }
      break;
    }
    case ShapeKind::Capsule: {
      // Rounded ends as deep as the narrower half of the cross section (at most half the length).
      const int n = std::clamp(prm.sides, 3, 64);
      const int caps = std::clamp(prm.capSegments, 1, 16);
      const auto outer = ring(n);
      const double h = std::min(0.5 * std::min(across, deep) / len, 0.5);
      std::vector<glm::dvec3> pts;
      for (int k = 0; k < caps; ++k) {
        const double phi = 0.5 * kPi * k / caps;
        const double r = std::cos(phi), y = h * (1.0 - std::sin(phi));
        for (const auto &p: outer) {
          const glm::dvec2 q = glm::dvec2(0.5) + (p - glm::dvec2(0.5)) * r;
          pts.push_back({q.x, y, q.y});
          pts.push_back({q.x, 1.0 - y, q.y});
        }
      }
      pts.push_back({0.5, 0.0, 0.5});
      pts.push_back({0.5, 1.0, 0.5});
      push(hull(pts, mn, mx, axis, mat, tpu));
      break;
    }
    case ShapeKind::Pyramid: {
      const int n = std::clamp(prm.sides, 3, 64);
      std::vector<glm::dvec3> pts;
      // Four sides fill the box corner to corner, like a classic pyramid.
      if (n == 4)
        pts = {{0, 0, 0}, {1, 0, 0}, {0, 0, 1}, {1, 0, 1}};
      else
        for (const auto &p: ring(n))
          pts.push_back({p.x, 0.0, p.y});
      pts.push_back({0.5, 1.0, 0.5});
      push(hull(pts, mn, mx, axis, mat, tpu));
      break;
    }
  }
  return out;
}

QJsonObject shapeToJson(const glm::dvec3 &mn, const glm::dvec3 &mx, const ShapeParams &p) {
  auto vec = [](const glm::dvec3 &v) { return QJsonArray {v.x, v.y, v.z}; };
  return {{QStringLiteral("min"), vec(mn)},          {QStringLiteral("max"), vec(mx)},
          {QStringLiteral("sides"), p.sides},        {QStringLiteral("axis"), p.axis},
          {QStringLiteral("wall"), p.wall},          {QStringLiteral("capSegments"), p.capSegments},
          {QStringLiteral("smooth"), p.smooth}};
}

void shapeFromJson(const QJsonObject &o, glm::dvec3 &mn, glm::dvec3 &mx, ShapeParams &p) {
  auto vec = [](const QJsonValue &v, const glm::dvec3 &fallback) {
    const QJsonArray a = v.toArray();
    return a.size() == 3 ? glm::dvec3(a[0].toDouble(), a[1].toDouble(), a[2].toDouble()) : fallback;
  };
  mn = vec(o.value(QStringLiteral("min")), mn);
  mx = vec(o.value(QStringLiteral("max")), mx);
  p.sides = std::clamp(o.value(QStringLiteral("sides")).toInt(p.sides), 3, 64);
  p.axis = std::clamp(o.value(QStringLiteral("axis")).toInt(p.axis), 0, 2);
  p.wall = std::max(0.0, o.value(QStringLiteral("wall")).toDouble(p.wall));
  p.capSegments = std::clamp(o.value(QStringLiteral("capSegments")).toInt(p.capSegments), 1, 16);
  p.smooth = o.value(QStringLiteral("smooth")).toBool(p.smooth);
}

} // namespace eng
