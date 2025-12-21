#include "zip_reader.h"

#include <QIODevice>

#include <zlib.h>

#include <algorithm>

namespace {

constexpr quint32 kEocdSig = 0x06054b50;
constexpr quint32 kZip64LocatorSig = 0x07064b50;
constexpr quint32 kZip64EocdSig = 0x06064b50;
constexpr quint32 kCentralSig = 0x02014b50;
constexpr quint32 kLocalSig = 0x04034b50;
constexpr qint64 kMaxCentralDirectory = qint64(512) << 20;
constexpr qint64 kChunk = 1 << 20;

quint16 u16(const char *p) {
  return quint16(quint8(p[0]) | quint8(p[1]) << 8);
}
quint32 u32(const char *p) {
  return quint32(u16(p)) | quint32(u16(p + 2)) << 16;
}
quint64 u64(const char *p) {
  return quint64(u32(p)) | quint64(u32(p + 4)) << 32;
}

} // namespace

bool ZipReader::fail(const QString &message) {
  m_error = message;
  return false;
}

bool ZipReader::open(const QString &path) {
  m_ok = false;
  m_error.clear();
  m_entries.clear();
  m_file.close();
  m_file.setFileName(path);
  if (!m_file.open(QIODevice::ReadOnly))
    return fail(QStringLiteral("cannot open the file"));
  if (!readCentralDirectory())
    return false;
  m_ok = true;
  return true;
}

bool ZipReader::readCentralDirectory() {
  const qint64 size = m_file.size();
  if (size < 22)
    return fail(QStringLiteral("not a zip file"));
  const qint64 tailLen = std::min<qint64>(size, 22 + 65535 + 20);
  if (!m_file.seek(size - tailLen))
    return fail(QStringLiteral("cannot read the file"));
  const QByteArray tail = m_file.read(tailLen);
  if (tail.size() != tailLen)
    return fail(QStringLiteral("cannot read the file"));
  qsizetype eocd = -1;
  for (qsizetype i = tail.size() - 22; i >= 0; --i)
    if (u32(tail.constData() + i) == kEocdSig && i + 22 + u16(tail.constData() + i + 20) <= tail.size()) {
      eocd = i;
      break;
    }
  if (eocd < 0)
    return fail(QStringLiteral("not a zip file"));
  const char *e = tail.constData() + eocd;
  if (u16(e + 4) != 0 || u16(e + 6) != 0)
    return fail(QStringLiteral("split zip archives are not supported"));
  quint64 count = u16(e + 10);
  quint64 cdSize = u32(e + 12);
  quint64 cdOffset = u32(e + 16);

  if (eocd >= 20 && u32(tail.constData() + eocd - 20) == kZip64LocatorSig) {
    const quint64 z64 = u64(tail.constData() + eocd - 20 + 8);
    if (!m_file.seek(qint64(z64)))
      return fail(QStringLiteral("damaged zip file"));
    const QByteArray rec = m_file.read(56);
    if (rec.size() != 56 || u32(rec.constData()) != kZip64EocdSig)
      return fail(QStringLiteral("damaged zip file"));
    count = u64(rec.constData() + 32);
    cdSize = u64(rec.constData() + 40);
    cdOffset = u64(rec.constData() + 48);
  }
  if (cdSize > quint64(kMaxCentralDirectory) || cdOffset + cdSize > quint64(size))
    return fail(QStringLiteral("damaged zip file"));
  if (!m_file.seek(qint64(cdOffset)))
    return fail(QStringLiteral("damaged zip file"));
  const QByteArray cd = m_file.read(qint64(cdSize));
  if (quint64(cd.size()) != cdSize)
    return fail(QStringLiteral("damaged zip file"));

  m_entries.reserve(qsizetype(std::min<quint64>(count, 1u << 20)));
  qsizetype pos = 0;
  for (quint64 n = 0; n < count; ++n) {
    if (pos + 46 > cd.size() || u32(cd.constData() + pos) != kCentralSig)
      return fail(QStringLiteral("damaged zip file"));
    const char *h = cd.constData() + pos;
    const quint16 flags = u16(h + 8);
    ZipEntry z;
    z.method = u16(h + 10);
    z.crc = u32(h + 16);
    z.compressedSize = u32(h + 20);
    z.size = u32(h + 24);
    const quint16 nameLen = u16(h + 28), extraLen = u16(h + 30), commentLen = u16(h + 32);
    z.localHeaderOffset = u32(h + 42);
    z.encrypted = flags & 1;
    if (pos + 46 + nameLen + extraLen + commentLen > cd.size())
      return fail(QStringLiteral("damaged zip file"));
    const QByteArray rawName(h + 46, nameLen);
    z.name = (flags & 0x800) ? QString::fromUtf8(rawName) : QString::fromLatin1(rawName);
    const char *x = h + 46 + nameLen;
    for (int i = 0; i + 4 <= extraLen;) {
      const quint16 id = u16(x + i), len = u16(x + i + 2);
      if (i + 4 + len > extraLen)
        break;
      if (id == 0x0001) {
        const char *v = x + i + 4;
        int off = 0;
        auto next = [&](quint64 &field) {
          if (field == 0xFFFFFFFFu && off + 8 <= len) {
            field = u64(v + off);
            off += 8;
          }
        };
        next(z.size);
        next(z.compressedSize);
        next(z.localHeaderOffset);
      }
      i += 4 + len;
    }
    if (z.localHeaderOffset >= quint64(size))
      return fail(QStringLiteral("damaged zip file"));
    m_entries.append(z);
    pos += 46 + nameLen + extraLen + commentLen;
  }
  return true;
}

