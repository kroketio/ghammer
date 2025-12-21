#pragma once

// The WebSocket control channel (ws://host:3000/ws): JSON requests, replies and
// live events for every connected level editor. Lives on the web server thread
// and is the only code that touches a QWebSocket.
//
//   request  {"id": 17, "op": "textures.query", "args": {...}}
//   reply    {"re": 17, "ok": true, "data": {...}}
//            {"re": 17, "ok": false, "error": {"code", "message", "current"?}}
//   event    {"ev": "texture.updated", "rev": 1234, "by": {...}, "data": {...}}
//
// Reads run on QtConcurrent and post their reply back here; writes go through
// Mutations (one thread, total order), which broadcasts its events through us.

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QWebSocket>

#include <map>
#include <memory>

class RealtimeHub : public QObject {
  Q_OBJECT

public:
  explicit RealtimeHub(QObject *parent = nullptr);
  ~RealtimeHub() override;

  static RealtimeHub *instance();

  void addClient(std::unique_ptr<QWebSocket> ws);

  // Thread-safe: queued onto the hub thread.
  void broadcast(const QJsonObject &event);
  void sendTo(int clientId, const QJsonObject &msg);

  static constexpr int kProtocolVersion = 1;

private:
  struct Client {
    int id = 0;
    std::unique_ptr<QWebSocket> ws;
    QString name;
    QString app;
    QString peer;
    bool greeted = false;
  };

  void onText(int clientId, const QString &text);
  void handleHello(Client &c, const QJsonValue &reqId, const QJsonObject &args);
  void dispatch(Client &c, const QJsonValue &reqId, const QString &op, const QJsonObject &args);
  void reply(int clientId, const QJsonValue &reqId, const QJsonObject &data);
  void replyError(int clientId, const QJsonValue &reqId, const QString &code, const QString &message,
                  const QJsonObject &current = {});
  void sendRaw(Client &c, const QJsonObject &msg);
  void removeClient(int clientId);
  [[nodiscard]] QJsonArray clientsJson() const;
  [[nodiscard]] QJsonObject statusJson() const;

  std::map<int, Client> m_clients;
  int m_nextId = 1;
};
