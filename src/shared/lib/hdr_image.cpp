#include "hdr_image.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <QFile>
#include <QFloat16>

#include "tinyexr/tinyexr.h"

namespace hdr {
namespace {

void setError(QString *err, const char *msg, const QString &fallback) {
  if (err)
    *err = msg ? QString::fromUtf8(msg) : fallback;
  if (msg)
    FreeEXRErrorMessage(msg);
}

float finite(const float v) {
  if (!std::isfinite(v))
    return v > 0.0f ? 65504.0f : 0.0f;
  return std::clamp(v, 0.0f, 65504.0f);
}

struct HeaderGuard {
  EXRHeader h;
  bool parsed = false;
  HeaderGuard() { InitEXRHeader(&h); }
  ~HeaderGuard() {
    if (parsed)
      FreeEXRHeader(&h);
  }
};

struct ImageGuard {
  EXRImage img;
  bool loaded = false;
  ImageGuard() { InitEXRImage(&img); }
  ~ImageGuard() {
    if (loaded)
      FreeEXRImage(&img);
  }
};

bool decode(const unsigned char *data, const size_t size, Image &out, QString *err, QSize target) {
  out = {};
  EXRVersion version;
  if (ParseEXRVersionFromMemory(&version, data, size) != TINYEXR_SUCCESS) {
    if (err)
      *err = QStringLiteral("not an OpenEXR file");
    return false;
  }
  if (version.multipart || version.non_image) {
    if (err)
      *err = QStringLiteral("multi-part / deep EXR files are not supported");
    return false;
  }
  HeaderGuard hg;
  const char *msg = nullptr;
  if (ParseEXRHeaderFromMemory(&hg.h, &version, data, size, &msg) != TINYEXR_SUCCESS) {
    setError(err, msg, QStringLiteral("bad EXR header"));
    return false;
  }
  hg.parsed = true;
  if (hg.h.tiled) {
    if (err)
      *err = QStringLiteral("tiled EXR files are not supported");
    return false;
  }
  // Keep halves as halves: a 16K x 8K RGB panorama is 800 MB that way.
  for (int c = 0; c < hg.h.num_channels; ++c)
    hg.h.requested_pixel_types[c] = hg.h.pixel_types[c] == TINYEXR_PIXELTYPE_UINT ? TINYEXR_PIXELTYPE_UINT
                                                                                    : hg.h.pixel_types[c];
  ImageGuard ig;
  if (LoadEXRImageFromMemory(&ig.img, &hg.h, data, size, &msg) != TINYEXR_SUCCESS) {
    setError(err, msg, QStringLiteral("cannot decode EXR pixels"));
    return false;
  }
  ig.loaded = true;
  const int w = ig.img.width, h = ig.img.height;
  if (w <= 0 || h <= 0 || !ig.img.images) {
    if (err)
      *err = QStringLiteral("empty EXR image");
    return false;
  }

  int idx[3] = {-1, -1, -1};
  for (int c = 0; c < hg.h.num_channels; ++c) {
    const char *n = hg.h.channels[c].name;
    if (!std::strcmp(n, "R") || !std::strcmp(n, "r"))
      idx[0] = c;
    else if (!std::strcmp(n, "G") || !std::strcmp(n, "g"))
      idx[1] = c;
    else if (!std::strcmp(n, "B") || !std::strcmp(n, "b"))
      idx[2] = c;
  }
  if (idx[0] < 0 || idx[1] < 0 || idx[2] < 0) {
    // One channel (Y, or anything): grey.
    int any = -1;
    for (int c = 0; c < hg.h.num_channels && any < 0; ++c)
      if (std::strcmp(hg.h.channels[c].name, "A"))
        any = c;
    if (any < 0) {
      if (err)
        *err = QStringLiteral("no colour channels");
      return false;
    }
    idx[0] = idx[0] < 0 ? any : idx[0];
    idx[1] = idx[1] < 0 ? any : idx[1];
    idx[2] = idx[2] < 0 ? any : idx[2];
  }

  const auto sample = [&](const int c, const size_t i) -> float {
    const unsigned char *p = ig.img.images[c];
    switch (hg.h.requested_pixel_types[c]) {
    case TINYEXR_PIXELTYPE_HALF: {
      qfloat16 v;
      std::memcpy(&v, p + i * 2, 2);
      return finite(float(v));
    }
    case TINYEXR_PIXELTYPE_FLOAT: {
      float v;
      std::memcpy(&v, p + i * 4, 4);
      return finite(v);
    }
    default: {
      quint32 v;
      std::memcpy(&v, p + i * 4, 4);
      return finite(float(v));
    }
    }
  };

  const bool shrink = target.isValid() && target.width() <= w && target.height() <= h &&
                      (target.width() < w || target.height() < h);
  const int ow = shrink ? target.width() : w, oh = shrink ? target.height() : h;
  out.w = ow;
  out.h = oh;
  out.rgb.assign(size_t(ow) * oh * 3, 0.0f);
  if (!shrink) {
    for (size_t i = 0, n = size_t(w) * h; i < n; ++i)
      for (int k = 0; k < 3; ++k)
        out.rgb[i * 3 + k] = sample(idx[k], i);
    return true;
  }
  // Area average: every source pixel lands in exactly one target pixel.
  std::vector<double> acc(size_t(ow) * oh * 3, 0.0);
  std::vector<quint32> count(size_t(ow) * oh, 0);
  std::vector<int> col(w);
  for (int x = 0; x < w; ++x)
    col[x] = int(qint64(x) * ow / w);
  for (int y = 0; y < h; ++y) {
    const size_t row = size_t(qint64(y) * oh / h) * ow;
    const size_t src = size_t(y) * w;
    for (int x = 0; x < w; ++x) {
      const size_t t = row + col[x];
      for (int k = 0; k < 3; ++k)
        acc[t * 3 + k] += sample(idx[k], src + x);
      ++count[t];
    }
  }
  for (size_t t = 0; t < count.size(); ++t) {
    const double n = std::max<quint32>(1, count[t]);
    for (int k = 0; k < 3; ++k)
      out.rgb[t * 3 + k] = float(acc[t * 3 + k] / n);
  }
  return true;
}

} // namespace

QSize exrSize(const QString &path, QString *err) {
  EXRVersion version;
  const QByteArray name = QFile::encodeName(path);
  if (ParseEXRVersionFromFile(&version, name.constData()) != TINYEXR_SUCCESS) {
    if (err)
      *err = QStringLiteral("not an OpenEXR file");
    return {};
  }
  HeaderGuard hg;
  const char *msg = nullptr;
  if (ParseEXRHeaderFromFile(&hg.h, &version, name.constData(), &msg) != TINYEXR_SUCCESS) {
    setError(err, msg, QStringLiteral("bad EXR header"));
    return {};
  }
  hg.parsed = true;
  return {hg.h.data_window.max_x - hg.h.data_window.min_x + 1, hg.h.data_window.max_y - hg.h.data_window.min_y + 1};
}

bool loadExr(const QString &path, Image &out, QString *err, const QSize size) {
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly)) {
    if (err)
      *err = f.errorString();
    return false;
  }
  // Mapped, not read: the source can be 170 MB.
  const qint64 n = f.size();
  uchar *data = n > 0 ? f.map(0, n) : nullptr;
  if (!data) {
    const QByteArray bytes = f.readAll();
    return loadExrFromMemory(bytes, out, err, size);
  }
  const bool ok = decode(data, size_t(n), out, err, size);
  f.unmap(data);
  return ok;
}

