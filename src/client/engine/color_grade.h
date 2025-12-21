#pragma once

// Colour grading for made LUTs: an ordered adjustment stack applied to every entry of the neutral LUT, on the
// sRGB-encoded 0..1 values (where Godot's LUT lookup happens), clamped after each step. Levels, Curves, Color
// balance, HSV/HSL and Slope/Offset/Power reimplement Krita's filters of the same name.

#include <QColor>
#include <QImage>
#include <QJsonObject>
#include <QString>

#include <array>
#include <vector>

#include <glm/glm.hpp>

namespace eng {

// The "Basic" step.
struct LutAdjustments {
  double exposure = 0.0;      // stops
  double contrast = 1.0;      // around mid grey, in display space
  double saturation = 1.0;
  double vibrance = 0.0;      // saturation boost weighted towards muted colours
  double gamma = 1.0;         // midtones: > 1 brightens
  double hue = 0.0;           // degrees
  double temperature = 0.0;   // -1 cool .. +1 warm
  double tint = 0.0;          // -1 green .. +1 magenta
  QColor shadows = QColor(128, 128, 128);    // split toning; grey = neutral
  QColor highlights = QColor(128, 128, 128);
  double toningStrength = 0.5;

  [[nodiscard]] bool isNeutral() const;
  bool operator==(const LutAdjustments &) const = default;

  [[nodiscard]] QJsonObject toJson() const;
  [[nodiscard]] static LutAdjustments fromJson(const QJsonObject &o);
};

// Order used everywhere (UI, JSON). Applied Red, Green, Blue, then RGB, then Lightness (CIE L*).
enum class GradeChannel { Lightness = 0, Rgb, Red, Green, Blue, Count };
constexpr int kGradeChannels = int(GradeChannel::Count);

struct LevelsCurve {
  double inBlack = 0.0, inWhite = 1.0, gamma = 1.0, outBlack = 0.0, outWhite = 1.0;

  [[nodiscard]] double map(double x) const;
  [[nodiscard]] bool isIdentity() const { return *this == LevelsCurve(); }
  bool operator==(const LevelsCurve &) const = default;
};

struct LutLevels {
  std::array<LevelsCurve, kGradeChannels> ch {};
  bool operator==(const LutLevels &) const = default;
};

// Points sorted by x, at least two; the default is (0,0)-(1,1).
using CurvePoints = std::vector<glm::dvec2>;
[[nodiscard]] CurvePoints identityCurve();
[[nodiscard]] bool isIdentityCurve(const CurvePoints &pts);
// Natural cubic spline through the points; held flat outside them, clamped 0..1.
[[nodiscard]] double curveValue(const CurvePoints &pts, double x);

struct LutCurves {
  std::array<CurvePoints, kGradeChannels> ch {identityCurve(), identityCurve(), identityCurve(), identityCurve(),
                                              identityCurve()};
  bool operator==(const LutCurves &) const = default;
};

// -1..1 per tone range; x = cyan-red, y = magenta-green, z = yellow-blue.
struct LutColorBalance {
  glm::dvec3 shadows {0.0}, midtones {0.0}, highlights {0.0};
  bool preserveLuminosity = true;
  bool operator==(const LutColorBalance &) const = default;
};

// model: 0 HSV, 1 HSL, 2 HSI, 3 HSY. hue in degrees (-180..180; 0..360 when
// colorizing), saturation and value -1..1 (saturation 0..1 when colorizing).
struct LutHsv {
  int model = 1;
  double hue = 0.0, saturation = 0.0, value = 0.0;
  bool colorize = false;
  bool operator==(const LutHsv &) const = default;
};

// ASC-CDL: out = pow(max(0, in * slope + offset), power), then saturation.
struct LutCdl {
  glm::dvec3 slope {1.0}, offset {0.0}, power {1.0};
  double saturation = 1.0;
  bool operator==(const LutCdl &) const = default;
};

struct GradeStep {
  enum class Kind { Basic = 0, Levels, Curves, ColorBalance, Hsv, Cdl, Count };
  Kind kind = Kind::Basic;
  int id = 0; // unique within its grade, stable across reordering
  bool enabled = true;
  LutAdjustments basic;
  LutLevels levels;
  LutCurves curves;
  LutColorBalance balance;
  LutHsv hsv;
  LutCdl cdl;

  bool operator==(const GradeStep &) const = default;
  [[nodiscard]] static GradeStep make(Kind kind);
};

[[nodiscard]] const char *gradeKindKey(GradeStep::Kind kind); // JSON: "basic", "levels", ...

struct LutGrade {
  std::vector<GradeStep> steps;

  bool operator==(const LutGrade &) const = default;
  [[nodiscard]] QJsonObject toJson() const;
  // {"steps": [...]}; an old flat object (Basic settings only) is one Basic step.
  [[nodiscard]] static LutGrade fromJson(const QJsonObject &o);
  [[nodiscard]] static LutGrade basic(const LutAdjustments &a = {});
  [[nodiscard]] int nextId() const;
  [[nodiscard]] int indexOf(int stepId) const; // -1 when gone
};

namespace grade {

// Grades `strip` (RGB888) through the first `stepCount` steps (-1 = all), skipping disabled ones.
[[nodiscard]] QImage apply(const QImage &strip, const LutGrade &g, int stepCount = -1);

// CIE L* / 100 of an sRGB-encoded colour.
[[nodiscard]] double lightness(double r, double g, double b);

using Histogram = std::array<double, 256>;
// 256 bins of an RGB888 image for one channel (RGB = mean of R, G, B).
[[nodiscard]] Histogram histogram(const QImage &image, GradeChannel channel);
// Black / white points clipping 0.1 % at each end, gamma from the median.
[[nodiscard]] LevelsCurve autoLevels(const Histogram &h, GradeChannel channel);

} // namespace grade
} // namespace eng
