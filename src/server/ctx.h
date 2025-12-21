#pragma once

#include <QObject>
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonArray>
#include <QSqlDatabase>
#include <QTimer>
#include <QDebug>
#include <QQueue>
#include <QThread>
#include <QMutex>
#include <QDir>

#include "server/web/web_webserver.h"

class Ctx;
extern Ctx* g_instance;

class Ctx final : public QObject {
Q_OBJECT

public:
  explicit Ctx();
  void start_webserver();
  ~Ctx() override;

  WebServer *web_server = nullptr;

  static Ctx* instance() {
    return g_instance;
  }

private:
  QThread* m_web_thread = nullptr;

  static void createConfigDirectory(const QStringList &lst);
};
