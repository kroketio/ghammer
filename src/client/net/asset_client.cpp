#include "asset_client.h"

#include <QCoreApplication>
#include <QHostInfo>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QtDebug>

#include "client/net/local_server.h"
#include "client/prefs/preferences.h"

namespace net {

namespace {

constexpr int kPingMs = 20000;
constexpr int kPongTimeoutMs = 12000;
constexpr int kMaxBackoffMs = 30000;
constexpr int kConnectTimeoutMs = 10000;

QString envUser() {
  QString u = qEnvironmentVariable("USER");
  if (u.isEmpty())
    u = qEnvironmentVariable("USERNAME");
  return u.isEmpty() ? QStringLiteral("editor") : u;
}

} // namespace

Result Result::failure(const QString &code, const QString &message) {
  Result r;
  r.ok = false;
  r.code = code;
  r.message = message;
  return r;
}

AssetClient &AssetClient::instance() {
  static AssetClient *c = new AssetClient();  // lives for the whole process
  return *c;
}

AssetClient::AssetClient() : QObject(nullptr) {
  m_reconnect.setSingleShot(true);
  connect(&m_reconnect, &QTimer::timeout, this, &AssetClient::openSocket);
  m_ping.setInterval(kPingMs);
  connect(&m_ping, &QTimer::timeout, this, [this] {
    if (!m_ws || m_ws->state() != QAbstractSocket::ConnectedState)
      return;
    m_ws->ping();
    if (!m_pongWatch.isActive())
      m_pongWatch.start();
  });
  m_pongWatch.setSingleShot(true);
  m_pongWatch.setInterval(kPongTimeoutMs);
  connect(&m_pongWatch, &QTimer::timeout, this, [this] {
    qWarning() << "[server] no pong - reconnecting";
    closeSocket(true);
  });
  m_connectWatch.setSingleShot(true);
  m_connectWatch.setInterval(kConnectTimeoutMs);
  connect(&m_connectWatch, &QTimer::timeout, this, [this] {
    qWarning().noquote() << "[server] no answer from" << m_wsUrl.toString() << "- retrying";
    m_lastError = tr("No answer from the server");
    closeSocket(true);
  });
}

QString AssetClient::displayName() const {
  const QString n = prefs::s("Server/DisplayName").trimmed();
  if (!n.isEmpty())
    return n;
  return QStringLiteral("%1@%2").arg(envUser(), QHostInfo::localHostName());
}

void AssetClient::start() {
  if (m_started)
    return;
  m_started = true;
  // EDITOR_SERVER_URL overrides the preferences for one run without saving them, and starts no server.
  if (const QString over = qEnvironmentVariable("EDITOR_SERVER_URL"); !over.isEmpty()) {
    setEndpoint(over, QString(), QString());
    return;
  }
  for (const char *key: {"Server/Mode", "Server/Url", "Server/User", "Server/Password"})
    prefs::get().watch(QString::fromLatin1(key), this, [this](const QVariant &) { applyPrefs(); });
  auto &local = LocalServer::instance();
  // Local mode: a relaunch picks a new port; the server only answers once ready.
  connect(&local, &LocalServer::stateChanged, this, [this] {
    if (m_local)
      applyPrefs();
  });
  connect(&local, &LocalServer::ready, this, [this] {
    if (!m_local)
      return;
    applyPrefs();
    if (!m_ws)
      reconnectNow();
  });
}

void AssetClient::applyPrefs() {
  auto &local = LocalServer::instance();
  m_local = prefs::s("Server/Mode") != QLatin1String("remote");
  if (m_local) {
    local.start();  // no-op while running
    if (local.isActive() && !local.url().isEmpty()) {
      setEndpoint(local.url().toString(), local.user(), local.password());
    } else {
      // Not running (missing binary, failed, scripted run): stay offline.
      m_lastError = local.lastError();
      setEndpoint(QString(), QString(), QString());
    }
  } else {
    local.stop();  // no-op when not running
    setEndpoint(prefs::s("Server/Url"), prefs::s("Server/User").trimmed(), prefs::s("Server/Password"));
  }
}

QString AssetClient::offlineDetail() const {
  if (m_local) {
    const auto &local = LocalServer::instance();
    if (!local.lastError().isEmpty())
      return local.lastError();
    return m_lastError;
  }
  return m_http.isEmpty() ? tr("No server address") : m_http.toString();
}

QNetworkRequest AssetClient::makeRequest(const QUrl &url) const {
  QNetworkRequest req(url);
  if (!m_authHeader.isEmpty())
    req.setRawHeader("Authorization", m_authHeader);
  return req;
}

void AssetClient::setEndpoint(const QString &text, const QString &user, const QString &password) {
  QUrl url;
  if (!text.trimmed().isEmpty()) {
    url = QUrl::fromUserInput(text.trimmed());
    if (url.port() < 0 && url.scheme() == QLatin1String("http") && !text.contains(QLatin1String(":80")))
      url.setPort(3000);
    // Kept: a server behind a reverse proxy may live under a path (https://host/ghammer).
    QString base = url.path();
    while (base.endsWith(QLatin1Char('/')))
      base.chop(1);
    url.setPath(base);
    url.setQuery(QString());
    url.setFragment(QString());
  }
  const QByteArray auth = user.isEmpty()
      ? QByteArray()
      : QByteArrayLiteral("Basic ") + (user + QLatin1Char(':') + password).toUtf8().toBase64();
  if (url == m_http && auth == m_authHeader && m_state != State::Offline)
    return;
  const bool changed = url != m_http || auth != m_authHeader;
  m_authHeader = auth;
  m_http = url;
  m_wsUrl = url;
  m_wsUrl.setScheme(url.scheme() == QLatin1String("https") ? QStringLiteral("wss") : QStringLiteral("ws"));
  m_wsUrl.setPath(url.path() + QStringLiteral("/ws"));
  if (url.isEmpty())
    m_wsUrl = QUrl();
  m_backoffMs = 1000;
  closeSocket(false);
  if (changed) {
    if (!url.isEmpty())
      qInfo().noquote() << "[server] using" << m_http.toString() << (m_local ? "(local)" : "")
                        << (m_authHeader.isEmpty() ? "" : "with credentials");
    m_lastRev = 0;  // a different server: nothing we hold is comparable
    m_forceResync = true;
    failAll(QStringLiteral("offline"), tr("Server changed"));
    emit endpointChanged();
  }
  openSocket();
}

void AssetClient::reconnectNow() {
  m_backoffMs = 1000;
  openSocket();
}

void AssetClient::setState(State s) {
  if (s == m_state)
    return;
  m_state = s;
  emit stateChanged(s);
}

void AssetClient::openSocket() {
  closeSocket(false);
  m_authFailed = false;
  if (!m_wsUrl.isValid() || m_wsUrl.host().isEmpty()) {
    setState(State::Offline);
    return;
  }
  if (m_local) {
    const LocalServer::State ls = LocalServer::instance().state();
    if (ls != LocalServer::State::Running) {
      setState(ls == LocalServer::State::Starting ? State::Connecting : State::Offline);
      return;
    }
  }
  auto *ws = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);
  m_ws = ws;
  connect(ws, &QWebSocket::connected, this, &AssetClient::onConnected);
  connect(ws, &QWebSocket::textMessageReceived, this, &AssetClient::onText);
  connect(ws, &QWebSocket::pong, this, [this] { m_pongWatch.stop(); });
  connect(ws, &QWebSocket::stateChanged, this, [this, ws](QAbstractSocket::SocketState st) {
    if (st == QAbstractSocket::UnconnectedState && ws == m_ws)
      closeSocket(true);
  });
  connect(ws, &QWebSocket::errorOccurred, this, [this, ws](QAbstractSocket::SocketError) {
    if (ws != m_ws)
      return;
    m_lastError = ws->errorString();
    if (m_lastError.contains(QLatin1String("401"))) {
      m_authFailed = true;
      m_lastError = m_authHeader.isEmpty() ? tr("The server requires a username and password")
                                           : tr("Wrong username or password");
    }
  });
  setState(State::Connecting);
  m_connectWatch.start();
  ws->open(makeRequest(m_wsUrl));
}

