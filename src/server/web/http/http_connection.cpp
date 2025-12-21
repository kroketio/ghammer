#include "http_connection.h"

#include <QDir>
#include <QJsonObject>
#include <QTcpSocket>
#include <QUuid>

#include "server/web/http/http_server.h"

namespace {

constexpr qint64 kMaxHead = 64 * 1024;
constexpr qint64 kMaxBody = qint64(1) << 30;
constexpr int kIdleMs = 60000;

HttpMethod methodFrom(const QByteArray &m) {
  if (m == "GET")
    return HttpMethod::Get;
  if (m == "HEAD")
    return HttpMethod::Head;
  if (m == "POST")
    return HttpMethod::Post;
  if (m == "PUT")
    return HttpMethod::Put;
  if (m == "DELETE")
    return HttpMethod::Delete;
  if (m == "OPTIONS")
    return HttpMethod::Options;
  if (m == "PATCH")
    return HttpMethod::Patch;
  return HttpMethod::Unknown;
}

QByteArray reasonPhrase(int status) {
  switch (status) {
    case 100: return "Continue";
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 304: return "Not Modified";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 411: return "Length Required";
    case 412: return "Precondition Failed";
    case 413: return "Content Too Large";
    case 423: return "Locked";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 503: return "Service Unavailable";
    case 507: return "Insufficient Storage";
    default: return status < 400 ? "OK" : "Error";
  }
}

bool hasToken(const QByteArray &headerValue, const char *token) {
  for (const QByteArray &part: headerValue.split(','))
    if (part.trimmed().compare(token, Qt::CaseInsensitive) == 0)
      return true;
  return false;
}

} // namespace

HttpConnection::HttpConnection(QTcpSocket *socket, HttpServer *server)
    : QObject(server), m_socket(socket), m_server(server) {
  m_idle.setSingleShot(true);
  m_idle.setInterval(kIdleMs);
  connect(&m_idle, &QTimer::timeout, this, [this] {
    if (m_socket)
      m_socket->disconnectFromHost();
  });
  connect(socket, &QTcpSocket::readyRead, this, &HttpConnection::process);
  connect(socket, &QTcpSocket::disconnected, this, &HttpConnection::deleteLater);
  m_idle.start();
  process();
}

HttpConnection::~HttpConnection() {
  if (m_upload) {
    m_upload->remove();
    delete m_upload;
  }
  if (m_socket)
    m_socket->deleteLater();
}

void HttpConnection::process() {
  while (m_socket && m_state != State::Busy) {
    if (m_state == State::Discard) {
      m_received += m_socket->read(m_socket->bytesAvailable()).size();
      if (m_received >= m_bodyLength)
        m_socket->disconnectFromHost();
      return;
    }
    if (m_state == State::Upload) {
      readUpload();
      return;
    }
    if (m_state == State::Head) {
      if (!readHead())
        return;
    }
    if (m_state == State::Body) {
      const qint64 want = m_bodyLength - m_request.body.size();
      if (want > 0) {
        m_request.body.append(m_socket->read(want));
        m_idle.start();
        if (m_request.body.size() < m_bodyLength)
          return;
      }
      finishRequest();
    }
  }
}

