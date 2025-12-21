#pragma once

#include <QByteArray>
#include <QHostAddress>
#include <QHttpHeaders>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QUrl>

enum class HttpMethod { Unknown, Get, Head, Post, Put, Delete, Options, Patch };

struct HttpRequest {
  HttpMethod method = HttpMethod::Unknown;
  QUrl url;
  QHttpHeaders headers;
  QByteArray body;
  QHostAddress remoteAddress;

  [[nodiscard]] QByteArray header(QByteArrayView name) const { return headers.value(name).toByteArray(); }
};

class HttpResponse {
public:
  HttpResponse() = default;
  explicit HttpResponse(int status) : m_status(status) {}
  HttpResponse(const QJsonObject &json, int status = 200)
      : m_status(status), m_body(QJsonDocument(json).toJson(QJsonDocument::Compact)) {
    m_headers.append(QHttpHeaders::WellKnownHeader::ContentType, QByteArrayLiteral("application/json"));
  }
  HttpResponse(const QByteArray &mime, const QByteArray &body, int status = 200) : m_status(status), m_body(body) {
    m_headers.append(QHttpHeaders::WellKnownHeader::ContentType, mime);
  }
  HttpResponse(const QString &text, int status = 200) : m_status(status), m_body(text.toUtf8()) {
    m_headers.append(QHttpHeaders::WellKnownHeader::ContentType, QByteArrayLiteral("text/plain; charset=utf-8"));
  }

  [[nodiscard]] int status() const { return m_status; }
  [[nodiscard]] const QHttpHeaders &headers() const { return m_headers; }
  [[nodiscard]] QHttpHeaders &headers() { return m_headers; }
  [[nodiscard]] const QByteArray &body() const { return m_body; }

private:
  int m_status = 200;
  QHttpHeaders m_headers;
  QByteArray m_body;
};