void AssetClient::scheduleReconnect() {
  if (m_reconnect.isActive())
    return;
  m_reconnect.start(m_backoffMs);
  m_backoffMs = std::min(kMaxBackoffMs, m_backoffMs * 2);
}

void AssetClient::onConnected() {
  qInfo().noquote() << "[server] connected to" << m_wsUrl.toString();
  m_ping.start();
  sendHello();
}

void AssetClient::closeSocket(const bool retry) {
  m_reconnect.stop();
  m_connectWatch.stop();
  m_ping.stop();
  m_pongWatch.stop();
  if (QWebSocket *ws = m_ws) {
    m_ws = nullptr;
    ws->disconnect(this);
    ws->abort();
    ws->deleteLater();
  }
  const bool wasUp = m_state == State::Online || m_state == State::Loading;
  if (wasUp && retry)
    qWarning().noquote() << "[server] disconnected" << (m_lastError.isEmpty() ? QString() : m_lastError);
  if (retry) {
    if (!m_authFailed)
      scheduleReconnect();
    setState(m_local && LocalServer::instance().state() == LocalServer::State::Starting ? State::Connecting
                                                                                         : State::Offline);
  }
  // Calls not yet answered cannot be answered by a new connection.
  QList<int> sent;
  for (auto it = m_pending.begin(); it != m_pending.end(); ++it)
    if (it->sent && it.key() != m_helloId)
      sent.append(it.key());
  for (int id: sent) {
    Pending p = m_pending.take(id);
    if (p.timer)
      p.timer->deleteLater();
    if (p.fn && (!p.hasContext || p.context))
      p.fn(Result::failure(QStringLiteral("offline"), tr("Lost the connection to the asset server")));
  }
  m_pending.remove(m_helloId);
  m_helloId = 0;
  if (m_clients.size()) {
    m_clients = {};
    emit presenceChanged(m_clients);
  }
}

