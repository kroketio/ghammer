#pragma once

#include <QFile>
#include <QObject>
#include <QPointer>
#include <QTimer>

#include "server/web/http/http_server.h"
#include "server/web/http/http_types.h"

class QTcpSocket;

// One accepted socket: parses requests, dispatches them one at a time, writes the responses. Deletes itself (and the
// socket) when the peer goes away, or hands the socket to the WebSocket server on an upgrade.
class HttpConnection final : public QObject {
  Q_OBJECT

public:
  HttpConnection(QTcpSocket *socket, HttpServer *server);
  ~HttpConnection() override;

private:
  enum class State { Head, Body, Upload, Discard, Busy };

  void process();
  bool readHead();
  void finishRequest();
  void respond(const HttpResponse &response);
  void fail(int status);
  void release();
  bool beginUpload(const HttpServer::UploadRoute *route, const QStringList &args);
  void readUpload();
  void rejectUpload(const HttpResponse &response);

  QPointer<QTcpSocket> m_socket;
  HttpServer *m_server = nullptr;
  State m_state = State::Head;
  HttpRequest m_request;
  qint64 m_bodyLength = 0;
  bool m_keepAlive = true;
  bool m_head = false;  // HEAD: no body in the response
  QTimer m_idle;
  const HttpServer::UploadRoute *m_uploadRoute = nullptr;
  QStringList m_uploadArgs;
  QFile *m_upload = nullptr;  // incomplete upload (removed when the connection dies)
  qint64 m_received = 0;
};
