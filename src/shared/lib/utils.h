#pragma once

#include <QObject>
#include <QProcess>
#include <QFileInfo>
#include <QSettings>
#include <QRegularExpression>
#include <QStandardItemModel>
#include <QTcpSocket>
#include <QTextCharFormat>
#include <QImageReader>
#include <algorithm>
#include <chrono>
#include <thread>
#include <iostream>

#include "shared/lib/globals.h"

class Utils
{

public:
  static void init();
  static bool readJsonFile(QIODevice &device, QSettings::SettingsMap &map);
  static bool writeJsonFile(QIODevice &device, const QSettings::SettingsMap &map);

  static bool isEqual(float a, float b);
  static bool isEqual(QVector3D a, QVector3D b);
};
