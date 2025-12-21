#pragma once

// Connection to the asset (texture) server: JSON (queries, CRUD, live events) over one WebSocket (ws://<host>/ws),
// images over HTTP. Reconnects with backoff and sends its last seen revision so missed events are replayed (or a
// resync requested). Server/Mode "local" = net::LocalServer (127.0.0.1, per-run credentials), "remote" = Server/Url +
// Server/User / Server/Password; HTTP Basic whenever there are credentials. Callbacks run on the main thread and are
// dropped when `context` dies first.

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QUrl>
#include <QWebSocket>

#include <functional>

class QNetworkReply;
class QNetworkRequest;

namespace net {

struct Result {
  bool ok = false;
  QJsonObject data;
  QString code;  // offline | timeout | read_only | conflict | not_found | locked | invalid | exists | io | loading
  QString message;
  QJsonObject current;  // conflicts: the server's current version

  static Result failure(const QString &code, const QString &message);
};

using ResultFn = std::function<void(const Result &)>;

class AssetClient final : public QObject {
  Q_OBJECT

public:
  enum class State { Offline, Connecting, Loading, Online };
  Q_ENUM(State)

  static AssetClient &instance();

  // Follows the Server/Mode preference (and remote URL / credentials), starting / stopping the local server.
  // EDITOR_SERVER_URL overrides it for one run.
  void start();
  void reconnectNow();

  [[nodiscard]] State state() const { return m_state; }
  [[nodiscard]] bool isOnline() const { return m_state == State::Online; }
  [[nodiscard]] QUrl httpBase() const { return m_http; }
  [[nodiscard]] QString serverUrlText() const { return m_http.toString(); }
  // Connected (or connecting) to the editor's own server.
  [[nodiscard]] bool isLocal() const { return m_local; }
  // One line on why it is offline (the local server's problem, or the unanswering remote address).
  [[nodiscard]] QString offlineDetail() const;
  [[nodiscard]] int clientId() const { return m_clientId; }
  [[nodiscard]] QString displayName() const;
  [[nodiscard]] QJsonArray clients() const { return m_clients; }
  [[nodiscard]] QJsonObject status() const { return m_status; }
  [[nodiscard]] QString lastError() const { return m_lastError; }

  // While connecting the call waits (up to the timeout); while offline it fails at once with code "offline".
  void call(const QString &op, const QJsonObject &args, QObject *context, ResultFn fn, int timeoutMs = 15000);

  // HTTP (bytes). The callback gets the finished reply (deleted afterwards).
  using ReplyFn = std::function<void(QNetworkReply *)>;
  void httpPost(const QString &path, const QByteArray &body, QObject *context, ReplyFn fn, int timeoutMs = 30000);
  void httpGet(const QString &path, QObject *context, ReplyFn fn, int timeoutMs = 60000);
  void httpPut(const QString &path, const QByteArray &body, const QHash<QByteArray, QByteArray> &headers,
               QObject *context, ReplyFn fn, int timeoutMs = 120000);
  static Result resultFromReply(QNetworkReply *reply);
  // A request to the server (`path` under its base URL) with its credentials and this editor's client id.
  [[nodiscard]] QNetworkRequest request(const QString &path) const;

signals:
  void stateChanged(net::AssetClient::State state);
  // Every server event: {"ev", "rev", "by", "data"}.
  void serverEvent(const QString &ev, const QJsonObject &event);
  // The journal could not cover our last revision: re-fetch what we hold.
  void resyncRequired();
  void presenceChanged(const QJsonArray &clients);
  void statusChanged(const QJsonObject &status);
  // Initial lock list after (re)connecting: [{ref, by, purpose}].
  void locksReset(const QJsonArray &locks);
  // Another server or credentials: fetched data is stale; resyncRequired() follows once the new one answers.
  void endpointChanged();

private:
  AssetClient();
  void setState(State s);
  void openSocket();
  void scheduleReconnect();
  void onConnected();
  void closeSocket(bool retry);
  void onText(const QString &text);
  void sendHello();
  void sendRequest(int id);
  void failAll(const QString &code, const QString &message);
  void handleEvent(const QJsonObject &msg);
  void applyPrefs();
  void setEndpoint(const QString &url, const QString &user, const QString &password);
  [[nodiscard]] QNetworkRequest makeRequest(const QUrl &url) const;
  [[nodiscard]] QUrl urlFor(const QString &path) const;

  struct Pending {
    QString op;
    QJsonObject args;
    QPointer<QObject> context;
    bool hasContext = false;
    ResultFn fn;
    QTimer *timer = nullptr;
    bool sent = false;
  };

  QWebSocket *m_ws = nullptr;
  QNetworkAccessManager m_nam;
  QUrl m_http;
  QUrl m_wsUrl;
  QByteArray m_authHeader;  // "Basic …", empty without credentials
  bool m_local = false;
  bool m_forceResync = false;
  State m_state = State::Offline;
  bool m_started = false;
  int m_nextId = 1;
  int m_helloId = 0;
  int m_clientId = 0;
  qint64 m_lastRev = 0;
  int m_backoffMs = 1000;
  QTimer m_reconnect;
  QTimer m_ping;
  QTimer m_pongWatch;
  QTimer m_connectWatch;
  bool m_authFailed = false;
  QHash<int, Pending> m_pending;
  QJsonArray m_clients;
  QJsonObject m_status;
  QString m_lastError;
};

} // namespace net