// The request line and headers, peeked first: a WebSocket upgrade leaves the socket untouched for QWebSocketServer.
bool HttpConnection::readHead() {
  const QByteArray peeked = m_socket->peek(std::min<qint64>(m_socket->bytesAvailable(), kMaxHead + 4));
  const qsizetype end = peeked.indexOf("\r\n\r\n");
  if (end < 0) {
    if (peeked.size() > kMaxHead)
      fail(431);
    return false;
  }
  m_idle.start();
  const QList<QByteArray> lines = peeked.left(end).split('\n');
  const QList<QByteArray> start = lines.value(0).trimmed().split(' ');
  if (start.size() != 3 || !start[2].startsWith("HTTP/1.")) {
    fail(400);
    return false;
  }
  HttpRequest req;
  req.method = methodFrom(start[0]);
  req.url = QUrl::fromEncoded(start[1]);
  req.remoteAddress = m_socket->peerAddress();
  for (qsizetype i = 1; i < lines.size(); ++i) {
    const QByteArray line = lines[i].trimmed();
    const qsizetype colon = line.indexOf(':');
    if (colon <= 0)
      continue;
    req.headers.append(line.left(colon).trimmed(), line.mid(colon + 1).trimmed());
  }
  const bool http10 = start[2] == "HTTP/1.0";
  const QByteArray connection = req.header("Connection");
  m_keepAlive = http10 ? hasToken(connection, "keep-alive") : !hasToken(connection, "close");

  if (req.method == HttpMethod::Get && m_server->isWebSocketPath(req.url.path()) &&
      hasToken(req.header("Upgrade"), "websocket")) {
    if (!m_server->verifyWebSocket(req)) {
      m_keepAlive = false;
      m_socket->read(end + 4);
      HttpResponse denied(QByteArrayLiteral("text/plain; charset=utf-8"), QByteArrayLiteral("Unauthorized"), 401);
      denied.headers().append(QHttpHeaders::WellKnownHeader::WWWAuthenticate,
                              QByteArrayLiteral("Basic realm=\"ghammer\", charset=\"UTF-8\""));
      respond(denied);
      return false;
    }
    release();
    return false;
  }

  m_socket->read(end + 4);
  if (req.method == HttpMethod::Unknown) {
    fail(501);
    return false;
  }
  if (!req.header("Transfer-Encoding").isEmpty()) {
    fail(411);
    return false;
  }
  m_bodyLength = 0;
  const QByteArray contentLength = req.header("Content-Length");
  if (!contentLength.isEmpty()) {
    bool ok = false;
    m_bodyLength = contentLength.toLongLong(&ok);
    if (!ok || m_bodyLength < 0) {
      fail(400);
      return false;
    }
  }
  if (QStringList args; const HttpServer::UploadRoute *up = m_server->uploadFor(req, &args)) {
    if (contentLength.isEmpty() || m_bodyLength == 0) {
      fail(411);
      return false;
    }
    if (auto deny = up->admit(req, args)) {
      m_request = std::move(req);
      rejectUpload(*deny);
      return false;
    }
    m_request = std::move(req);
    return beginUpload(up, args);
  }
  if (m_bodyLength > kMaxBody) {
    fail(413);
    return false;
  }
  if (m_bodyLength > 0 && hasToken(req.header("Expect"), "100-continue"))
    m_socket->write("HTTP/1.1 100 Continue\r\n\r\n");
  m_head = req.method == HttpMethod::Head;
  if (m_head)
    req.method = HttpMethod::Get;
  m_request = std::move(req);
  m_request.body.reserve(m_bodyLength);
  m_state = State::Body;
  return true;
}

void HttpConnection::finishRequest() {
  m_state = State::Busy;
  m_idle.stop();
  QFuture<HttpResponse> future = m_server->dispatch(m_request);
  m_request = HttpRequest();
  if (future.isFinished() && future.resultCount() > 0) {
    respond(future.result());
    return;
  }
  future
      .then(this, [this](const HttpResponse &response) { respond(response); })
      .onFailed(this, [this] { respond(HttpResponse(500)); })
      .onCanceled(this, [this] { respond(HttpResponse(500)); });
}

void HttpConnection::respond(const HttpResponse &response) {
  if (!m_socket)
    return;
  QByteArray out;
  out.reserve(256 + (m_head ? 0 : response.body().size()));
  out += "HTTP/1.1 " + QByteArray::number(response.status()) + ' ' + reasonPhrase(response.status()) + "\r\n";
  const QHttpHeaders &h = response.headers();
  for (qsizetype i = 0; i < h.size(); ++i) {
    const QLatin1StringView nameView = h.nameAt(i);
    const QByteArray name(nameView.data(), nameView.size());
    if (name.compare("content-length", Qt::CaseInsensitive) == 0 || name.compare("connection", Qt::CaseInsensitive) == 0)
      continue;
    out += name + ": " + h.valueAt(i).toByteArray() + "\r\n";
  }
  out += "Content-Length: " + QByteArray::number(response.body().size()) + "\r\n";
  out += m_keepAlive ? "Connection: keep-alive\r\n\r\n" : "Connection: close\r\n\r\n";
  m_socket->write(out);
  if (!m_head && !response.body().isEmpty())
    m_socket->write(response.body());
  m_head = false;
  m_state = State::Head;
  if (!m_keepAlive) {
    disconnect(m_socket, &QTcpSocket::readyRead, this, nullptr);
    m_state = State::Busy;
    m_socket->disconnectFromHost();
    return;
  }
  m_idle.start();
  if (m_socket->bytesAvailable() > 0)
    QMetaObject::invokeMethod(this, &HttpConnection::process, Qt::QueuedConnection);
}