bool loadExrFromMemory(const QByteArray &bytes, Image &out, QString *err, const QSize size) {
  if (bytes.size() < 8) {
    if (err)
      *err = QStringLiteral("not an OpenEXR file");
    return false;
  }
  return decode(reinterpret_cast<const unsigned char *>(bytes.constData()), size_t(bytes.size()), out, err, size);
}

QByteArray encodeExr(const Image &img, QString *err) {
  if (img.isNull()) {
    if (err)
      *err = QStringLiteral("empty image");
    return {};
  }
  const size_t n = size_t(img.w) * img.h;
  // OpenEXR's channel order is alphabetical: B, G, R.
  std::vector<float> planes[3];
  for (auto &p: planes)
    p.resize(n);
  for (size_t i = 0; i < n; ++i) {
    planes[0][i] = img.rgb[i * 3 + 2];
    planes[1][i] = img.rgb[i * 3 + 1];
    planes[2][i] = img.rgb[i * 3 + 0];
  }
  float *ptrs[3] = {planes[0].data(), planes[1].data(), planes[2].data()};

  EXRImage image;
  InitEXRImage(&image);
  image.num_channels = 3;
  image.images = reinterpret_cast<unsigned char **>(ptrs);
  image.width = img.w;
  image.height = img.h;

  EXRHeader header;
  InitEXRHeader(&header);
  EXRChannelInfo channels[3];
  std::memset(channels, 0, sizeof(channels));
  std::strcpy(channels[0].name, "B");
  std::strcpy(channels[1].name, "G");
  std::strcpy(channels[2].name, "R");
  int pixelTypes[3] = {TINYEXR_PIXELTYPE_FLOAT, TINYEXR_PIXELTYPE_FLOAT, TINYEXR_PIXELTYPE_FLOAT};
  int storedTypes[3] = {TINYEXR_PIXELTYPE_HALF, TINYEXR_PIXELTYPE_HALF, TINYEXR_PIXELTYPE_HALF};
  header.num_channels = 3;
  header.channels = channels;
  header.pixel_types = pixelTypes;
  header.requested_pixel_types = storedTypes;
  header.compression_type = TINYEXR_COMPRESSIONTYPE_ZIP;

  unsigned char *mem = nullptr;
  const char *msg = nullptr;
  const size_t bytes = SaveEXRImageToMemory(&image, &header, &mem, &msg);
  if (!bytes || !mem) {
    setError(err, msg, QStringLiteral("cannot encode EXR"));
    return {};
  }
  QByteArray result(reinterpret_cast<const char *>(mem), qsizetype(bytes));
  std::free(mem);
  return result;
}

