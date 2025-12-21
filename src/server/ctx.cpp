#include <filesystem>

#include <QObject>
#include <QDir>
#include <QSemaphore>
#include <QHostAddress>
#include <QNetworkInterface>

#include <cstdio>

#include "server/catalog/installer.h"
#include "shared/lib/stb/stb_image.h"
#include "shared/lib/config.h"
#include "shared/lib/globals.h"
#include "shared/lib/utils.h"

#include "server/ctx.h"
#include "server/globals.h"
#include "server/catalog/catalog.h"
#include "server/catalog/derivatives.h"
#include "server/catalog/server_db.h"
#include "server/web/realtime_hub.h"

Ctx::Ctx() {
  g_instance = this;
  Utils::init();

  stbi_set_flip_vertically_on_load(true);

  config()->set(ConfigKeys::CacheDir, gs::cacheDirectory);

  createConfigDirectory(QStringList({
    gs::configDirectory,
    gs::cacheDirectory,
    gs::configDirectoryAssets,
    gs::configDirectoryAssetsTextures,
    gs::configDirectoryAssetsModels}));

  if (!serverdb::init(gs::pathDatabase.absoluteFilePath()))
    throw QString("Cannot open catalogue db at %1").arg(gs::pathDatabase.absoluteFilePath()).toStdString();

  // web server: up first, so editors connect (and see "loading") right away
  start_webserver();

  // texture catalogue (API v2). Read-only scan of data/textures.
  if (g::derivativeThreads > 0)
    DerivativeBuilder::instance().setThreadCount(g::derivativeThreads);
  const auto missing = Catalog::instance().load(gs::configDirectoryAssetsTextures, gs::cacheDirectory, gs::configDirectory);
  DerivativeBuilder::instance().collectGarbage(Catalog::instance().derivativeNames());
  DerivativeBuilder::instance().enqueue(missing);
  PackInstall::cleanIncoming();
  if (auto *hub = RealtimeHub::instance()) {
    QJsonObject ev;
    ev.insert("ev", "catalog.ready");
    ev.insert("rev", double(Catalog::instance().revision()));
    QJsonObject d;
    d.insert("textures", Catalog::instance().textureCount());
    ev.insert("data", d);
    hub->broadcast(ev);
  }

}

void Ctx::createConfigDirectory(const QStringList &lst) {
  for(const auto &d: lst) {
    if(!std::filesystem::exists(d.toStdString())) {
      qDebug() << QString("Creating directory: %1").arg(d);
      if(!QDir().mkpath(d))
        throw std::runtime_error("Could not create directory " + d.toStdString());
    }
  }
}

namespace {

// Where editors can reach us: the bound address, or every IPv4 address of
// this machine when bound to all of them.
void logListening() {
  const QString auth = g::authUser.isEmpty() ? QStringLiteral("off") : QStringLiteral("on");
  const QHostAddress bound(g::host);
  QStringList hosts;
  if (bound == QHostAddress::AnyIPv4 || bound == QHostAddress::Any || bound == QHostAddress::AnyIPv6) {
    for (const QHostAddress &a: QNetworkInterface::allAddresses())
      if (a.protocol() == QAbstractSocket::IPv4Protocol && !a.isLoopback())
        hosts << a.toString();
    hosts.prepend(QStringLiteral("127.0.0.1"));
  } else {
    hosts << (bound.protocol() == QAbstractSocket::IPv6Protocol ? QStringLiteral("[%1]").arg(g::host) : g::host);
  }
  qInfo().noquote() << QStringLiteral("[server] listening on %1:%2 (HTTP + ws /ws), auth %3")
                           .arg(g::host).arg(g::port).arg(auth);
  for (const QString &h: hosts)
    qInfo().noquote() << QStringLiteral("[server]   http://%1:%2").arg(h).arg(g::port);
  if (hosts.size() > 1 && g::authUser.isEmpty())
    qWarning().noquote() << "[server] reachable from the network without authentication (--auth user:password)";
}

} // namespace

void Ctx::start_webserver() {
  // Constructed *inside* its thread: HttpServer creates its WebSocket server in its constructor and the sockets
  // inherit that thread. Built on the main thread and moved, the hub could not write to them.
  m_web_thread = new QThread();
  m_web_thread->setObjectName(QString("webserver"));
  QSemaphore ready;
  connect(m_web_thread, &QThread::started, [this, &ready]() {  // direct: runs in the web thread
    web_server = new WebServer();
    web_server->setHost(g::host);
    web_server->setPort(g::port);
    if (!web_server->start()) {
      qWarning().noquote() << QStringLiteral("[server] could not listen on %1:%2").arg(g::host).arg(g::port);
      QMetaObject::invokeMethod(QCoreApplication::instance(), [] { QCoreApplication::exit(1); }, Qt::QueuedConnection);
    } else {
      logListening();
      // The editor that started us waits for this line (machine-readable,
      // independent of the log format).
      if (g::parentPid > 0) {
        fprintf(stdout, "GHAMMER_SERVER_READY %u\n", unsigned(g::port));
        fflush(stdout);
      }
    }
    ready.release();
  });

  connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, [this] {
    if (web_server)
      QMetaObject::invokeMethod(web_server, &WebServer::stop, Qt::BlockingQueuedConnection);
    m_web_thread->quit();
    m_web_thread->wait();
    delete web_server;  // its thread has finished
    web_server = nullptr;
    m_web_thread->deleteLater();
  });

  m_web_thread->start();
  ready.acquire();
}

Ctx::~Ctx() {}

Ctx* g_instance = nullptr;