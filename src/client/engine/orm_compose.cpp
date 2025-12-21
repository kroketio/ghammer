#include "orm_compose.h"

#include <algorithm>

namespace eng {

namespace {

QImage rgba(const QImage &img, const QSize &size) {
  if (img.isNull())
    return {};
  QImage out = img.size() == size ? img : img.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
  if (out.format() != QImage::Format_RGBA8888)
    out = out.convertToFormat(QImage::Format_RGBA8888);
  return out;
}

QSize largest(const QImage &a, const QImage &b, const QImage &c) {
  QSize s;
  for (const QImage *img: {&a, &b, &c})
    if (!img->isNull() && qint64(img->width()) * img->height() > qint64(s.width()) * s.height())
      s = img->size();
  return s;
}

} // namespace

uchar channelValue(const QImage &img, int x, int y, matprops::Channel ch) {
  const uchar *p = img.constScanLine(y) + x * 4;
  switch (ch) {
    case matprops::Channel::Red: return p[0];
    case matprops::Channel::Green: return p[1];
    case matprops::Channel::Blue: return p[2];
    case matprops::Channel::Alpha: return p[3];
    case matprops::Channel::Grey: return uchar((int(p[0]) + int(p[1]) + int(p[2]) + 1) / 3);
  }
  return p[0];
}

QImage composeOrm(const QImage &ao, matprops::Channel aoChannel, const QImage &roughness,
                  matprops::Channel roughnessChannel, const QImage &metallic, matprops::Channel metallicChannel) {
  const QSize size = largest(ao, roughness, metallic);
  if (!size.isValid() || size.isEmpty())
    return {};
  // Often one ARM image is all three sources: convert it once.
  const QImage a = rgba(ao, size);
  const QImage r = (roughness.cacheKey() == ao.cacheKey() && !a.isNull()) ? a : rgba(roughness, size);
  const QImage m = (metallic.cacheKey() == ao.cacheKey() && !a.isNull())
                       ? a
                       : ((metallic.cacheKey() == roughness.cacheKey() && !r.isNull()) ? r : rgba(metallic, size));
  QImage out(size, QImage::Format_RGBA8888);
  for (int y = 0; y < size.height(); ++y) {
    uchar *dst = out.scanLine(y);
    for (int x = 0; x < size.width(); ++x) {
      dst[x * 4 + 0] = a.isNull() ? 255 : channelValue(a, x, y, aoChannel);
      dst[x * 4 + 1] = r.isNull() ? 255 : channelValue(r, x, y, roughnessChannel);
      dst[x * 4 + 2] = m.isNull() ? 255 : channelValue(m, x, y, metallicChannel);
      dst[x * 4 + 3] = 255;
    }
  }
  return out;
}

QImage mergeOpacity(const QImage &albedo, const QImage &opacity) {
  if (albedo.isNull() || opacity.isNull())
    return albedo;
  QImage out = albedo.format() == QImage::Format_RGBA8888 ? albedo : albedo.convertToFormat(QImage::Format_RGBA8888);
  const QImage o = rgba(opacity, out.size());
  for (int y = 0; y < out.height(); ++y) {
    uchar *dst = out.scanLine(y);
    for (int x = 0; x < out.width(); ++x)
      dst[x * 4 + 3] = channelValue(o, x, y, matprops::Channel::Red);
  }
  return out;
}

QImage errorChecker() {
  QImage img(64, 64, QImage::Format_RGBA8888);
  for (int y = 0; y < 64; ++y) {
    uchar *row = img.scanLine(y);
    for (int x = 0; x < 64; ++x) {
      const bool odd = ((x / 8) + (y / 8)) & 1;
      row[x * 4 + 0] = odd ? 0 : 200;
      row[x * 4 + 1] = 0;
      row[x * 4 + 2] = odd ? 0 : 200;
      row[x * 4 + 3] = 255;
    }
  }
  return img;
}

} // namespace eng
