#include "color_lut.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonValue>
#include <QTextStream>
#include <algorithm>
#include <cmath>
#include <vector>

namespace eng {

namespace lut {

namespace {

struct Rgb {
  double r, g, b;
};

double clamp01(double v) {
  return std::clamp(v, 0.0, 1.0);
}

QImage stripFromCube(const std::vector<Rgb> &values, int n) {
  QImage strip(n * n, n, QImage::Format_RGB888);
  for (int b = 0; b < n; ++b)
    for (int g = 0; g < n; ++g)
      for (int r = 0; r < n; ++r) {
        const Rgb &v = values[size_t(r + n * (g + n * b))];
        strip.setPixel(b * n + r, g,
                       qRgb(int(std::lround(clamp01(v.r) * 255.0)), int(std::lround(clamp01(v.g) * 255.0)),
                            int(std::lround(clamp01(v.b) * 255.0))));
      }
  return strip;
}

std::optional<ColorLut> readCube(const QString &path, QString *error) {
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
    if (error)
      *error = QStringLiteral("Cannot open %1").arg(path);
    return std::nullopt;
  }
  QTextStream in(&f);
  int n = 0;
  QString title;
  double dmin[3] = {0, 0, 0}, dmax[3] = {1, 1, 1};
  std::vector<Rgb> values;
  while (!in.atEnd()) {
    const QString line = in.readLine().trimmed();
    if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
      continue;
    const QStringList parts = line.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    const QString key = parts.first().toUpper();
    if (key == QLatin1String("TITLE")) {
      title = line.mid(5).trimmed();
      title.remove(QLatin1Char('"'));
    } else if (key == QLatin1String("LUT_3D_SIZE") && parts.size() > 1) {
      n = parts[1].toInt();
    } else if (key == QLatin1String("LUT_1D_SIZE")) {
      if (error)
        *error = QStringLiteral("1D .cube LUTs are not supported, only 3D");
      return std::nullopt;
    } else if (key == QLatin1String("DOMAIN_MIN") && parts.size() > 3) {
      for (int i = 0; i < 3; ++i)
        dmin[i] = parts[i + 1].toDouble();
    } else if (key == QLatin1String("DOMAIN_MAX") && parts.size() > 3) {
      for (int i = 0; i < 3; ++i)
        dmax[i] = parts[i + 1].toDouble();
    } else if (parts.size() >= 3) {
      bool ok1 = false, ok2 = false, ok3 = false;
      Rgb v{parts[0].toDouble(&ok1), parts[1].toDouble(&ok2), parts[2].toDouble(&ok3)};
      if (ok1 && ok2 && ok3) {
        v.r = (v.r - dmin[0]) / std::max(1e-9, dmax[0] - dmin[0]);
        v.g = (v.g - dmin[1]) / std::max(1e-9, dmax[1] - dmin[1]);
        v.b = (v.b - dmin[2]) / std::max(1e-9, dmax[2] - dmin[2]);
        values.push_back(v);
      }
    }
  }
  if (n < 2 || n > 256 || values.size() != size_t(n) * n * n) {
    if (error)
      *error = QStringLiteral("%1 is not a valid 3D .cube file (size %2, %3 entries)")
                   .arg(QFileInfo(path).fileName())
                   .arg(n)
                   .arg(values.size());
    return std::nullopt;
  }
  ColorLut out;
  out.name = title.isEmpty() ? QFileInfo(path).completeBaseName() : title;
  out.strip = stripFromCube(values, n);
  return out;
}

} // namespace

QImage templateStrip() {
  // Read once: every slider move grades it again.
  static const QImage strip = [] {
    QImage img(QStringLiteral(":/assets/environment_adjustments_3d_lut_template.png"));
    if (img.isNull() || img.width() != img.height() * img.height())
      return identityStrip(33);
    return img.convertToFormat(QImage::Format_RGB888);
  }();
  return strip;
}

QImage identityStrip(int n) {
  n = std::clamp(n, 2, 256);
  QImage strip(n * n, n, QImage::Format_RGB888);
  const auto v = [n](int i) { return int(std::lround(double(i) * 255.0 / double(n - 1))); };
  for (int b = 0; b < n; ++b)
    for (int g = 0; g < n; ++g)
      for (int r = 0; r < n; ++r)
        strip.setPixel(b * n + r, g, qRgb(v(r), v(g), v(b)));
  return strip;
}

std::optional<ColorLut> importFile(const QString &path, QString *error) {
  if (path.endsWith(QLatin1String(".cube"), Qt::CaseInsensitive)) {
    auto cube = readCube(path, error);
    if (cube)
      cube->source = QFileInfo(path).absoluteFilePath();
    return cube;
  }

  QImage img(path);
  if (img.isNull()) {
    if (error)
      *error = QStringLiteral("Cannot read image %1").arg(path);
    return std::nullopt;
  }
  img = img.convertToFormat(QImage::Format_RGB888);
  // Any layout of N slices of N×N: N = cbrt(pixel count), slices row-major.
  const qint64 pixels = qint64(img.width()) * img.height();
  const int n = int(std::lround(std::cbrt(double(pixels))));
  if (n < 2 || qint64(n) * n * n != pixels || img.width() % n != 0 || img.height() % n != 0) {
    if (error)
      *error = QStringLiteral("%1 (%2×%3) is not a 3D LUT: expected N slices of N×N pixels, e.g. 1089×33")
                   .arg(QFileInfo(path).fileName())
                   .arg(img.width())
                   .arg(img.height());
    return std::nullopt;
  }
  ColorLut out;
  out.name = QFileInfo(path).completeBaseName();
  out.source = QFileInfo(path).absoluteFilePath();
  const int cols = img.width() / n;
  if (img.height() == n) {
    out.strip = img;
  } else {
    out.strip = QImage(n * n, n, QImage::Format_RGB888);
    for (int s = 0; s < n; ++s) {
      const int sx = (s % cols) * n, sy = (s / cols) * n;
      for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x)
          out.strip.setPixel(s * n + x, y, img.pixel(sx + x, sy + y));
    }
  }
  return out;
}