void HttpConnection::fail(int status) {
  m_keepAlive = false;
  QJsonObject e;
  e.insert(QStringLiteral("code"), QStringLiteral("http"));
  e.insert(QStringLiteral("message"), QString::fromLatin1(reasonPhrase(status)));
  QJsonObject o;
  o.insert(QStringLiteral("ok"), false);
  o.insert(QStringLiteral("error"), e);
  m_state = State::Busy;
  respond(HttpResponse(o, status));
}

bool HttpConnection::beginUpload(const HttpServer::UploadRoute *route, const QStringList &args) {
  QDir().mkpath(m_server->uploadDir());
  m_upload = new QFile(QDir(m_server->uploadDir())
                           .absoluteFilePath(QStringLiteral("upload-%1.part")
                                                 .arg(QUuid::createUuid().toString(QUuid::Id128))));
  if (!m_upload->open(QIODevice::WriteOnly)) {
    delete m_upload;
    m_upload = nullptr;
    rejectUpload(HttpResponse(500));
    return false;
  }
  if (hasToken(m_request.header("Expect"), "100-continue"))
    m_socket->write("HTTP/1.1 100 Continue\r\n\r\n");
  m_socket->setReadBufferSize(qint64(4) << 20);
  m_uploadRoute = route;
  m_uploadArgs = args;
  m_received = 0;
  m_state = State::Upload;
  return true;
}

void HttpConnection::readUpload() {
  while (m_socket->bytesAvailable() > 0 && m_received < m_bodyLength) {
    const QByteArray chunk = m_socket->read(std::min<qint64>(m_bodyLength - m_received, qint64(1) << 20));
    if (chunk.isEmpty())
      break;
    if (m_upload->write(chunk) != chunk.size()) {
      m_upload->remove();
      delete m_upload;
      m_upload = nullptr;
      QJsonObject e{{QStringLiteral("code"), QStringLiteral("no_space")},
                    {QStringLiteral("message"), QStringLiteral("cannot store the upload (disk full?)")}};
      rejectUpload(HttpResponse(QJsonObject{{QStringLiteral("ok"), false}, {QStringLiteral("error"), e}}, 507));
      return;
    }
    m_received += chunk.size();
    m_idle.start();
  }
  if (m_received < m_bodyLength)
    return;
  m_socket->setReadBufferSize(0);
  m_upload->close();
  const QString path = m_upload->fileName();
  delete m_upload;
  m_upload = nullptr;
  m_state = State::Busy;
  m_idle.stop();
  QFuture<HttpResponse> future;
  try {
    future = m_uploadRoute->handler(m_request, m_uploadArgs, path);
  } catch (const std::exception &) {
    QFile::remove(path);
    future = QtFuture::makeReadyValueFuture(HttpResponse(500));
  }
  m_request = HttpRequest();
  m_uploadRoute = nullptr;
  future
      .then(this, [this](const HttpResponse &response) { respond(response); })
      .onFailed(this, [this] { respond(HttpResponse(500)); })
      .onCanceled(this, [this] { respond(HttpResponse(500)); });
}

// Answers an upload without reading its body, then drops what is still coming (for a few seconds, so the client reads
// the answer instead of a reset connection) and closes.
void HttpConnection::rejectUpload(const HttpResponse &response) {
  m_keepAlive = false;
  m_state = State::Busy;
  m_head = false;
  if (!m_socket)
    return;
  QByteArray out = "HTTP/1.1 " + QByteArray::number(response.status()) + ' ' + reasonPhrase(response.status()) + "\r\n";
  const QHttpHeaders &h = response.headers();
  for (qsizetype i = 0; i < h.size(); ++i) {
    const QLatin1StringView n = h.nameAt(i);
    out += QByteArray(n.data(), n.size()) + ": " + h.valueAt(i).toByteArray() + "\r\n";
  }
  out += "Content-Length: " + QByteArray::number(response.body().size()) + "\r\nConnection: close\r\n\r\n";
  m_socket->write(out + response.body());
  m_received = 0;
  m_state = State::Discard;
  m_idle.stop();
  QTimer::singleShot(5000, this, [this] {
    if (m_socket)
      m_socket->disconnectFromHost();
  });
  process();
}

// The socket now belongs to the WebSocket server.
void HttpConnection::release() {
  QTcpSocket *socket = m_socket;
  m_socket = nullptr;
  m_idle.stop();
  socket->disconnect(this);
  m_server->upgrade(socket);
  deleteLater();
}
