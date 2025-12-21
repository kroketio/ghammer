#pragma once

#include <vector>

#include <QByteArray>
#include <QImage>
#include <QSize>
#include <QString>

// HDR panoramas (skyboxes): OpenEXR read / write through tinyexr (server and client).
namespace hdr {

// Linear RGB, float, interleaved, rows top to bottom.
struct Image {
  int w = 0, h = 0;
  std::vector<float> rgb;
  [[nodiscard]] bool isNull() const { return w <= 0 || h <= 0 || rgb.empty(); }
};

// The data window's size; reads the header only (the file is mapped).
QSize exrSize(const QString &path, QString *err = nullptr);

// Decodes a scanline EXR (R, G, B in any order, half or float; one channel = grey). A valid `size` no larger than
// the image area-averages the pixels down while reading, so a 16K panorama never exists as floats.
bool loadExr(const QString &path, Image &out, QString *err = nullptr, QSize size = {});
bool loadExrFromMemory(const QByteArray &bytes, Image &out, QString *err = nullptr, QSize size = {});

// Half-float RGB, ZIP compressed.
QByteArray encodeExr(const Image &img, QString *err = nullptr);

// An 8-bit sRGB preview: exposure from the log-average luminance, Reinhard.
QImage tonemapped(const Image &img, int w, int h);

// For a float texture upload (QRhiTexture::RGBA16F), alpha 1.
QImage toRgba16F(const Image &img);

} // namespace hdr
