#pragma once

// A small HTTP/1.1 server on QTcpServer, with WebSocket upgrades handed to QWebSocketServer.
//
// Handlers return an HttpResponse or a QFuture<HttpResponse>; the response is written on the server's thread when the
// future is ready, so heavy work belongs on QtConcurrent. Each connection keeps one request in flight (keep-alive,
// responses in order). Patterns are paths whose "<arg>" segments match one percent-decoded segment each.

#include <QFuture>
#include <QList>
#include <QObject>
#include <QStringList>

#include <functional>
#include <optional>
#include <type_traits>

#include "server/web/http/http_types.h"

class QTcpServer;
class QTcpSocket;
class QWebSocket;
class QWebSocketServer;

class HttpServer final : public QObject {
  Q_OBJECT

public:
  using Handler = std::function<QFuture<HttpResponse>(const HttpRequest &, const QStringList &args)>;
  using WebSocketVerifier = std::function<bool(const HttpRequest &)>;
  // Uploads: the body streams into a file in uploadDir(), never into memory. `admit` sees the request head and may
  // answer instead (auth, size); the handler gets the complete file and owns it.
  using UploadAdmit = std::function<std::optional<HttpResponse>(const HttpRequest &, const QStringList &args)>;
  using UploadHandler =
      std::function<QFuture<HttpResponse>(const HttpRequest &, const QStringList &args, const QString &file)>;
  struct UploadRoute {
    HttpMethod method;
    QStringList segments;
    UploadAdmit admit;
    UploadHandler handler;
  };

  explicit HttpServer(QObject *parent = nullptr);
  ~HttpServer() override;

  bool listen(const QHostAddress &address, quint16 port);
  void close();

  template <typename F>
  void route(HttpMethod method, const QString &pattern, F &&handler) {
    using R = std::invoke_result_t<F &, const HttpRequest &, const QStringList &>;
    if constexpr (std::is_same_v<R, HttpResponse>)
      addRoute(method, pattern, [h = std::forward<F>(handler)](const HttpRequest &req, const QStringList &args) {
        return QtFuture::makeReadyValueFuture(h(req, args));
      });
    else
      addRoute(method, pattern, Handler(std::forward<F>(handler)));
  }

  void uploadRoute(HttpMethod method, const QString &pattern, UploadAdmit admit, UploadHandler handler);
  void setUploadDir(const QString &dir) { m_uploadDir = dir; }
  [[nodiscard]] QString uploadDir() const { return m_uploadDir; }
  [[nodiscard]] const UploadRoute *uploadFor(const HttpRequest &request, QStringList *args) const;

  // GET requests to `path` that ask for a WebSocket upgrade; the verifier decides (false = 401).
  void setWebSocketRoute(const QString &path, WebSocketVerifier verifier);

  // Used by the connections.
  [[nodiscard]] QFuture<HttpResponse> dispatch(const HttpRequest &request) const;
  [[nodiscard]] bool isWebSocketPath(const QString &path) const { return !m_wsPath.isEmpty() && path == m_wsPath; }
  [[nodiscard]] bool verifyWebSocket(const HttpRequest &request) const { return !m_wsVerify || m_wsVerify(request); }
  void upgrade(QTcpSocket *socket);

signals:
  // An accepted WebSocket; the receiver owns it.
  void newWebSocket(QWebSocket *socket);

private:
  struct Route {
    HttpMethod method;
    QStringList segments;
    Handler handler;
  };
  void addRoute(HttpMethod method, const QString &pattern, Handler handler);
  void onNewConnection();

  QList<Route> m_routes;
  QList<UploadRoute> m_uploads;
  QString m_uploadDir;
  QTcpServer *m_tcp = nullptr;
  QWebSocketServer *m_ws = nullptr;
  QString m_wsPath;
  WebSocketVerifier m_wsVerify;
};