QImage tonemapped(const Image &img, const int w, const int h) {
  if (img.isNull() || w <= 0 || h <= 0)
    return {};
  const size_t n = size_t(img.w) * img.h;
  double logSum = 0.0;
  for (size_t i = 0; i < n; ++i) {
    const float *p = &img.rgb[i * 3];
    const double l = 0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2];
    logSum += std::log(1e-4 + l);
  }
  const double key = 0.18 / std::max(1e-4, std::exp(logSum / double(n)));
  const auto encode = [](double v) {
    v = v / (1.0 + v);
    v = v <= 0.0031308 ? 12.92 * v : 1.055 * std::pow(v, 1.0 / 2.4) - 0.055;
    return int(std::clamp(v, 0.0, 1.0) * 255.0 + 0.5);
  };
  QImage out(w, h, QImage::Format_RGB888);
  for (int y = 0; y < h; ++y) {
    uchar *row = out.scanLine(y);
    const int y0 = int(qint64(y) * img.h / h), y1 = std::max(y0 + 1, int(qint64(y + 1) * img.h / h));
    for (int x = 0; x < w; ++x) {
      const int x0 = int(qint64(x) * img.w / w), x1 = std::max(x0 + 1, int(qint64(x + 1) * img.w / w));
      double s[3] = {0, 0, 0};
      for (int sy = y0; sy < y1; ++sy)
        for (int sx = x0; sx < x1; ++sx) {
          const float *p = &img.rgb[(size_t(sy) * img.w + sx) * 3];
          s[0] += p[0];
          s[1] += p[1];
          s[2] += p[2];
        }
      const double c = double(y1 - y0) * double(x1 - x0) / key;
      row[x * 3 + 0] = uchar(encode(s[0] / c));
      row[x * 3 + 1] = uchar(encode(s[1] / c));
      row[x * 3 + 2] = uchar(encode(s[2] / c));
    }
  }
  return out;
}

QImage toRgba16F(const Image &img) {
  if (img.isNull())
    return {};
  QImage out(img.w, img.h, QImage::Format_RGBA16FPx4);
  for (int y = 0; y < img.h; ++y) {
    auto *row = reinterpret_cast<qfloat16 *>(out.scanLine(y));
    const float *src = &img.rgb[size_t(y) * img.w * 3];
    for (int x = 0; x < img.w; ++x) {
      row[x * 4 + 0] = qfloat16(src[x * 3 + 0]);
      row[x * 4 + 1] = qfloat16(src[x * 3 + 1]);
      row[x * 4 + 2] = qfloat16(src[x * 3 + 2]);
      row[x * 4 + 3] = qfloat16(1.0f);
    }
  }
  return out;
}

} // namespace hdr