bool exportFile(const ColorLut &l, const QString &path, QString *error) {
  if (!l.isValid()) {
    if (error)
      *error = QStringLiteral("LUT is empty");
    return false;
  }
  if (path.endsWith(QLatin1String(".cube"), Qt::CaseInsensitive)) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
      if (error)
        *error = QStringLiteral("Cannot write %1").arg(path);
      return false;
    }
    QTextStream out(&f);
    const int n = l.size();
    out << "TITLE \"" << l.name << "\"\n" << "LUT_3D_SIZE " << n << "\n\n";
    for (int b = 0; b < n; ++b)
      for (int g = 0; g < n; ++g)
        for (int r = 0; r < n; ++r) {
          const QRgb p = l.strip.pixel(b * n + r, g);
          out << QString::number(qRed(p) / 255.0, 'f', 6) << ' ' << QString::number(qGreen(p) / 255.0, 'f', 6)
              << ' ' << QString::number(qBlue(p) / 255.0, 'f', 6) << '\n';
        }
    return true;
  }
  if (!l.strip.save(path)) {
    if (error)
      *error = QStringLiteral("Cannot write %1").arg(path);
    return false;
  }
  return true;
}

QImage applyAdjustments(const QImage &src, const LutAdjustments &adj) {
  return grade::apply(src, LutGrade::basic(adj));
}

ColorLut graded(const QString &name, const LutGrade &g) {
  ColorLut l;
  l.name = name;
  l.strip = grade::apply(templateStrip(), g);
  l.grade = g;
  return l;
}

namespace {

// Trilinear lookup with Godot's convention: texel coordinate = c·n − 0.5,
// clamped to the edge texels (linear filtering, clamp-to-edge sampler).
struct LutTable {
  int n = 0;
  const uchar *bits = nullptr;
  qsizetype stride = 0;

  explicit LutTable(const QImage &rgb888) : n(rgb888.height()), bits(rgb888.constBits()), stride(rgb888.bytesPerLine()) {}

  void lookup(const double in[3], double out[3]) const {
    int i0[3], i1[3];
    double t[3];
    for (int k = 0; k < 3; ++k) {
      const double f = std::clamp(std::clamp(in[k], 0.0, 1.0) * n - 0.5, 0.0, double(n - 1));
      i0[k] = std::min(int(f), n - 1);
      i1[k] = std::min(i0[k] + 1, n - 1);
      t[k] = f - i0[k];
    }
    out[0] = out[1] = out[2] = 0.0;
    for (int corner = 0; corner < 8; ++corner) {
      const int r = (corner & 1) ? i1[0] : i0[0];
      const int g = (corner & 2) ? i1[1] : i0[1];
      const int b = (corner & 4) ? i1[2] : i0[2];
      const double w = ((corner & 1) ? t[0] : 1 - t[0]) * ((corner & 2) ? t[1] : 1 - t[1])
          * ((corner & 4) ? t[2] : 1 - t[2]);
      if (w == 0.0)
        continue;
      const uchar *p = bits + g * stride + (b * n + r) * 3;
      out[0] += w * p[0];
      out[1] += w * p[1];
      out[2] += w * p[2];
    }
  }
};

} // namespace

QColor sample(const QImage &strip, const QColor &c) {
  if (strip.height() < 2)
    return c;
  const QImage rgb = strip.convertToFormat(QImage::Format_RGB888);
  const LutTable lut(rgb);
  const double in[3] = {c.redF(), c.greenF(), c.blueF()};
  double out[3];
  lut.lookup(in, out);
  return QColor(int(std::lround(out[0])), int(std::lround(out[1])), int(std::lround(out[2])));
}

QImage applyToImage(const QImage &strip, const QImage &image) {
  QImage out = image.convertToFormat(QImage::Format_RGB888);
  if (strip.height() < 2)
    return out;
  const QImage rgb = strip.convertToFormat(QImage::Format_RGB888);
  const LutTable lut(rgb);
  for (int y = 0; y < out.height(); ++y) {
    auto *row = out.scanLine(y);
    for (int x = 0; x < out.width(); ++x) {
      uchar *p = row + x * 3;
      const double in[3] = {p[0] / 255.0, p[1] / 255.0, p[2] / 255.0};
      double o[3];
      lut.lookup(in, o);
      p[0] = uchar(std::clamp(std::lround(o[0]), 0L, 255L));
      p[1] = uchar(std::clamp(std::lround(o[1]), 0L, 255L));
      p[2] = uchar(std::clamp(std::lround(o[2]), 0L, 255L));
    }
  }
  return out;
}

} // namespace lut
} // namespace eng
