#include "web_webserver.h"

#include <QHostAddress>
#include <QWebSocket>

#include "realtime_hub.h"
#include "routes/route_v2.h"
#include "server/globals.h"
#include "server/web/http/http_server.h"
#include "server/web/http_auth.h"

WebServer::WebServer(QObject *parent) : QObject(parent), m_server(new HttpServer(this)) {
  registerRoutes();
}

void WebServer::setHost(const QString &host) { m_host = host; }
void WebServer::setPort(quint16 port) { m_port = port; }

bool WebServer::start() {
  if (!m_server->listen(QHostAddress(m_host), m_port)) {
    emit failed(tr("Server failed to listen on %1:%2").arg(m_host).arg(m_port));
    return false;
  }
  emit started();
  return true;
}

void WebServer::registerRoutes() {
  m_server->route(HttpMethod::Get, QStringLiteral("/"), [](const HttpRequest &request, const QStringList &) {
    if (!httpAuthorized(request))
      return unauthorizedResponse();
    return HttpResponse(QStringLiteral("gHammer asset server"));
  });

  RouteV2::install(m_server);

  // WebSocket control channel on the same port: ws://host:port/ws
  m_hub = new RealtimeHub(this);
  m_server->setWebSocketRoute(QStringLiteral("/ws"), [](const HttpRequest &request) { return httpAuthorized(request); });
  connect(m_server, &HttpServer::newWebSocket, this,
          [this](QWebSocket *socket) { m_hub->addClient(std::unique_ptr<QWebSocket>(socket)); });
}

void WebServer::stop() {
  m_server->close();
}

WebServer::~WebServer() {
  stop();
}
