#pragma once
#include <QObject>
#include <QDirIterator>
#include <QDebug>
#include <QFileInfo>

namespace gs {
  extern QString configRoot;
  extern QString homeDir;
  extern QString configDirectory;
  extern QString configDirectoryAssets;
  extern QString configDirectoryAssetsTextures;
  extern QString configDirectoryAssetsModels;
  extern QString cacheDirectory;
  extern QFileInfo pathDatabase;
}

struct PackedFile {
  QString fileName;
  QByteArray data;
};
