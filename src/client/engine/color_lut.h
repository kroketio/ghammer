#pragma once

// 3D colour LUTs (Godot-compatible): an N×N×N cube as a horizontal strip of N slices of N×N (x red, y green,
// slice = blue). The neutral 33³ template (1089×33) is Godot's. Applied after tone mapping to the clamped
// sRGB colour, like Godot.

#include <QColor>
#include <QImage>
#include <QJsonObject>
#include <QString>
#include <optional>

#include "color_grade.h"

namespace eng {

// Made here (`grade` set: always the neutral template through the grade) or imported (`grade` unset, used as is).
struct ColorLut {
  QString name;
  QImage strip; // RGB888, (N*N) × N
  std::optional<LutGrade> grade;
  QString source; // imported: the file it was read from

  [[nodiscard]] int size() const { return strip.height(); }
  [[nodiscard]] bool isValid() const { return size() >= 2 && strip.width() == size() * size(); }
  [[nodiscard]] bool isGraded() const { return grade.has_value(); }
};

namespace lut {

// The neutral 33³ template from resources.
[[nodiscard]] QImage templateStrip();
// A neutral strip of any size (identity mapping).
[[nodiscard]] QImage identityStrip(int size);

// Reads a LUT image (any strip / grid layout with N³ pixels) or an Adobe/Resolve .cube file.
[[nodiscard]] std::optional<ColorLut> importFile(const QString &path, QString *error = nullptr);
// Writes .png (strip) or .cube depending on the extension.
bool exportFile(const ColorLut &lut, const QString &path, QString *error = nullptr);

// Applies Basic grading to every entry of `strip`.
[[nodiscard]] QImage applyAdjustments(const QImage &strip, const LutAdjustments &adj);
// A LUT made in the editor: the neutral template through `g`.
[[nodiscard]] ColorLut graded(const QString &name, const LutGrade &g);

// One sRGB colour through the strip (trilinear), sampled like Godot's tonemap shader: the colour is the
// texture coordinate as is (no half-texel remap), clamped to the edge texels.
[[nodiscard]] QColor sample(const QImage &strip, const QColor &c);
// Applies the LUT to a whole image, trilinear.
[[nodiscard]] QImage applyToImage(const QImage &strip, const QImage &image);

} // namespace lut
} // namespace eng
