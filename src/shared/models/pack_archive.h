#pragma once

// An asset pack as a zip: "<pack>.json" at the root (the same JSON the asset server keeps in data/textures) and the
// pack's files in the folder its "path" names (default: its name). Checking one reads only the zip's directory and
// the JSON, so it is cheap for any archive size. Used by the editor before uploading and by the server on install.

#include <QJsonObject>
#include <QString>
#include <QVector>

#include "shared/lib/zip_reader.h"

struct PackArchive {
  bool ok = false;
  QString error;

  QString name;
  QString type;  // "texture" or "lut"
  QString description;
  QJsonObject json;       // the pack JSON as found in the zip
  QString jsonEntry;      // its name in the zip
  QString folder;         // the pack folder in the zip ("path")
  QVector<ZipEntry> files;  // the pack folder's files (the only entries that get extracted)
  int assets = 0;         // textures / skyboxes / LUTs found by file name
  quint64 bytes = 0;      // uncompressed size of the JSON and `files`
};

PackArchive inspectPackArchive(const QString &zipPath);
PackArchive inspectPackArchive(ZipReader &zip);

// The pack's type as shown to people ("Texture pack", "LUT pack").
QString packTypeLabel(const QString &type);
