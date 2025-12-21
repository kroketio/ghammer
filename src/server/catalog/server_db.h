#pragma once

// sqlite access for the catalogue. QSqlDatabase connections are thread-bound, so every thread gets its own named
// connection to the same file (main thread, derivative workers, the mutation thread).

#include <QHash>
#include <QSqlDatabase>
#include <QString>

#include "shared/models/material_props.h"

namespace serverdb {

// Must be called once before anything else (main thread).
bool init(const QString &path);

// This thread's connection (opened on first use).
QSqlDatabase db();

// Cached header/decode info of a source image, valid while size + mtime match.
struct ImageInfo {
  qint64 size = 0;
  qint64 mtime = 0;
  int w = 0;
  int h = 0;
  bool alphaChannel = false;  // the file has an alpha channel (header)
  int alphaReal = -1;         // -1 unknown, 0 opaque, 1 has transparent pixels (decode)
  int channels = 0;
};

QHash<QString, ImageInfo> loadImageInfo();  // keyed by path relative to data/textures
void storeImageInfo(const QHash<QString, ImageInfo> &items);
void storeImageInfo(const QString &relPath, const ImageInfo &info);

// Server-side material property overrides, keyed by texture uuid.
struct PropsRow {
  QString ref;
  MaterialProps props;
  qint64 rev = 0;
};
QHash<QString, PropsRow> loadProps();
bool storeProps(const QString &textureId, const QString &ref, const MaterialProps &props, qint64 rev);
bool deleteProps(const QString &textureId);

// Small key/value table (catalogue revision etc.).
qint64 metaInt(const QString &key, qint64 fallback = 0);
void setMetaInt(const QString &key, qint64 value);

} // namespace serverdb