void AssetClient::sendHello() {
  m_helloId = m_nextId++;
  QJsonObject args;
  args.insert(QStringLiteral("client"), QStringLiteral("level-editor"));
  args.insert(QStringLiteral("version"), QCoreApplication::applicationVersion());
  args.insert(QStringLiteral("name"), displayName());
  args.insert(QStringLiteral("last_rev"), double(m_lastRev));
  QJsonObject msg;
  msg.insert(QStringLiteral("id"), m_helloId);
  msg.insert(QStringLiteral("op"), QStringLiteral("hello"));
  msg.insert(QStringLiteral("args"), args);
  Pending p;
  p.op = QStringLiteral("hello");
  p.sent = true;
  m_pending.insert(m_helloId, p);
  if (m_ws)
    m_ws->sendTextMessage(QString::fromUtf8(QJsonDocument(msg).toJson(QJsonDocument::Compact)));
}

void AssetClient::call(const QString &op, const QJsonObject &args, QObject *context, ResultFn fn, int timeoutMs) {
  if (m_state == State::Offline) {
    if (fn)
      QTimer::singleShot(0, context ? context : this, [fn] {
        fn(Result::failure(QStringLiteral("offline"), tr("The asset server is not connected")));
      });
    return;
  }
  const int id = m_nextId++;
  Pending p;
  p.op = op;
  p.args = args;
  p.context = context;
  p.hasContext = context != nullptr;
  p.fn = std::move(fn);
  p.timer = new QTimer(this);
  p.timer->setSingleShot(true);
  connect(p.timer, &QTimer::timeout, this, [this, id] {
    auto it = m_pending.find(id);
    if (it == m_pending.end())
      return;
    Pending q = m_pending.take(id);
    q.timer->deleteLater();
    if (q.fn && (!q.hasContext || q.context))
      q.fn(Result::failure(QStringLiteral("timeout"), tr("The asset server did not answer (%1)").arg(q.op)));
  });
  p.timer->start(timeoutMs);
  m_pending.insert(id, p);
  if (m_state == State::Online)
    sendRequest(id);
}

