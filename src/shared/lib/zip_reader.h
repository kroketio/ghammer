#pragma once

// Reads zip archives without loading them: the central directory comes from the end of the file, and one entry at a
// time is read or streamed out. Zip64 (archives and entries over 4 GB), stored and deflated entries; nothing else.

#include <QFile>
#include <QString>
#include <QVector>

#include <functional>

class QIODevice;

struct ZipEntry {
  QString name;  // as stored ('/' separated)
  quint64 compressedSize = 0;
  quint64 size = 0;
  quint16 method = 0;  // 0 stored, 8 deflate
  quint32 crc = 0;
  quint64 localHeaderOffset = 0;
  bool encrypted = false;
  [[nodiscard]] bool isDir() const { return name.endsWith(QLatin1Char('/')); }
};

class ZipReader {
public:
  ZipReader() = default;
  explicit ZipReader(const QString &path) { open(path); }

  bool open(const QString &path);
  [[nodiscard]] bool isOpen() const { return m_ok; }
  [[nodiscard]] QString error() const { return m_error; }
  [[nodiscard]] const QVector<ZipEntry> &entries() const { return m_entries; }

  // The whole entry, refused when bigger than `maxBytes`.
  bool readFile(const ZipEntry &entry, QByteArray &out, qint64 maxBytes);
  // Streams the entry into `out`; `progress` gets bytes written and returns false to stop.
  bool extract(const ZipEntry &entry, QIODevice &out, const std::function<bool(qint64)> &progress = {});

private:
  bool fail(const QString &message);
  bool readCentralDirectory();
  bool seekToData(const ZipEntry &entry);

  QFile m_file;
  bool m_ok = false;
  QString m_error;
  QVector<ZipEntry> m_entries;
};
