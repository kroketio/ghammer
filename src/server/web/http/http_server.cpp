#include "http_server.h"

#include <QTcpServer>
#include <QTcpSocket>
#include <QWebSocket>
#include <QWebSocketServer>

#include "server/web/http/http_connection.h"

namespace {

QStringList pathSegments(const QString &path) {
  return path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
}

bool matchPath(const QStringList &pattern, const QStringList &path, QStringList *args) {
  if (pattern.size() != path.size())
    return false;
  args->clear();
  for (qsizetype i = 0; i < path.size(); ++i) {
    if (pattern[i] == QLatin1String("<arg>"))
      args->append(QUrl::fromPercentEncoding(path[i].toUtf8()));
    else if (pattern[i] != path[i])
      return false;
  }
  return true;
}

HttpResponse errorResponse(int status, const char *code, const char *message) {
  QJsonObject e;
  e.insert(QStringLiteral("code"), QString::fromLatin1(code));
  e.insert(QStringLiteral("message"), QString::fromLatin1(message));
  QJsonObject o;
  o.insert(QStringLiteral("ok"), false);
  o.insert(QStringLiteral("error"), e);
  return HttpResponse(o, status);
}

} // namespace

HttpServer::HttpServer(QObject *parent)
    : QObject(parent), m_tcp(new QTcpServer(this)),
      m_ws(new QWebSocketServer(QStringLiteral("ghammer"), QWebSocketServer::NonSecureMode, this)) {
  connect(m_tcp, &QTcpServer::newConnection, this, &HttpServer::onNewConnection);
  connect(m_ws, &QWebSocketServer::newConnection, this, [this] {
    while (m_ws->hasPendingConnections()) {
      QWebSocket *socket = m_ws->nextPendingConnection();
      socket->setParent(nullptr);
      emit newWebSocket(socket);
    }
  });
}

HttpServer::~HttpServer() {
  close();
}

bool HttpServer::listen(const QHostAddress &address, quint16 port) {
  return m_tcp->listen(address, port);
}

void HttpServer::close() {
  if (m_tcp->isListening())
    m_tcp->close();
}

void HttpServer::addRoute(HttpMethod method, const QString &pattern, Handler handler) {
  m_routes.append({method, pathSegments(pattern), std::move(handler)});
}

void HttpServer::uploadRoute(HttpMethod method, const QString &pattern, UploadAdmit admit, UploadHandler handler) {
  m_uploads.append({method, pathSegments(pattern), std::move(admit), std::move(handler)});
}

const HttpServer::UploadRoute *HttpServer::uploadFor(const HttpRequest &request, QStringList *args) const {
  const QStringList path = pathSegments(request.url.path(QUrl::FullyEncoded));
  for (const UploadRoute &r: m_uploads)
    if (r.method == request.method && matchPath(r.segments, path, args))
      return &r;
  return nullptr;
}

void HttpServer::setWebSocketRoute(const QString &path, WebSocketVerifier verifier) {
  m_wsPath = path;
  m_wsVerify = std::move(verifier);
}

void HttpServer::onNewConnection() {
  while (m_tcp->hasPendingConnections()) {
    QTcpSocket *socket = m_tcp->nextPendingConnection();
    socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
    new HttpConnection(socket, this);
  }
}

void HttpServer::upgrade(QTcpSocket *socket) {
  m_ws->handleConnection(socket);
}

QFuture<HttpResponse> HttpServer::dispatch(const HttpRequest &request) const {
  const QStringList path = pathSegments(request.url.path(QUrl::FullyEncoded));
  bool pathMatched = false;
  for (const Route &r: m_routes) {
    QStringList args;
    if (!matchPath(r.segments, path, &args))
      continue;
    pathMatched = true;
    if (r.method != request.method)
      continue;
    try {
      return r.handler(request, args);
    } catch (const std::exception &) {
      return QtFuture::makeReadyValueFuture(errorResponse(500, "io", "internal error"));
    }
  }
  if (pathMatched)
    return QtFuture::makeReadyValueFuture(errorResponse(405, "invalid", "method not allowed"));
  return QtFuture::makeReadyValueFuture(errorResponse(404, "not_found", "no such route"));
}
