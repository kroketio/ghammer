#include "path_curve.h"

#include <algorithm>
#include <cmath>

#include <QJsonArray>

namespace eng::path {

namespace {

constexpr double kAutoHandle = 0.390; // Blender's auto handle length: 1 / 2.5614

glm::dvec3 safeNormalize(const glm::dvec3 &v, const glm::dvec3 &fallback) {
  const double l = glm::length(v);
  return l > 1e-12 ? v / l : fallback;
}

// Gauss-Legendre, 5 points on [-1, 1].
constexpr double kGlX[5] = {0.0, -0.5384693101056831, 0.5384693101056831, -0.9061798459386640, 0.9061798459386640};
constexpr double kGlW[5] = {0.5688888888888889, 0.4786286704993665, 0.4786286704993665, 0.2369268850561891,
                            0.2369268850561891};

QJsonArray vec(const glm::dvec3 &v) { return {v.x, v.y, v.z}; }
glm::dvec3 vec(const QJsonValue &v, const glm::dvec3 &def = glm::dvec3(0.0)) {
  const QJsonArray a = v.toArray();
  return a.size() == 3 ? glm::dvec3(a[0].toDouble(), a[1].toDouble(), a[2].toDouble()) : def;
}

} // namespace

void resolveHandles(std::vector<Point> &pts, const bool closed) {
  const int n = int(pts.size());
  if (n < 2) {
    for (Point &p: pts)
      if (p.type == HandleType::Auto || p.type == HandleType::Vector)
        p.in = p.out = glm::dvec3(0.0);
    return;
  }
  for (int i = 0; i < n; ++i) {
    Point &p = pts[size_t(i)];
    if (p.type != HandleType::Auto && p.type != HandleType::Vector)
      continue;
    const bool hasPrev = closed || i > 0, hasNext = closed || i < n - 1;
    const glm::dvec3 prev = hasPrev ? pts[size_t((i - 1 + n) % n)].pos : glm::dvec3(0.0);
    const glm::dvec3 next = hasNext ? pts[size_t((i + 1) % n)].pos : glm::dvec3(0.0);
    if (p.type == HandleType::Vector) {
      p.in = hasPrev ? (prev - p.pos) / 3.0 : glm::dvec3(0.0);
      p.out = hasNext ? (next - p.pos) / 3.0 : glm::dvec3(0.0);
      if (!hasPrev)
        p.in = -p.out;
      if (!hasNext)
        p.out = -p.in;
      continue;
    }
    // Auto: the tangent bisects the directions to the neighbours (an open end mirrors its only neighbour).
    const glm::dvec3 a = hasPrev ? p.pos - prev : next - p.pos;
    const glm::dvec3 b = hasNext ? next - p.pos : p.pos - prev;
    const double la = glm::length(a), lb = glm::length(b);
    if (la < 1e-12 || lb < 1e-12) {
      p.in = p.out = glm::dvec3(0.0);
      continue;
    }
    const glm::dvec3 t = safeNormalize(a / la + b / lb, b / lb);
    p.in = -t * la * kAutoHandle;
    p.out = t * lb * kAutoHandle;
  }
}

void dragHandle(std::vector<Point> &pts, const bool closed, const int i, const bool outHandle,
                const glm::dvec3 &offset, const bool forceFree) {
  if (i < 0 || i >= int(pts.size()))
    return;
  Point &p = pts[size_t(i)];
  if (forceFree || p.type == HandleType::Vector)
    p.type = HandleType::Free;
  else if (p.type == HandleType::Auto)
    p.type = HandleType::Aligned;
  glm::dvec3 &moved = outHandle ? p.out : p.in;
  glm::dvec3 &other = outHandle ? p.in : p.out;
  moved = offset;
  if (p.type == HandleType::Aligned) {
    const double l = glm::length(offset);
    double ol = glm::length(other);
    if (ol < 1e-9)
      ol = l;
    if (l > 1e-12)
      other = -offset / l * ol;
  }
  resolveHandles(pts, closed);
}

void insertPoint(std::vector<Point> &pts, const bool closed, const int segment, double t) {
  const int n = int(pts.size());
  const int segs = closed ? n : n - 1;
  if (n < 2 || segment < 0 || segment >= segs)
    return;
  t = std::clamp(t, 0.0, 1.0);
  const int j = (segment + 1) % n;
  Point &a = pts[size_t(segment)];
  Point &b = pts[size_t(j)];
  const glm::dvec3 p0 = a.pos, p1 = a.pos + a.out, p2 = b.pos + b.in, p3 = b.pos;
  const glm::dvec3 q0 = glm::mix(p0, p1, t), q1 = glm::mix(p1, p2, t), q2 = glm::mix(p2, p3, t);
  const glm::dvec3 r0 = glm::mix(q0, q1, t), r1 = glm::mix(q1, q2, t);
  const glm::dvec3 m = glm::mix(r0, r1, t);
  // The neighbours' handles get shortened; an Auto one would be recomputed and bend the curve, so it becomes
  // Aligned.
  auto keep = [](Point &p) {
    if (p.type == HandleType::Auto)
      p.type = HandleType::Aligned;
    else if (p.type == HandleType::Vector)
      p.type = HandleType::Free;
  };
  keep(a);
  keep(b);
  a.out = q0 - p0;
  b.in = q2 - p3;
  Point mid;
  mid.pos = m;
  mid.in = r0 - m;
  mid.out = r1 - m;
  mid.type = HandleType::Aligned;
  pts.insert(pts.begin() + (segment + 1), mid);
  resolveHandles(pts, closed);
}

QJsonObject toJson(const Path &p) {
  QJsonObject o;
  o.insert(QStringLiteral("id"), double(p.id));
  o.insert(QStringLiteral("name"), p.name);
  o.insert(QStringLiteral("kind"), p.kind == Path::Kind::Connect ? QStringLiteral("connect") : QStringLiteral("drawn"));
  o.insert(QStringLiteral("closed"), p.closed);
  if (p.hidden)
    o.insert(QStringLiteral("hidden"), true);
  if (p.locked)
    o.insert(QStringLiteral("locked"), true);
  QJsonArray pts;
  for (const Point &q: p.points)
    pts.append(QJsonObject {{QStringLiteral("p"), vec(q.pos)},
                            {QStringLiteral("i"), vec(q.in)},
                            {QStringLiteral("o"), vec(q.out)},
                            {QStringLiteral("t"), int(q.type)}});
  o.insert(QStringLiteral("points"), pts);
  const ConnectParams &c = p.connect;
  o.insert(QStringLiteral("connect"),
           QJsonObject {{QStringLiteral("a"), double(c.anchorA)},
                        {QStringLiteral("b"), double(c.anchorB)},
                        {QStringLiteral("sharpness"), c.sharpness},
                        {QStringLiteral("position"), c.position},
                        {QStringLiteral("straight"), c.straightEnds},
                        {QStringLiteral("tensionA"), c.tensionA},
                        {QStringLiteral("tensionB"), c.tensionB},
                        {QStringLiteral("height"), c.heightMode},
                        {QStringLiteral("maxSlope"), c.maxSlopeDeg},
                        {QStringLiteral("minRadius"), c.minRadius}});
  return o;
}

Path fromJson(const QJsonObject &o) {
  Path p;
  p.id = quint32(o.value(QStringLiteral("id")).toDouble());
  p.name = o.value(QStringLiteral("name")).toString();
  p.kind = o.value(QStringLiteral("kind")).toString() == QLatin1String("connect") ? Path::Kind::Connect
                                                                                  : Path::Kind::Drawn;
  p.closed = o.value(QStringLiteral("closed")).toBool();
  p.hidden = o.value(QStringLiteral("hidden")).toBool();
  p.locked = o.value(QStringLiteral("locked")).toBool();
  for (const QJsonValue &v: o.value(QStringLiteral("points")).toArray()) {
    const QJsonObject q = v.toObject();
    Point pt;
    pt.pos = vec(q.value(QStringLiteral("p")));
    pt.in = vec(q.value(QStringLiteral("i")));
    pt.out = vec(q.value(QStringLiteral("o")));
    pt.type = HandleType(std::clamp(q.value(QStringLiteral("t")).toInt(), 0, 3));
    p.points.push_back(pt);
  }
  const QJsonObject c = o.value(QStringLiteral("connect")).toObject();
  ConnectParams &cp = p.connect;
  cp.anchorA = quint32(c.value(QStringLiteral("a")).toDouble());
  cp.anchorB = quint32(c.value(QStringLiteral("b")).toDouble());
  cp.sharpness = c.value(QStringLiteral("sharpness")).toDouble(cp.sharpness);
  cp.position = c.value(QStringLiteral("position")).toDouble(cp.position);
  cp.straightEnds = c.value(QStringLiteral("straight")).toDouble(cp.straightEnds);
  cp.tensionA = c.value(QStringLiteral("tensionA")).toDouble(cp.tensionA);
  cp.tensionB = c.value(QStringLiteral("tensionB")).toDouble(cp.tensionB);
  cp.heightMode = std::clamp(c.value(QStringLiteral("height")).toInt(cp.heightMode), 0, 2);
  cp.maxSlopeDeg = c.value(QStringLiteral("maxSlope")).toDouble(cp.maxSlopeDeg);
  cp.minRadius = c.value(QStringLiteral("minRadius")).toDouble(cp.minRadius);
  return p;
}

glm::dvec3 Curve::bez(const Segment &g, const double t) {
  const double u = 1.0 - t;
  return u * u * u * g.p0 + 3.0 * u * u * t * g.p1 + 3.0 * u * t * t * g.p2 + t * t * t * g.p3;
}

glm::dvec3 Curve::bezD(const Segment &g, const double t) {
  const double u = 1.0 - t;
  return 3.0 * u * u * (g.p1 - g.p0) + 6.0 * u * t * (g.p2 - g.p1) + 3.0 * t * t * (g.p3 - g.p2);
}

glm::dvec3 Curve::bezDD(const Segment &g, const double t) {
  return 6.0 * (1.0 - t) * (g.p2 - 2.0 * g.p1 + g.p0) + 6.0 * t * (g.p3 - 2.0 * g.p2 + g.p1);
}

double Curve::lengthBetween(const Segment &g, const double t0, const double t1) {
  const double half = (t1 - t0) * 0.5, mid = (t0 + t1) * 0.5;
  double sum = 0.0;
  for (int i = 0; i < 5; ++i)
    sum += kGlW[i] * glm::length(bezD(g, mid + half * kGlX[i]));
  return sum * half;
}

std::shared_ptr<const Curve> Curve::build(const std::vector<Point> &points, const bool closed) {
  auto c = std::make_shared<Curve>();
  c->m_closed = closed && points.size() >= 3;
  const int n = int(points.size());
  const int segs = n < 2 ? 0 : (c->m_closed ? n : n - 1);
  double s = 0.0;
  for (int i = 0; i < segs; ++i) {
    const Point &a = points[size_t(i)];
    const Point &b = points[size_t((i + 1) % n)];
    Segment g;
    g.p0 = a.pos;
    g.p1 = a.pos + a.out;
    g.p2 = b.pos + b.in;
    g.p3 = b.pos;
    g.s0 = s;
    g.cum.resize(kSub + 1, 0.0);
    for (int k = 0; k < kSub; ++k)
      g.cum[size_t(k + 1)] = g.cum[size_t(k)] + lengthBetween(g, double(k) / kSub, double(k + 1) / kSub);
    g.len = g.cum.back();
    if (g.len < 1e-9)
      continue;
    s += g.len;
    c->m_segments.push_back(std::move(g));
  }
  c->m_length = s;
  constexpr int kDraw = 24;
  for (size_t i = 0; i < c->m_segments.size(); ++i) {
    const Segment &g = c->m_segments[i];
    for (int k = (i == 0 ? 0 : 1); k <= kDraw; ++k) {
      const double t = double(k) / kDraw;
      c->m_poly.push_back(bez(g, t));
      const int sub = std::min(kSub - 1, int(t * kSub));
      c->m_polyS.push_back(g.s0 + g.cum[size_t(sub)] + lengthBetween(g, double(sub) / kSub, t));
    }
  }
  return c;
}

double Curve::tAt(const Segment &g, const double local) const {
  if (local <= 0.0)
    return 0.0;
  if (local >= g.len)
    return 1.0;
  const auto it = std::upper_bound(g.cum.begin(), g.cum.end(), local);
  const int k = std::clamp(int(it - g.cum.begin()) - 1, 0, kSub - 1);
  const double c0 = g.cum[size_t(k)], c1 = g.cum[size_t(k + 1)];
  const double ta = double(k) / kSub, tb = double(k + 1) / kSub;
  double t = ta + (tb - ta) * ((local - c0) / std::max(1e-15, c1 - c0));
  for (int it2 = 0; it2 < 4; ++it2) {
    const double f = c0 + lengthBetween(g, ta, t) - local;
    const double d = glm::length(bezD(g, t));
    if (d < 1e-12)
      break;
    t = std::clamp(t - f / d, ta, tb);
    if (std::fabs(f) < 1e-10)
      break;
  }
  return t;
}

double Curve::wrap(double s) const {
  if (!m_closed || m_length <= 0.0)
    return s;
  s = std::fmod(s, m_length);
  return s < 0.0 ? s + m_length : s;
}

const Curve::Segment &Curve::segmentFor(double s, double &local) const {
  size_t lo = 0, hi = m_segments.size();
  while (hi - lo > 1) {
    const size_t mid = (lo + hi) / 2;
    if (m_segments[mid].s0 <= s)
      lo = mid;
    else
      hi = mid;
  }
  local = s - m_segments[lo].s0;
  return m_segments[lo];
}

glm::dvec3 Curve::position(double s) const {
  if (m_segments.empty())
    return glm::dvec3(0.0);
  s = wrap(s);
  if (!m_closed) {
    if (s < 0.0)
      return bez(m_segments.front(), 0.0) + tangent(0.0) * s;
    if (s > m_length)
      return bez(m_segments.back(), 1.0) + tangent(m_length) * (s - m_length);
  }
  double local = 0.0;
  const Segment &g = segmentFor(s, local);
  return bez(g, tAt(g, local));
}

glm::dvec3 Curve::tangent(double s) const {
  if (m_segments.empty())
    return {0.0, 0.0, 1.0};
  s = m_closed ? wrap(s) : std::clamp(s, 0.0, m_length);
  double local = 0.0;
  const Segment &g = segmentFor(s, local);
  const double t = tAt(g, local);
  glm::dvec3 d = bezD(g, t);
  if (glm::length(d) < 1e-9)
    d = bezDD(g, t) * (t < 0.5 ? 1.0 : -1.0);
  if (glm::length(d) < 1e-9)
    d = bez(g, std::min(1.0, t + 1e-4)) - bez(g, std::max(0.0, t - 1e-4));
  return safeNormalize(d, {0.0, 0.0, 1.0});
}

double Curve::curvature(double s) const {
  if (m_segments.empty() || (!m_closed && (s < 0.0 || s > m_length)))
    return 0.0;
  s = wrap(s);
  double local = 0.0;
  const Segment &g = segmentFor(s, local);
  const double t = tAt(g, local);
  const glm::dvec3 d1 = bezD(g, t), d2 = bezDD(g, t);
  const double l = glm::length(d1);
  return l < 1e-9 ? 0.0 : glm::length(glm::cross(d1, d2)) / (l * l * l);
}

double Curve::horizontalCurvature(double s) const {
  if (m_segments.empty() || (!m_closed && (s < 0.0 || s > m_length)))
    return 0.0;
  s = wrap(s);
  double local = 0.0;
  const Segment &g = segmentFor(s, local);
  const double t = tAt(g, local);
  const glm::dvec3 d1 = bezD(g, t), d2 = bezDD(g, t);
  const double h2 = d1.x * d1.x + d1.z * d1.z, l = glm::length(d1);
  if (h2 < 1e-12 || l < 1e-9)
    return 0.0;
  // d(heading)/ds, heading measured in the XZ plane.
  return (d1.z * d2.x - d1.x * d2.z) / (h2 * l);
}

double Curve::sAt(const int segment, const double t) const {
  if (segment < 0 || segment >= int(m_segments.size()))
    return segment < 0 ? 0.0 : m_length;
  const Segment &g = m_segments[size_t(segment)];
  const double tc = std::clamp(t, 0.0, 1.0);
  const int k = std::min(kSub - 1, int(tc * kSub));
  return g.s0 + g.cum[size_t(k)] + lengthBetween(g, double(k) / kSub, tc);
}

void Curve::segmentAt(double s, int &segment, double &t) const {
  segment = 0;
  t = 0.0;
  if (m_segments.empty())
    return;
  s = m_closed ? wrap(s) : std::clamp(s, 0.0, m_length);
  double local = 0.0;
  const Segment &g = segmentFor(s, local);
  segment = int(&g - m_segments.data());
  t = tAt(g, local);
}

double Curve::nearest2D(const glm::dvec2 &p, const glm::dvec3 &right, const glm::dvec3 &up, double &distance) const {
  distance = 1e300;
  double best = 0.0;
  for (size_t i = 0; i + 1 < m_poly.size(); ++i) {
    const glm::dvec2 a(glm::dot(m_poly[i], right), glm::dot(m_poly[i], up));
    const glm::dvec2 b(glm::dot(m_poly[i + 1], right), glm::dot(m_poly[i + 1], up));
    const glm::dvec2 ab = b - a;
    const double l2 = glm::dot(ab, ab);
    const double u = l2 > 1e-18 ? std::clamp(glm::dot(p - a, ab) / l2, 0.0, 1.0) : 0.0;
    const double d = glm::length(p - (a + ab * u));
    if (d < distance) {
      distance = d;
      best = m_polyS[i] + (m_polyS[i + 1] - m_polyS[i]) * u;
    }
  }
  return best;
}

double Curve::minRadius(double &atS) const {
  double kMax = 0.0;
  atS = 0.0;
  for (const double s: m_polyS) {
    const double k = curvature(s);
    if (k > kMax) {
      kMax = k;
      atS = s;
    }
  }
  return kMax > 1e-12 ? 1.0 / kMax : 1e300;
}

namespace {

double cross2(const glm::dvec2 &a, const glm::dvec2 &b) { return a.x * b.y - a.y * b.x; }
glm::dvec3 to3(const glm::dvec2 &v) { return {v.x, 0.0, v.y}; }
double smoothstep01(const double x) {
  const double t = std::clamp(x, 0.0, 1.0);
  return t * t * (3.0 - 2.0 * t);
}
double smoothstepD(const double x) { // derivative w.r.t. x
  const double t = std::clamp(x, 0.0, 1.0);
  return (x <= 0.0 || x >= 1.0) ? 0.0 : 6.0 * t * (1.0 - t);
}

Point planPoint(const glm::dvec2 &pos, const glm::dvec2 &in, const glm::dvec2 &out) {
  Point p;
  p.pos = to3(pos);
  p.in = to3(in);
  p.out = to3(out);
  p.type = HandleType::Free;
  return p;
}

// Straight run to `to`: handles a third of the way.
void straightTo(std::vector<Point> &pts, const glm::dvec2 &to) {
  const glm::dvec2 from(pts.back().pos.x, pts.back().pos.z);
  const glm::dvec2 d = to - from;
  if (glm::length(d) < 1e-6)
    return;
  pts.back().out = to3(d / 3.0);
  pts.push_back(planPoint(to, -d / 3.0, glm::dvec2(0.0)));
}

} // namespace

ConnectResult buildConnect(const ConnectEnd &a, const ConnectEnd &b, const ConnectParams &p, const double autoStraight,
                           const double autoRadius) {
  ConnectResult r;
  const glm::dvec2 A(a.pos.x, a.pos.z), B(b.pos.x, b.pos.z);
  glm::dvec2 dA(a.facing.x, a.facing.z), fB(b.facing.x, b.facing.z);
  if (glm::length(dA) < 1e-6 || glm::length(fB) < 1e-6) {
    r.error = QStringLiteral("An anchor faces straight up or down");
    return r;
  }
  dA = glm::normalize(dA);
  const glm::dvec2 dB = -glm::normalize(fB); // direction of travel arriving at B
  const glm::dvec2 D = B - A;
  const double dist = glm::length(D);
  if (dist < 1e-3) {
    r.error = QStringLiteral("The anchors are at the same spot");
    return r;
  }
  const double straight = std::max(0.0, p.straightEnds >= 0.0 ? p.straightEnds : autoStraight);
  const double R = std::max(0.0, p.minRadius >= 0.0 ? p.minRadius : autoRadius);
  const double sigma = std::clamp(p.sharpness, 0.0, 1.0);
  const double beta = std::clamp(p.position, -1.0, 1.0);
  const double cosTurn = glm::dot(dA, dB);
  const double side = cross2(dA, D), along = glm::dot(dA, D);
  constexpr double kParallel = 0.99619469809; // cos 5°
  auto units = [](double w) { return QString::number(std::ceil(w * 10.0)); }; // world -> Source units

  std::vector<Point> pts;
  pts.push_back(planPoint(A, glm::dvec2(0.0), glm::dvec2(0.0)));
  int turnI0 = 0, turnI1 = 0; // point indices bounding the turn

  if (cosTurn > kParallel) {
    // S: parallel, offset sideways by `side`.
    if (along <= 1e-6) {
      r.error = QStringLiteral("B is behind A");
      return r;
    }
    const double h = std::fabs(side);
    if (h < 1e-3) {
      straightTo(pts, B);
      turnI0 = 0;
      turnI1 = int(pts.size()) - 1;
    } else {
      const double aMin = std::sqrt(6.0 * h * R);
      if (along < aMin + 2.0 * straight) {
        r.error = QStringLiteral("Not enough room for the S-bend: needs %1 more units")
                      .arg(units(aMin + 2.0 * straight - along));
        return r;
      }
      const double aS = aMin + (along - 2.0 * straight - aMin) * (1.0 - sigma);
      const double leads = along - aS;
      const double leadA = straight + (leads - 2.0 * straight) * (1.0 + beta) * 0.5;
      const glm::dvec2 s0 = A + dA * leadA;
      const glm::dvec2 s1 = s0 + dA * aS + glm::dvec2(-dA.y, dA.x) * side;
      straightTo(pts, s0);
      turnI0 = int(pts.size()) - 1;
      pts.back().out = to3(dA * (aS / 3.0));
      pts.push_back(planPoint(s1, -dA * (aS / 3.0), glm::dvec2(0.0)));
      turnI1 = int(pts.size()) - 1;
      straightTo(pts, B);
      r.radius = aS * aS / (6.0 * h);
    }
  } else if (cosTurn < -kParallel) {
    // U: opposite directions; a half circle of radius |side| / 2.
    const double radius = std::fabs(side) * 0.5;
    if (radius < R || radius < 1e-3) {
      r.error = QStringLiteral("For a U-turn the anchors must be %1 units apart sideways").arg(units(2.0 * R));
      return r;
    }
    const double legA = std::max(0.0, along) + straight + (1.0 - sigma) * radius;
    const double legB = legA - along;
    if (legB < straight - 1e-6) {
      r.error = QStringLiteral("B is too far ahead for a U-turn");
      return r;
    }
    const glm::dvec2 n = glm::dvec2(-dA.y, dA.x) * (side >= 0.0 ? 1.0 : -1.0);
    const double k = 0.5522847498 * radius;
    const glm::dvec2 t0 = A + dA * legA;
    const glm::dvec2 apex = t0 + dA * radius + n * radius;
    const glm::dvec2 t1 = t0 + n * (2.0 * radius);
    straightTo(pts, t0);
    turnI0 = int(pts.size()) - 1;
    pts.back().out = to3(dA * k);
    pts.push_back(planPoint(apex, -n * k, n * k));
    pts.push_back(planPoint(t1, dA * k, glm::dvec2(0.0)));
    turnI1 = int(pts.size()) - 1;
    straightTo(pts, B);
    r.radius = radius;
  } else {
    // Corner: the rays meet at X = A + u dA = B - v dB.
    const double det = cross2(dA, dB);
    const double u = cross2(D, dB) / det, v = cross2(dA, D) / det;
    if (u <= 0.0 || v <= 0.0) {
      if (glm::dot(dA, D) <= 0.0 && glm::dot(dB, D) <= 0.0) {
        r.error = QStringLiteral("The anchors face away from each other");
        return r;
      }
      // The rays meet behind an anchor: a free curve, sized by the distance.
      const double k = (0.6 + (0.3 - 0.6) * sigma) * dist;
      pts.back().out = to3(dA * k * p.tensionA);
      pts.push_back(planPoint(B, -dB * k * p.tensionB, glm::dvec2(0.0)));
      turnI0 = 0;
      turnI1 = 1;
      r.warning = QStringLiteral("The anchors do not face a common corner");
    } else {
      const double theta = std::acos(std::clamp(cosTurn, -1.0, 1.0));
      const double tMax = std::min(u, v) - straight;
      const double tMin = R * std::tan(theta * 0.5);
      if (tMax < tMin) {
        r.error = QStringLiteral("The turn is too tight: needs %1 more units of room").arg(units(tMin - tMax));
        return r;
      }
      const double t = tMax + (tMin - tMax) * sigma;
      const glm::dvec2 X = A + dA * u;
      const double sign = det >= 0.0 ? 1.0 : -1.0;
      if (theta <= M_PI * 0.5 + 1e-9) {
        const double tA = std::clamp(t * (1.0 - beta * 0.5), tMin * 0.5, u - straight);
        const double tB = std::clamp(t * (1.0 + beta * 0.5), tMin * 0.5, v - straight);
        const double rho = (4.0 / 3.0) * std::tan(theta * 0.25) / std::tan(theta * 0.5);
        const glm::dvec2 ps = X - dA * tA, pe = X + dB * tB;
        straightTo(pts, ps);
        turnI0 = int(pts.size()) - 1;
        pts.back().out = to3(dA * (rho * tA * p.tensionA));
        pts.push_back(planPoint(pe, -dB * (rho * tB * p.tensionB), glm::dvec2(0.0)));
        turnI1 = int(pts.size()) - 1;
        straightTo(pts, B);
        r.radius = t / std::tan(theta * 0.5);
      } else {
        // Wider than 90°: two circular halves (symmetric).
        const double radius = t / std::tan(theta * 0.5);
        const glm::dvec2 ps = X - dA * t, pe = X + dB * t;
        const glm::dvec2 c = ps + glm::dvec2(-dA.y, dA.x) * (sign * radius);
        const glm::dvec2 m = c + glm::normalize(X - c) * radius;
        const glm::dvec2 tm = glm::normalize(dA + dB);
        const double hl = (4.0 / 3.0) * std::tan(theta / 8.0) * radius;
        straightTo(pts, ps);
        turnI0 = int(pts.size()) - 1;
        pts.back().out = to3(dA * hl);
        pts.push_back(planPoint(m, -tm * hl, tm * hl));
        pts.push_back(planPoint(pe, -dB * hl, glm::dvec2(0.0)));
        turnI1 = int(pts.size()) - 1;
        straightTo(pts, B);
        r.radius = radius;
      }
    }
  }
  // The last point continues straight on (out handle unused).
  pts.back().out = -pts.back().in;

  // Heights.
  const double dy = b.pos.y - a.pos.y;
  auto planCurve = Curve::build(pts, false);
  auto sOf = [&](int i) { return i >= int(pts.size()) - 1 ? planCurve->length() : planCurve->sAt(i, 0.0); };
  const double total = planCurve->length();
  double h0 = 0.0, h1 = total;
  if (p.heightMode == 0) {
    h0 = sOf(turnI0);
    h1 = sOf(turnI1);
  }
  const double maxTan = std::tan(glm::radians(std::clamp(p.maxSlopeDeg, 1.0, 85.0)));
  if (std::fabs(dy) > 1e-6) {
    const double factor = p.heightMode == 2 ? 1.0 : 1.5; // the steepest point of a smoothstep
    const double needed = factor * std::fabs(dy) / maxTan;
    if (needed > total + 1e-6) {
      r.error = QStringLiteral("The height difference needs %1 more units of length").arg(units(needed - total));
      return r;
    }
    if (h1 - h0 < needed) { // widen into the leads
      const double mid = (h0 + h1) * 0.5;
      h0 = std::clamp(mid - needed * 0.5, 0.0, total - needed);
      h1 = h0 + needed;
    }
    // Split the leads where the climb starts / ends, so the height change sits on its own segments.
    for (const double cut: {h0, h1}) {
      int seg = 0;
      double t = 0.0;
      planCurve->segmentAt(cut, seg, t);
      if (t > 1e-4 && t < 1.0 - 1e-4) {
        insertPoint(pts, false, seg, t);
        pts[size_t(seg + 1)].type = HandleType::Free;
        planCurve = Curve::build(pts, false);
      }
    }
  }
  const double span = std::max(1e-9, h1 - h0);
  auto height = [&](double s) {
    const double x = (s - h0) / span;
    return a.pos.y + dy * (p.heightMode == 2 ? std::clamp(x, 0.0, 1.0) : smoothstep01(x));
  };
  auto slope = [&](double s) {
    const double x = (s - h0) / span;
    return dy / span * (p.heightMode == 2 ? (x > 0.0 && x < 1.0 ? 1.0 : 0.0) : smoothstepD(x));
  };
  for (int i = 0; i < int(pts.size()); ++i) {
    const double s = i >= int(pts.size()) - 1 ? planCurve->length() : planCurve->sAt(i, 0.0);
    Point &q = pts[size_t(i)];
    q.pos.y = height(s);
    const double k = slope(s);
    q.in.y = -k * glm::length(glm::dvec2(q.in.x, q.in.z));
    q.out.y = k * glm::length(glm::dvec2(q.out.x, q.out.z));
  }
  // Check buildability: the final 3D curve must keep the radius.
  if (R > 1e-6) {
    const auto curve = Curve::build(pts, false);
    double at = 0.0;
    const double minR = curve->minRadius(at);
    if (minR < R * 0.95) {
      r.error = QStringLiteral("The turn is too tight here: radius %1 < %2 units").arg(units(minR)).arg(units(R));
      return r;
    }
  }
  r.points = std::move(pts);
  r.ok = true;
  return r;
}

} // namespace eng::path
