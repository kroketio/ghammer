#include "arch_gen.h"

#include <QJsonArray>

#include <algorithm>
#include <cmath>

namespace eng {

namespace {
constexpr double kPi = 3.14159265358979323846;

// Profile (x across, y up) -> world, extruded over [d0, d1] along the depth axis.
struct Frame {
  int span = 0;  // 0 = X, 2 = Z
  int depth = 2; // the other horizontal axis
  double d0 = 0.0, d1 = 1.0;
  glm::dvec3 at(double x, double y, double d) const {
    glm::dvec3 p(0.0);
    p[span] = x;
    p.y = y;
    p[depth] = d;
    return p;
  }
};

std::optional<ConvexBrush> prism(const Frame &f, const std::vector<glm::dvec2> &profile, const MaterialPtr &mat,
                                 double tpu) {
  std::vector<glm::dvec3> pts;
  pts.reserve(profile.size() * 2);
  for (const auto &q: profile) {
    pts.push_back(f.at(q.x, q.y, f.d0));
    pts.push_back(f.at(q.x, q.y, f.d1));
  }
  auto b = ConvexBrush::fromPoints(pts, mat, tpu);
  if (b && !b->valid())
    return std::nullopt;
  return b;
}

} // namespace

std::vector<glm::dvec2> archCurve(const ArchProfile profile, const double x0, const double x1, const double base,
                                  const double top, int segments, const double rise) {
  std::vector<glm::dvec2> out;
  segments = std::clamp(segments, 2, 64);
  const double a = 0.5 * (x1 - x0); // half span
  const double cx = 0.5 * (x0 + x1);
  const double h = std::max(1e-6, top - base);
  switch (profile) {
    case ArchProfile::Round: {
      // A semicircle of radius a: a taller box adds straight jambs, a lower one squashes it.
      const double ry = std::min(a, h);
      const double y0 = top - ry;
      for (int i = 0; i <= segments; ++i) {
        const double t = kPi * (1.0 - double(i) / segments);
        out.push_back({cx + a * std::cos(t), y0 + ry * std::sin(t)});
      }
      break;
    }
    case ArchProfile::Elliptical:
      for (int i = 0; i <= segments; ++i) {
        const double t = kPi * (1.0 - double(i) / segments);
        out.push_back({cx + a * std::cos(t), base + h * std::sin(t)});
      }
      break;
    case ArchProfile::Segmental: {
      const double hs = std::max(1e-4, h * std::clamp(rise, 0.05, 1.0));
      const double r = (a * a + hs * hs) / (2.0 * hs);
      const double cy = top - r; // centre below the apex
      const double half = std::asin(std::clamp(a / r, -1.0, 1.0));
      // Arc from the left springing (angle pi/2 + half) to the right one.
      for (int i = 0; i <= segments; ++i) {
        const double t = kPi * 0.5 + half - 2.0 * half * double(i) / segments;
        out.push_back({cx + r * std::cos(t), cy + r * std::sin(t)});
      }
      break;
    }
    case ArchProfile::Pointed:
    case ArchProfile::Tudor: {
      if (segments % 2)
        ++segments;
      const int half = segments / 2;
      std::vector<glm::dvec2> left;
      if (profile == ArchProfile::Pointed) {
        // Two circular arcs, each centred on the springing line through its springing point and the apex.
        const double c = (a * a + h * h) / (2.0 * a); // centre offset from the left springing
        const double r = c;
        const double tApex = std::atan2(h, a - c);
        for (int i = 0; i <= half; ++i) {
          const double t = kPi + (tApex - kPi) * double(i) / half;
          left.push_back({x0 + c + r * std::cos(t), base + r * std::sin(t)});
        }
      } else {
        // Four-centred feel: vertical at the springing, a shallow point on top.
        for (int i = 0; i <= half; ++i) {
          const double u = double(i) / half;
          const double y = h * std::sqrt(std::max(0.0, 1.0 - (1.0 - u) * (1.0 - u))) * (0.72 + 0.28 * u);
          left.push_back({x0 + a * u, base + y});
        }
        left.back().y = top;
      }
      out = left;
      for (int i = half - 1; i >= 0; --i)
        out.push_back({2.0 * cx - left[size_t(i)].x, left[size_t(i)].y});
      break;
    }
  }
  out.front().x = x0;
  out.back().x = x1;
  return out;
}

std::vector<ConvexBrush> buildArch(const glm::dvec3 &mn, const glm::dvec3 &mx, const int spanAxis,
                                   const ArchParams &prm, const MaterialPtr &mat, const double tpu) {
  std::vector<ConvexBrush> out;
  Frame f;
  f.span = spanAxis == 2 ? 2 : 0;
  f.depth = f.span == 0 ? 2 : 0;
  f.d0 = mn[f.depth];
  f.d1 = mx[f.depth];
  const double L = mn[f.span], R = mx[f.span];
  const double B = mn.y, T = mx.y;
  const double W = R - L, H = T - B;
  if (W <= 1e-6 || H <= 1e-6 || f.d1 - f.d0 <= 1e-6)
    return out;
  auto push = [&out](std::optional<ConvexBrush> b) {
    if (b)
      out.push_back(std::move(*b));
  };
  const int n = std::clamp(prm.segments, 2, 64);

  switch (prm.mode) {
    case ArchMode::Opening: {
      const double spring = std::clamp(prm.spring, 0.0, H * 0.95);
      const auto c = archCurve(prm.profile, L, R, B + spring, T, n, prm.rise);
      // The curve may start above the spring line.
      const double floorOfArc = std::min(c.front().y, c.back().y);
      for (size_t i = 0; i + 1 < c.size(); ++i)
        push(prism(f, {c[i], c[i + 1], {c[i + 1].x, floorOfArc}, {c[i].x, floorOfArc}}, mat, tpu));
      if (floorOfArc > B + 1e-6)
        push(prism(f, {{L, B}, {R, B}, {R, floorOfArc}, {L, floorOfArc}}, mat, tpu));
      break;
    }
    case ArchMode::Archway: {
      // Jamb / crown 0: no posts, the curve springs from the box's sides (a tunnel between two walls).
      const double t = std::clamp(prm.thickness, 0.0, std::min(W * 0.45, H * 0.9));
      const double spring = std::clamp(prm.spring, 0.0, H - t - 1e-3);
      const auto c = archCurve(prm.profile, L + t, R - t, B + spring, T - t, n, prm.rise);
      if (t > 1e-6) {
        push(prism(f, {{L, B}, {L + t, B}, {L + t, T}, {L, T}}, mat, tpu));
        push(prism(f, {{R - t, B}, {R, B}, {R, T}, {R - t, T}}, mat, tpu));
      }
      for (size_t i = 0; i + 1 < c.size(); ++i)
        push(prism(f, {c[i], c[i + 1], {c[i + 1].x, T}, {c[i].x, T}}, mat, tpu));
      break;
    }
    case ArchMode::Ring:
    default: {
      const double t = std::clamp(std::max(prm.thickness, 0.1), 1e-3, std::min(W * 0.45, H * 0.9));
      const double spring = std::clamp(prm.spring, 0.0, H - t - 1e-3);
      const auto inner = archCurve(prm.profile, L + t, R - t, B + spring, T - t, n, prm.rise);
      const auto outer = archCurve(prm.profile, L, R, B + spring, T, n, prm.rise);
      const size_t m = std::min(inner.size(), outer.size());
      for (size_t i = 0; i + 1 < m; ++i)
        push(prism(f, {inner[i], inner[i + 1], outer[i + 1], outer[i]}, mat, tpu));
      const double postTop = std::min(inner.front().y, outer.front().y);
      if (postTop > B + 1e-6) {
        push(prism(f, {{L, B}, {L + t, B}, {L + t, postTop}, {L, postTop}}, mat, tpu));
        push(prism(f, {{R - t, B}, {R, B}, {R, postTop}, {R - t, postTop}}, mat, tpu));
      }
      break;
    }
  }
  return out;
}

QJsonObject archToJson(const ArchPlacement &place, const ArchParams &params) {
  auto vec = [](const glm::dvec3 &v) { return QJsonArray {v.x, v.y, v.z}; };
  return {{QStringLiteral("min"), vec(place.mn)},
          {QStringLiteral("max"), vec(place.mx)},
          {QStringLiteral("spanAxis"), place.spanAxis},
          {QStringLiteral("mode"), int(params.mode)},
          {QStringLiteral("profile"), int(params.profile)},
          {QStringLiteral("segments"), params.segments},
          {QStringLiteral("thickness"), params.thickness},
          {QStringLiteral("spring"), params.spring},
          {QStringLiteral("rise"), params.rise},
          {QStringLiteral("smooth"), params.smooth}};
}

void archFromJson(const QJsonObject &o, ArchPlacement &place, ArchParams &params) {
  auto vec = [](const QJsonValue &v, const glm::dvec3 &fallback) {
    const QJsonArray a = v.toArray();
    return a.size() == 3 ? glm::dvec3(a[0].toDouble(), a[1].toDouble(), a[2].toDouble()) : fallback;
  };
  place.mn = vec(o.value(QStringLiteral("min")), place.mn);
  place.mx = vec(o.value(QStringLiteral("max")), place.mx);
  place.spanAxis = o.value(QStringLiteral("spanAxis")).toInt(place.spanAxis) == 2 ? 2 : 0;
  // Carving is a one-off edit, never a generated arch.
  params.mode = std::clamp(o.value(QStringLiteral("mode")).toInt(int(params.mode)), 0, 1) == 0 ? ArchMode::Ring
                                                                                                 : ArchMode::Archway;
  params.profile = ArchProfile(std::clamp(o.value(QStringLiteral("profile")).toInt(int(params.profile)), 0, 4));
  params.segments = std::clamp(o.value(QStringLiteral("segments")).toInt(params.segments), 2, 64);
  params.thickness = std::max(0.0, o.value(QStringLiteral("thickness")).toDouble(params.thickness));
  params.spring = std::max(0.0, o.value(QStringLiteral("spring")).toDouble(params.spring));
  params.rise = std::clamp(o.value(QStringLiteral("rise")).toDouble(params.rise), 0.05, 1.0);
  params.smooth = o.value(QStringLiteral("smooth")).toBool(params.smooth);
}

} // namespace eng