void AssetClient::sendRequest(int id) {
  auto it = m_pending.find(id);
  if (it == m_pending.end() || it->sent)
    return;
  QJsonObject msg;
  msg.insert(QStringLiteral("id"), id);
  msg.insert(QStringLiteral("op"), it->op);
  msg.insert(QStringLiteral("args"), it->args);
  if (!m_ws)
    return;
  it->sent = true;
  m_ws->sendTextMessage(QString::fromUtf8(QJsonDocument(msg).toJson(QJsonDocument::Compact)));
}

void AssetClient::failAll(const QString &code, const QString &message) {
  const auto pending = m_pending;
  m_pending.clear();
  for (const auto &p: pending) {
    if (p.timer)
      p.timer->deleteLater();
    if (p.fn && (!p.hasContext || p.context))
      p.fn(Result::failure(code, message));
  }
  m_helloId = 0;
}

void AssetClient::onText(const QString &text) {
  QJsonParseError pe{};
  const QJsonObject msg = QJsonDocument::fromJson(text.toUtf8(), &pe).object();
  if (pe.error != QJsonParseError::NoError)
    return;
  if (msg.contains(QStringLiteral("ev"))) {
    handleEvent(msg);
    return;
  }
  const int id = msg.value(QStringLiteral("re")).toInt();
  auto it = m_pending.find(id);
  if (it == m_pending.end())
    return;
  Pending p = m_pending.take(id);
  if (p.timer)
    p.timer->deleteLater();

  Result r;
  r.ok = msg.value(QStringLiteral("ok")).toBool();
  r.data = msg.value(QStringLiteral("data")).toObject();
  const QJsonObject err = msg.value(QStringLiteral("error")).toObject();
  r.code = err.value(QStringLiteral("code")).toString();
  r.message = err.value(QStringLiteral("message")).toString();
  r.current = err.value(QStringLiteral("current")).toObject();

  if (id == m_helloId) {
    m_helloId = 0;
    if (!r.ok) {
      qWarning() << "[server] hello refused:" << r.message;
      m_lastError = r.message;
      closeSocket(true);
      return;
    }
    m_connectWatch.stop();
    m_lastError.clear();
    m_backoffMs = 1000;
    m_clientId = r.data.value(QStringLiteral("client_id")).toInt();
    m_status = r.data;
    m_clients = r.data.value(QStringLiteral("clients")).toArray();
    const qint64 serverRev = qint64(r.data.value(QStringLiteral("rev")).toDouble());
    const bool resync = r.data.value(QStringLiteral("resync")).toBool();
    for (const auto &ev: r.data.value(QStringLiteral("replay")).toArray())
      handleEvent(ev.toObject());
    m_lastRev = std::max(m_lastRev, serverRev);
    if (resync)
      m_lastRev = serverRev;
    emit statusChanged(m_status);
    emit presenceChanged(m_clients);
    emit locksReset(r.data.value(QStringLiteral("locks")).toArray());
    const bool loading = r.data.value(QStringLiteral("loading")).toBool();
    setState(loading ? State::Loading : State::Online);
    if (resync || m_forceResync)
      emit resyncRequired();
    m_forceResync = false;
    qInfo().noquote() << QStringLiteral("[server] online: %1 textures, rev %2, %3 editor(s) connected%4")
                             .arg(r.data.value(QStringLiteral("textures")).toInt())
                             .arg(serverRev)
                             .arg(m_clients.size())
                             .arg(loading ? QStringLiteral(" (catalogue still loading)") : QString());
    if (!loading)
      for (auto pit = m_pending.begin(); pit != m_pending.end(); ++pit)
        sendRequest(pit.key());
    return;
  }
  if (p.fn && (!p.hasContext || p.context))
    p.fn(r);
}

