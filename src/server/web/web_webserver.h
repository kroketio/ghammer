#pragma once
#include <QObject>
#include <QString>

class HttpServer;
class RealtimeHub;

class WebServer : public QObject {
  Q_OBJECT

public:
  explicit WebServer(QObject *parent = nullptr);
  ~WebServer() override;

  // configure host/port before start()
  void setHost(const QString &host);
  void setPort(quint16 port);

public slots:
  bool start();
  void stop();

signals:
  void started();
  void failed(const QString &reason);

private:
  HttpServer *m_server = nullptr;
  QString m_host = QStringLiteral("0.0.0.0");
  quint16 m_port = 3000;
  RealtimeHub *m_hub = nullptr;

  void registerRoutes();
};
