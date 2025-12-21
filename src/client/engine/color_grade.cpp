#include "color_grade.h"

#include <QJsonArray>
#include <QJsonValue>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <optional>

namespace eng {

// Basic

bool LutAdjustments::isNeutral() const {
  const auto grey = [](const QColor &c) { return c.red() == c.green() && c.green() == c.blue(); };
  return exposure == 0.0 && contrast == 1.0 && saturation == 1.0 && vibrance == 0.0 && gamma == 1.0 && hue == 0.0
      && temperature == 0.0 && tint == 0.0 && (toningStrength == 0.0 || (grey(shadows) && grey(highlights)));
}

QJsonObject LutAdjustments::toJson() const {
  return {{"exposure", exposure},
          {"contrast", contrast},
          {"saturation", saturation},
          {"vibrance", vibrance},
          {"gamma", gamma},
          {"hue", hue},
          {"temperature", temperature},
          {"tint", tint},
          {"shadows", shadows.name(QColor::HexRgb)},
          {"highlights", highlights.name(QColor::HexRgb)},
          {"toningStrength", toningStrength}};
}

LutAdjustments LutAdjustments::fromJson(const QJsonObject &o) {
  LutAdjustments a;
  const auto num = [&o](const char *key, double &v) {
    if (o.value(QLatin1String(key)).isDouble())
      v = o.value(QLatin1String(key)).toDouble();
  };
  const auto col = [&o](const char *key, QColor &c) {
    const QColor parsed = QColor::fromString(o.value(QLatin1String(key)).toString());
    if (parsed.isValid())
      c = parsed;
  };
  num("exposure", a.exposure);
  num("contrast", a.contrast);
  num("saturation", a.saturation);
  num("vibrance", a.vibrance);
  num("gamma", a.gamma);
  num("hue", a.hue);
  num("temperature", a.temperature);
  num("tint", a.tint);
  col("shadows", a.shadows);
  col("highlights", a.highlights);
  num("toningStrength", a.toningStrength);
  return a;
}

// Levels / curves

double LevelsCurve::map(double x) const {
  if (x <= inBlack)
    return outBlack;
  if (x >= inWhite)
    return outWhite;
  const double t = (x - inBlack) / std::max(1e-9, inWhite - inBlack);
  return outBlack + (outWhite - outBlack) * std::pow(t, 1.0 / std::max(0.01, gamma));
}

CurvePoints identityCurve() {
  return {{0.0, 0.0}, {1.0, 1.0}};
}

bool isIdentityCurve(const CurvePoints &pts) {
  return pts == identityCurve();
}

namespace {

// Second derivatives of the natural cubic spline (zero at both ends).
std::vector<double> splineSecond(const CurvePoints &p) {
  const size_t n = p.size();
  std::vector<double> m(n, 0.0);
  if (n < 3)
    return m;
  std::vector<double> c(n, 0.0), d(n, 0.0);
  for (size_t i = 1; i + 1 < n; ++i) {
    const double h0 = p[i].x - p[i - 1].x, h1 = p[i + 1].x - p[i].x;
    const double a = h0, b = 2.0 * (h0 + h1), cc = h1;
    const double r = 6.0 * ((p[i + 1].y - p[i].y) / h1 - (p[i].y - p[i - 1].y) / h0);
    const double denom = b - a * c[i - 1];
    c[i] = cc / denom;
    d[i] = (r - a * d[i - 1]) / denom;
  }
  for (size_t i = n - 2; i >= 1; --i) {
    m[i] = d[i] - c[i] * m[i + 1];
    if (i == 1)
      break;
  }
  return m;
}

double splineAt(const CurvePoints &p, const std::vector<double> &m, double x) {
  if (p.empty())
    return x;
  if (p.size() == 1 || x <= p.front().x)
    return p.front().y;
  if (x >= p.back().x)
    return p.back().y;
  const auto it = std::upper_bound(p.begin(), p.end(), x, [](double v, const glm::dvec2 &q) { return v < q.x; });
  const size_t i = size_t(it - p.begin()) - 1;
  const double h = p[i + 1].x - p[i].x;
  if (h <= 1e-12)
    return p[i].y;
  const double a = (p[i + 1].x - x) / h, b = (x - p[i].x) / h;
  return a * p[i].y + b * p[i + 1].y + ((a * a * a - a) * m[i] + (b * b * b - b) * m[i + 1]) * h * h / 6.0;
}

} // namespace

double curveValue(const CurvePoints &pts, double x) {
  return std::clamp(splineAt(pts, splineSecond(pts), x), 0.0, 1.0);
}

// Steps

GradeStep GradeStep::make(Kind kind) {
  GradeStep s;
  s.kind = kind;
  return s;
}

const char *gradeKindKey(GradeStep::Kind kind) {
  switch (kind) {
  case GradeStep::Kind::Basic:
    return "basic";
  case GradeStep::Kind::Levels:
    return "levels";
  case GradeStep::Kind::Curves:
    return "curves";
  case GradeStep::Kind::ColorBalance:
    return "balance";
  case GradeStep::Kind::Hsv:
    return "hsv";
  case GradeStep::Kind::Cdl:
    return "cdl";
  case GradeStep::Kind::Count:
    break;
  }
  return "basic";
}

namespace {

QJsonArray vec3Json(const glm::dvec3 &v) {
  return {v.x, v.y, v.z};
}

glm::dvec3 vec3From(const QJsonValue &v, const glm::dvec3 &fallback) {
  const QJsonArray a = v.toArray();
  if (a.size() != 3)
    return fallback;
  return {a[0].toDouble(fallback.x), a[1].toDouble(fallback.y), a[2].toDouble(fallback.z)};
}

QJsonObject stepJson(const GradeStep &s) {
  QJsonObject o {{"kind", QLatin1String(gradeKindKey(s.kind))}, {"id", s.id}, {"enabled", s.enabled}};
  switch (s.kind) {
  case GradeStep::Kind::Basic:
    o.insert("basic", s.basic.toJson());
    break;
  case GradeStep::Kind::Levels: {
    QJsonArray chans;
    for (const LevelsCurve &c: s.levels.ch)
      chans.append(QJsonArray {c.inBlack, c.inWhite, c.gamma, c.outBlack, c.outWhite});
    o.insert("levels", chans);
    break;
  }
  case GradeStep::Kind::Curves: {
    QJsonArray chans;
    for (const CurvePoints &pts: s.curves.ch) {
      QJsonArray pa;
      for (const glm::dvec2 &p: pts)
        pa.append(QJsonArray {p.x, p.y});
      chans.append(pa);
    }
    o.insert("curves", chans);
    break;
  }
  case GradeStep::Kind::ColorBalance:
    o.insert("shadows", vec3Json(s.balance.shadows));
    o.insert("midtones", vec3Json(s.balance.midtones));
    o.insert("highlights", vec3Json(s.balance.highlights));
    o.insert("preserveLuminosity", s.balance.preserveLuminosity);
    break;
  case GradeStep::Kind::Hsv:
    o.insert("model", s.hsv.model);
    o.insert("hue", s.hsv.hue);
    o.insert("saturation", s.hsv.saturation);
    o.insert("value", s.hsv.value);
    o.insert("colorize", s.hsv.colorize);
    break;
  case GradeStep::Kind::Cdl:
    o.insert("slope", vec3Json(s.cdl.slope));
    o.insert("offset", vec3Json(s.cdl.offset));
    o.insert("power", vec3Json(s.cdl.power));
    o.insert("saturation", s.cdl.saturation);
    break;
  case GradeStep::Kind::Count:
    break;
  }
  return o;
}

std::optional<GradeStep> stepFrom(const QJsonObject &o) {
  const QString key = o.value("kind").toString();
  GradeStep s;
  bool found = false;
  for (int k = 0; k < int(GradeStep::Kind::Count); ++k)
    if (key == QLatin1String(gradeKindKey(GradeStep::Kind(k)))) {
      s.kind = GradeStep::Kind(k);
      found = true;
    }
  if (!found)
    return std::nullopt;
  s.id = o.value("id").toInt(0);
  s.enabled = o.value("enabled").toBool(true);
  switch (s.kind) {
  case GradeStep::Kind::Basic:
    s.basic = LutAdjustments::fromJson(o.value("basic").toObject());
    break;
  case GradeStep::Kind::Levels: {
    const QJsonArray chans = o.value("levels").toArray();
    for (int i = 0; i < kGradeChannels && i < chans.size(); ++i) {
      const QJsonArray v = chans[i].toArray();
      if (v.size() != 5)
        continue;
      s.levels.ch[size_t(i)] = {v[0].toDouble(0.0), v[1].toDouble(1.0), v[2].toDouble(1.0), v[3].toDouble(0.0),
                                v[4].toDouble(1.0)};
    }
    break;
  }
  case GradeStep::Kind::Curves: {
    const QJsonArray chans = o.value("curves").toArray();
    for (int i = 0; i < kGradeChannels && i < chans.size(); ++i) {
      CurvePoints pts;
      for (const QJsonValue &pv: chans[i].toArray()) {
        const QJsonArray p = pv.toArray();
        if (p.size() == 2)
          pts.push_back({std::clamp(p[0].toDouble(), 0.0, 1.0), std::clamp(p[1].toDouble(), 0.0, 1.0)});
      }
      std::sort(pts.begin(), pts.end(), [](const glm::dvec2 &a, const glm::dvec2 &b) { return a.x < b.x; });
      if (pts.size() >= 2)
        s.curves.ch[size_t(i)] = pts;
    }
    break;
  }
  case GradeStep::Kind::ColorBalance:
    s.balance.shadows = vec3From(o.value("shadows"), glm::dvec3(0.0));
    s.balance.midtones = vec3From(o.value("midtones"), glm::dvec3(0.0));
    s.balance.highlights = vec3From(o.value("highlights"), glm::dvec3(0.0));
    s.balance.preserveLuminosity = o.value("preserveLuminosity").toBool(true);
    break;
  case GradeStep::Kind::Hsv:
    s.hsv.model = std::clamp(o.value("model").toInt(1), 0, 3);
    s.hsv.hue = o.value("hue").toDouble(0.0);
    s.hsv.saturation = o.value("saturation").toDouble(0.0);
    s.hsv.value = o.value("value").toDouble(0.0);
    s.hsv.colorize = o.value("colorize").toBool(false);
    break;
  case GradeStep::Kind::Cdl:
    s.cdl.slope = vec3From(o.value("slope"), glm::dvec3(1.0));
    s.cdl.offset = vec3From(o.value("offset"), glm::dvec3(0.0));
    s.cdl.power = vec3From(o.value("power"), glm::dvec3(1.0));
    s.cdl.saturation = o.value("saturation").toDouble(1.0);
    break;
  case GradeStep::Kind::Count:
    break;
  }
  return s;
}

} // namespace

QJsonObject LutGrade::toJson() const {
  QJsonArray a;
  for (const GradeStep &s: steps)
    a.append(stepJson(s));
  return {{"steps", a}};
}

LutGrade LutGrade::fromJson(const QJsonObject &o) {
  if (!o.contains("steps"))
    return basic(LutAdjustments::fromJson(o));
  LutGrade g;
  for (const QJsonValue &v: o.value("steps").toArray())
    if (auto s = stepFrom(v.toObject()))
      g.steps.push_back(*s);
  // Missing or repeated ids get fresh ones.
  QSet<int> seen;
  for (GradeStep &s: g.steps) {
    if (s.id <= 0 || seen.contains(s.id))
      s.id = 0;
    else
      seen.insert(s.id);
  }
  for (GradeStep &s: g.steps)
    if (s.id == 0) {
      s.id = g.nextId();
      seen.insert(s.id);
    }
  return g;
}

int LutGrade::nextId() const {
  int id = 0;
  for (const GradeStep &s: steps)
    id = std::max(id, s.id);
  return id + 1;
}

int LutGrade::indexOf(int stepId) const {
  for (size_t i = 0; i < steps.size(); ++i)
    if (steps[i].id == stepId)
      return int(i);
  return -1;
}

LutGrade LutGrade::basic(const LutAdjustments &a) {
  LutGrade g;
  GradeStep s = GradeStep::make(GradeStep::Kind::Basic);
  s.id = 1;
  s.basic = a;
  g.steps.push_back(s);
  return g;
}

// Colour maths

namespace grade {
namespace {

using Rgb = glm::dvec3;

double clamp01(double v) {
  return std::clamp(v, 0.0, 1.0);
}

Rgb clamp01(const Rgb &c) {
  return glm::clamp(c, Rgb(0.0), Rgb(1.0));
}

double srgbToLinear(double c) {
  return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

double linearToSrgb(double c) {
  c = std::max(0.0, c);
  return c <= 0.0031308 ? c * 12.92 : 1.055 * std::pow(c, 1.0 / 2.4) - 0.055;
}

double luma(const Rgb &c) {
  return 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b;
}

// sRGB-encoded <-> CIE Lab (D65), L 0..100.
constexpr double kXn = 0.95047, kYn = 1.0, kZn = 1.08883;

double labF(double t) {
  constexpr double d = 6.0 / 29.0;
  return t > d * d * d ? std::cbrt(t) : t / (3.0 * d * d) + 4.0 / 29.0;
}

double labFInv(double f) {
  constexpr double d = 6.0 / 29.0;
  return f > d ? f * f * f : 3.0 * d * d * (f - 4.0 / 29.0);
}

glm::dvec3 toLab(const Rgb &c) {
  const double r = srgbToLinear(c.r), g = srgbToLinear(c.g), b = srgbToLinear(c.b);
  const double x = 0.4124564 * r + 0.3575761 * g + 0.1804375 * b;
  const double y = 0.2126729 * r + 0.7151522 * g + 0.0721750 * b;
  const double z = 0.0193339 * r + 0.1191920 * g + 0.9503041 * b;
  const double fx = labF(x / kXn), fy = labF(y / kYn), fz = labF(z / kZn);
  return {116.0 * fy - 16.0, 500.0 * (fx - fy), 200.0 * (fy - fz)};
}

Rgb fromLab(const glm::dvec3 &lab) {
  const double fy = (lab.x + 16.0) / 116.0, fx = fy + lab.y / 500.0, fz = fy - lab.z / 200.0;
  const double x = labFInv(fx) * kXn, y = labFInv(fy) * kYn, z = labFInv(fz) * kZn;
  const double r = 3.2404542 * x - 1.5371385 * y - 0.4985314 * z;
  const double g = -0.9692660 * x + 1.8760108 * y + 0.0415560 * z;
  const double b = 0.0556434 * x - 0.2040259 * y + 1.0572252 * z;
  return clamp01(Rgb(linearToSrgb(r), linearToSrgb(g), linearToSrgb(b)));
}

// HSL with h in 0..6 (sextants).
glm::dvec3 toHsl(const Rgb &c) {
  const double mx = std::max({c.r, c.g, c.b}), mn = std::min({c.r, c.g, c.b});
  const double l = (mx + mn) * 0.5, d = mx - mn;
  if (d <= 1e-12)
    return {0.0, 0.0, l};
  const double s = l > 0.5 ? d / (2.0 - mx - mn) : d / (mx + mn);
  double h;
  if (mx == c.r)
    h = std::fmod((c.g - c.b) / d + 6.0, 6.0);
  else if (mx == c.g)
    h = (c.b - c.r) / d + 2.0;
  else
    h = (c.r - c.g) / d + 4.0;
  return {h, s, l};
}

// The pure-hue part of a colour of chroma `c` at hue `h` (0..6).
Rgb hueChroma(double h, double c) {
  h = std::fmod(std::fmod(h, 6.0) + 6.0, 6.0);
  const double x = c * (1.0 - std::abs(std::fmod(h, 2.0) - 1.0));
  switch (int(h)) {
  case 0:
    return {c, x, 0.0};
  case 1:
    return {x, c, 0.0};
  case 2:
    return {0.0, c, x};
  case 3:
    return {0.0, x, c};
  case 4:
    return {x, 0.0, c};
  default:
    return {c, 0.0, x};
  }
}

Rgb fromHsl(const glm::dvec3 &hsl) {
  const double c = (1.0 - std::abs(2.0 * hsl.z - 1.0)) * hsl.y;
  return clamp01(hueChroma(hsl.x, c) + Rgb(hsl.z - c * 0.5));
}

Rgb rotateHue(const Rgb &c, double degrees) {
  const double a = degrees * M_PI / 180.0;
  const double cs = std::cos(a), sn = std::sin(a);
  const double k = 1.0 / 3.0, sq = std::sqrt(k);
  const double m00 = cs + (1.0 - cs) * k, m01 = k * (1.0 - cs) - sq * sn, m02 = k * (1.0 - cs) + sq * sn;
  const double m10 = k * (1.0 - cs) + sq * sn, m11 = cs + k * (1.0 - cs), m12 = k * (1.0 - cs) - sq * sn;
  const double m20 = k * (1.0 - cs) - sq * sn, m21 = k * (1.0 - cs) + sq * sn, m22 = cs + k * (1.0 - cs);
  return {m00 * c.r + m01 * c.g + m02 * c.b, m10 * c.r + m11 * c.g + m12 * c.b, m20 * c.r + m21 * c.g + m22 * c.b};
}

Rgb basicStep(Rgb c, const LutAdjustments &a) {
  // Exposure and white balance in linear light.
  Rgb l {srgbToLinear(c.r), srgbToLinear(c.g), srgbToLinear(c.b)};
  const double gain = std::pow(2.0, a.exposure);
  const double t = a.temperature * 0.25, n = a.tint * 0.25;
  l.r *= gain * (1.0 + t) * (1.0 + n * 0.5);
  l.g *= gain * (1.0 - n);
  l.b *= gain * (1.0 - t) * (1.0 + n * 0.5);
  c = {linearToSrgb(l.r), linearToSrgb(l.g), linearToSrgb(l.b)};

  // Midtones (gamma), then contrast around mid grey, in display space.
  if (a.gamma != 1.0) {
    const double inv = 1.0 / std::max(0.01, a.gamma);
    c = {std::pow(clamp01(c.r), inv), std::pow(clamp01(c.g), inv), std::pow(clamp01(c.b), inv)};
  }
  c = (c - 0.5) * a.contrast + 0.5;

  if (a.hue != 0.0)
    c = rotateHue(c, a.hue);

  const double y = luma(c);
  double sat = a.saturation;
  if (a.vibrance != 0.0) {
    const double existing = std::max({c.r, c.g, c.b}) - std::min({c.r, c.g, c.b});
    sat *= 1.0 + a.vibrance * (1.0 - clamp01(existing));
  }
  c = Rgb(y) + (c - y) * sat;

  if (a.toningStrength > 0.0) {
    const double w = clamp01(luma(c));
    const double ws = (1.0 - w) * (1.0 - w) * a.toningStrength;
    const double wh = w * w * a.toningStrength;
    const auto off = [](const QColor &q) { return Rgb(q.redF() - 0.5, q.greenF() - 0.5, q.blueF() - 0.5); };
    c += off(a.shadows) * ws + off(a.highlights) * wh;
  }
  return clamp01(c);
}

Rgb balanceStep(const Rgb &c, const LutColorBalance &b) {
  const double l = (std::max({c.r, c.g, c.b}) + std::min({c.r, c.g, c.b})) * 0.5;
  constexpr double a = 0.25, bb = 0.333, scale = 0.7;
  const double ws = clamp01((l - bb) / -a + 0.5) * scale;
  const double wm = clamp01((l - bb) / a + 0.5) * clamp01((l + bb - 1.0) / -a + 0.5) * scale;
  const double wh = clamp01((l + bb - 1.0) / a + 0.5) * scale;
  Rgb out = clamp01(c + b.shadows * ws + b.midtones * wm + b.highlights * wh);
  if (b.preserveLuminosity) {
    glm::dvec3 hsl = toHsl(out);
    hsl.z = l;
    out = fromHsl(hsl);
  }
  return out;
}

double hsvValueOf(const Rgb &c, int model) {
  switch (model) {
  case 0:
    return std::max({c.r, c.g, c.b});
  case 1:
    return (std::max({c.r, c.g, c.b}) + std::min({c.r, c.g, c.b})) * 0.5;
  case 2:
    return (c.r + c.g + c.b) / 3.0;
  default:
    return luma(c);
  }
}

Rgb hsvStep(const Rgb &c, const LutHsv &p) {
  if (p.colorize) {
    double y = luma(c);
    const double dv = p.value;
    y = dv > 0.0 ? y * (1.0 - dv) + dv : y * (1.0 + dv);
    return fromHsl({std::fmod(p.hue / 60.0 + 6.0, 6.0), clamp01(p.saturation), clamp01(y)});
  }
  const double mx = std::max({c.r, c.g, c.b}), mn = std::min({c.r, c.g, c.b});
  double chroma = mx - mn;
  double v = hsvValueOf(c, p.model);
  const double dv = p.value;
  if (chroma <= 1e-9) {
    v = dv < 0.0 ? v * (1.0 + dv) : v + dv * (1.0 - v);
    return Rgb(clamp01(v));
  }
  double h;
  if (mx == c.r)
    h = std::fmod((c.g - c.b) / chroma + 6.0, 6.0);
  else if (mx == c.g)
    h = (c.b - c.r) / chroma + 2.0;
  else
    h = (c.r - c.g) / chroma + 4.0;
  h += p.hue / 60.0;

  const double s = p.saturation;
  chroma = s > 0.0 ? std::min(1.0, chroma * (1.0 + s + 2.0 * s * s)) : chroma * (1.0 + s);
  if (dv != 0.0) {
    v += std::abs(dv) * ((dv > 0.0 ? 1.0 : 0.0) - v);
    chroma -= std::abs(dv) * chroma;
  }
  v = clamp01(v);
  switch (p.model) {
  case 0:
    chroma = std::min(chroma, v);
    break;
  case 1:
    chroma = std::min(chroma, v < 0.5 ? 2.0 * v : 2.0 - 2.0 * v);
    break;
  case 2:
    chroma = std::min(chroma, v < 1.0 / 3.0 ? 3.0 * v : 1.5 * (1.0 - v));
    break;
  default:
    break;
  }
  const Rgb hc = hueChroma(h, std::max(0.0, chroma));
  double m;
  switch (p.model) {
  case 0:
    m = v - chroma;
    break;
  case 1:
    m = v - chroma * 0.5;
    break;
  case 2:
    m = v - (hc.r + hc.g + hc.b) / 3.0;
    break;
  default:
    m = v - luma(hc);
    break;
  }
  return clamp01(hc + Rgb(m));
}

Rgb cdlStep(const Rgb &c, const LutCdl &p) {
  Rgb o;
  for (int i = 0; i < 3; ++i)
    o[i] = std::pow(std::max(0.0, c[i] * p.slope[i] + p.offset[i]), std::max(0.01, p.power[i]));
  const double y = luma(o);
  return clamp01(Rgb(y) + (o - y) * p.saturation);
}

// 0..1 function sampled for fast lookups (levels and curves channels).
struct Table {
  static constexpr int kSize = 4096;
  std::vector<float> v;

  template <typename F> explicit Table(F f) : v(kSize + 1) {
    for (int i = 0; i <= kSize; ++i)
      v[size_t(i)] = float(clamp01(f(double(i) / kSize)));
  }
  [[nodiscard]] double operator()(double x) const {
    const double f = clamp01(x) * kSize;
    const int i = std::min(int(f), kSize - 1);
    const double t = f - i;
    return v[size_t(i)] * (1.0 - t) + v[size_t(i) + 1] * t;
  }
};

// A step with its lookup tables built once per grading.
struct Prepared {
  const GradeStep *step;
  std::array<std::optional<Table>, kGradeChannels> tables;
};

Prepared prepare(const GradeStep &s) {
  Prepared p {&s, {}};
  for (int c = 0; c < kGradeChannels; ++c) {
    if (s.kind == GradeStep::Kind::Levels && !s.levels.ch[size_t(c)].isIdentity()) {
      const LevelsCurve lc = s.levels.ch[size_t(c)];
      p.tables[size_t(c)].emplace([lc](double x) { return lc.map(x); });
    } else if (s.kind == GradeStep::Kind::Curves && !isIdentityCurve(s.curves.ch[size_t(c)])) {
      const CurvePoints pts = s.curves.ch[size_t(c)];
      const std::vector<double> m = splineSecond(pts);
      p.tables[size_t(c)].emplace([&pts, &m](double x) { return splineAt(pts, m, x); });
    }
  }
  return p;
}

Rgb channelsStep(Rgb c, const Prepared &p) {
  const auto &t = p.tables;
  if (t[size_t(GradeChannel::Red)])
    c.r = (*t[size_t(GradeChannel::Red)])(c.r);
  if (t[size_t(GradeChannel::Green)])
    c.g = (*t[size_t(GradeChannel::Green)])(c.g);
  if (t[size_t(GradeChannel::Blue)])
    c.b = (*t[size_t(GradeChannel::Blue)])(c.b);
  if (const auto &m = t[size_t(GradeChannel::Rgb)])
    c = {(*m)(c.r), (*m)(c.g), (*m)(c.b)};
  if (const auto &l = t[size_t(GradeChannel::Lightness)]) {
    glm::dvec3 lab = toLab(c);
    lab.x = (*l)(lab.x / 100.0) * 100.0;
    c = fromLab(lab);
  }
  return c;
}

Rgb applyStep(const Rgb &c, const Prepared &p) {
  switch (p.step->kind) {
  case GradeStep::Kind::Basic:
    return basicStep(c, p.step->basic);
  case GradeStep::Kind::Levels:
  case GradeStep::Kind::Curves:
    return clamp01(channelsStep(c, p));
  case GradeStep::Kind::ColorBalance:
    return balanceStep(c, p.step->balance);
  case GradeStep::Kind::Hsv:
    return hsvStep(c, p.step->hsv);
  case GradeStep::Kind::Cdl:
    return cdlStep(c, p.step->cdl);
  case GradeStep::Kind::Count:
    break;
  }
  return c;
}

} // namespace

QImage apply(const QImage &strip, const LutGrade &g, int stepCount) {
  QImage out = strip.convertToFormat(QImage::Format_RGB888);
  const int n = stepCount < 0 ? int(g.steps.size()) : std::min(stepCount, int(g.steps.size()));
  std::vector<Prepared> steps;
  for (int i = 0; i < n; ++i)
    if (g.steps[size_t(i)].enabled)
      steps.push_back(prepare(g.steps[size_t(i)]));
  if (steps.empty())
    return out;
  for (int y = 0; y < out.height(); ++y) {
    uchar *row = out.scanLine(y);
    for (int x = 0; x < out.width(); ++x) {
      uchar *px = row + x * 3;
      Rgb c(px[0] / 255.0, px[1] / 255.0, px[2] / 255.0);
      for (const Prepared &p: steps)
        c = applyStep(c, p);
      px[0] = uchar(std::lround(clamp01(c.r) * 255.0));
      px[1] = uchar(std::lround(clamp01(c.g) * 255.0));
      px[2] = uchar(std::lround(clamp01(c.b) * 255.0));
    }
  }
  return out;
}

double lightness(double r, double g, double b) {
  return clamp01(toLab({r, g, b}).x / 100.0);
}

Histogram histogram(const QImage &image, GradeChannel channel) {
  Histogram h {};
  const QImage img = image.convertToFormat(QImage::Format_RGB888);
  // L* per 8-bit grey level would be wrong for colours: per pixel.
  for (int y = 0; y < img.height(); ++y) {
    const uchar *row = img.constScanLine(y);
    for (int x = 0; x < img.width(); ++x) {
      const uchar *p = row + x * 3;
      int bin = 0;
      switch (channel) {
      case GradeChannel::Lightness:
        bin = int(std::lround(lightness(p[0] / 255.0, p[1] / 255.0, p[2] / 255.0) * 255.0));
        break;
      case GradeChannel::Rgb:
        bin = (int(p[0]) + p[1] + p[2] + 1) / 3;
        break;
      case GradeChannel::Red:
        bin = p[0];
        break;
      case GradeChannel::Green:
        bin = p[1];
        break;
      case GradeChannel::Blue:
      case GradeChannel::Count:
        bin = p[2];
        break;
      }
      h[size_t(std::clamp(bin, 0, 255))] += 1.0;
    }
  }
  return h;
}

LevelsCurve autoLevels(const Histogram &h, GradeChannel channel) {
  LevelsCurve out;
  double total = 0.0;
  for (double v: h)
    total += v;
  if (total <= 0.0)
    return out;
  constexpr double clip = 0.001;
  // Krita's defaults: lightness moves the points less and the midtones a quarter.
  const bool light = channel == GradeChannel::Lightness;
  const double maxOffset = light ? 0.1 : 1.0, amount = light ? 0.25 : 0.5;

  int black = 0, white = 255;
  double acc = 0.0;
  for (int i = 0; i < 256; ++i) {
    acc += h[size_t(i)] / total;
    if (acc > clip)
      break;
    black = i;
  }
  acc = 0.0;
  for (int i = 255; i >= 0; --i) {
    acc += h[size_t(i)] / total;
    if (acc > clip)
      break;
    white = i;
  }
  if (white <= black)
    return out;
  out.inBlack = std::min(maxOffset, black / 255.0);
  out.inWhite = std::max(1.0 - maxOffset, white / 255.0);

  // Median of the bins between the points.
  double inside = 0.0;
  for (int i = black; i <= white; ++i)
    inside += h[size_t(i)];
  double median = (out.inBlack + out.inWhite) * 0.5;
  acc = 0.0;
  for (int i = black; i <= white; ++i) {
    acc += h[size_t(i)];
    if (acc >= inside * 0.5) {
      median = i / 255.0;
      break;
    }
  }
  const double mid = (out.inBlack + out.inWhite) * 0.5;
  const double target = mid + (median - mid) * amount;
  const double rel = std::clamp((target - out.inBlack) / (out.inWhite - out.inBlack), 1e-4, 1.0 - 1e-4);
  out.gamma = std::clamp(std::log(rel) / std::log(0.5), 0.1, 10.0);
  return out;
}

} // namespace grade
} // namespace eng