void AssetClient::handleEvent(const QJsonObject &msg) {
  const QString ev = msg.value(QStringLiteral("ev")).toString();
  const qint64 rev = qint64(msg.value(QStringLiteral("rev")).toDouble());
  if (ev == QLatin1String("client.joined") || ev == QLatin1String("client.left")) {
    m_clients = msg.value(QStringLiteral("data")).toObject().value(QStringLiteral("clients")).toArray();
    emit presenceChanged(m_clients);
  } else if (ev == QLatin1String("catalog.ready")) {
    if (m_state == State::Loading) {
      setState(State::Online);
      for (auto it = m_pending.begin(); it != m_pending.end(); ++it)
        sendRequest(it.key());
      emit resyncRequired();
    }
  }
  if (rev > m_lastRev && !ev.startsWith(QLatin1String("client.")) && !ev.startsWith(QLatin1String("texture.lock")) &&
      !ev.startsWith(QLatin1String("texture.unlock")))
    m_lastRev = rev;
  emit serverEvent(ev, msg);
}

namespace {

void wireReply(QNetworkReply *reply, QObject *owner, QObject *context, AssetClient::ReplyFn fn) {
  QPointer<QObject> ctx(context);
  const bool hasCtx = context != nullptr;
  QObject::connect(reply, &QNetworkReply::finished, owner, [reply, ctx, hasCtx, fn] {
    if (fn && (!hasCtx || ctx))
      fn(reply);
    reply->deleteLater();
  });
}

} // namespace

QNetworkRequest AssetClient::request(const QString &path) const {
  QNetworkRequest req = makeRequest(urlFor(path));
  req.setRawHeader("X-Client-Id", QByteArray::number(m_clientId));
  return req;
}

QUrl AssetClient::urlFor(const QString &path) const {
  QUrl url = m_http;
  const qsizetype q = path.indexOf(QLatin1Char('?'));
  url.setPath(m_http.path() + path.left(q));
  if (q >= 0)
    url.setQuery(path.mid(q + 1));
  return url;
}

void AssetClient::httpPost(const QString &path, const QByteArray &body, QObject *context, ReplyFn fn, int timeoutMs) {
  const QUrl url = urlFor(path);
  QNetworkRequest req = makeRequest(url);
  req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
  req.setTransferTimeout(timeoutMs);
  wireReply(m_nam.post(req, body), this, context, std::move(fn));
}

void AssetClient::httpGet(const QString &path, QObject *context, ReplyFn fn, int timeoutMs) {
  const QUrl url = urlFor(path);
  QNetworkRequest req = makeRequest(url);
  req.setTransferTimeout(timeoutMs);
  wireReply(m_nam.get(req), this, context, std::move(fn));
}

void AssetClient::httpPut(const QString &path, const QByteArray &body, const QHash<QByteArray, QByteArray> &headers,
                          QObject *context, ReplyFn fn, int timeoutMs) {
  const QUrl url = urlFor(path);
  QNetworkRequest req = makeRequest(url);
  req.setTransferTimeout(timeoutMs);
  req.setRawHeader("X-Client-Id", QByteArray::number(m_clientId));
  for (auto it = headers.begin(); it != headers.end(); ++it)
    req.setRawHeader(it.key(), it.value());
  wireReply(m_nam.put(req, body), this, context, std::move(fn));
}

Result AssetClient::resultFromReply(QNetworkReply *reply) {
  Result r;
  const QByteArray body = reply->readAll();
  const QJsonObject o = QJsonDocument::fromJson(body).object();
  if (reply->error() == QNetworkReply::NoError && o.value(QStringLiteral("ok")).toBool(true)) {
    r.ok = true;
    r.data = o;
    return r;
  }
  const QJsonObject err = o.value(QStringLiteral("error")).toObject();
  r.code = err.value(QStringLiteral("code")).toString();
  r.message = err.value(QStringLiteral("message")).toString();
  r.current = err.value(QStringLiteral("current")).toObject();
  if (r.code.isEmpty()) {
    r.code = reply->error() == QNetworkReply::OperationCanceledError ? QStringLiteral("timeout")
                                                                     : QStringLiteral("offline");
    r.message = reply->errorString();
  }
  return r;
}

} // namespace net