bool ZipReader::seekToData(const ZipEntry &entry) {
  if (!m_file.seek(qint64(entry.localHeaderOffset)))
    return fail(QStringLiteral("damaged zip file"));
  const QByteArray h = m_file.read(30);
  if (h.size() != 30 || u32(h.constData()) != kLocalSig)
    return fail(QStringLiteral("damaged zip file"));
  const qint64 data = qint64(entry.localHeaderOffset) + 30 + u16(h.constData() + 26) + u16(h.constData() + 28);
  if (quint64(data) + entry.compressedSize > quint64(m_file.size()) || !m_file.seek(data))
    return fail(QStringLiteral("damaged zip file"));
  return true;
}

bool ZipReader::readFile(const ZipEntry &entry, QByteArray &out, qint64 maxBytes) {
  if (entry.size > quint64(maxBytes))
    return fail(QStringLiteral("%1 is too large").arg(entry.name));
  out.clear();
  out.reserve(qsizetype(entry.size));
  struct Sink : QIODevice {
    QByteArray *buf;
    explicit Sink(QByteArray *b) : buf(b) { QIODevice::open(WriteOnly); }
    qint64 readData(char *, qint64) override { return -1; }
    qint64 writeData(const char *d, qint64 n) override {
      buf->append(d, n);
      return n;
    }
  } sink(&out);
  return extract(entry, sink);
}

bool ZipReader::extract(const ZipEntry &entry, QIODevice &out, const std::function<bool(qint64)> &progress) {
  if (!m_ok)
    return false;
  if (entry.encrypted)
    return fail(QStringLiteral("%1 is encrypted").arg(entry.name));
  if (entry.method != 0 && entry.method != 8)
    return fail(QStringLiteral("%1 uses an unsupported compression method").arg(entry.name));
  if (!seekToData(entry))
    return false;

  quint32 crc = crc32(0, nullptr, 0);
  quint64 written = 0;
  quint64 remaining = entry.compressedSize;
  auto emitBytes = [&](const char *data, qint64 n) {
    if (n <= 0)
      return true;
    if (out.write(data, n) != n)
      return fail(QStringLiteral("cannot write %1").arg(entry.name));
    crc = crc32(crc, reinterpret_cast<const Bytef *>(data), uInt(n));
    written += quint64(n);
    if (written > entry.size)
      return fail(QStringLiteral("%1 is damaged").arg(entry.name));
    if (progress && !progress(qint64(written)))
      return fail(QStringLiteral("cancelled"));
    return true;
  };

  if (entry.method == 0) {
    while (remaining > 0) {
      const QByteArray in = m_file.read(qint64(std::min<quint64>(remaining, kChunk)));
      if (in.isEmpty())
        return fail(QStringLiteral("damaged zip file"));
      remaining -= quint64(in.size());
      if (!emitBytes(in.constData(), in.size()))
        return false;
    }
  } else {
    z_stream z{};
    if (inflateInit2(&z, -MAX_WBITS) != Z_OK)
      return fail(QStringLiteral("cannot decompress"));
    QByteArray outBuf(kChunk, Qt::Uninitialized);
    QByteArray in;
    int rc = Z_OK;
    bool ok = true;
    while (ok && rc != Z_STREAM_END) {
      if (z.avail_in == 0) {
        if (remaining == 0) {
          ok = fail(QStringLiteral("%1 is damaged").arg(entry.name));
          break;
        }
        in = m_file.read(qint64(std::min<quint64>(remaining, kChunk)));
        if (in.isEmpty()) {
          ok = fail(QStringLiteral("damaged zip file"));
          break;
        }
        remaining -= quint64(in.size());
        z.next_in = reinterpret_cast<Bytef *>(in.data());
        z.avail_in = uInt(in.size());
      }
      z.next_out = reinterpret_cast<Bytef *>(outBuf.data());
      z.avail_out = uInt(outBuf.size());
      rc = inflate(&z, Z_NO_FLUSH);
      if (rc != Z_OK && rc != Z_STREAM_END) {
        ok = fail(QStringLiteral("%1 is damaged").arg(entry.name));
        break;
      }
      ok = emitBytes(outBuf.constData(), outBuf.size() - qint64(z.avail_out));
    }
    inflateEnd(&z);
    if (!ok)
      return false;
  }
  if (written != entry.size || crc != entry.crc)
    return fail(QStringLiteral("%1 is damaged (checksum)").arg(entry.name));
  return true;
}
