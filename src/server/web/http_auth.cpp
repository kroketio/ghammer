#include "http_auth.h"

#include <QByteArray>
#include <QHttpHeaders>
#include <QJsonObject>

#include "server/globals.h"

namespace {

// Compares without returning early on the first different byte.
bool constantTimeEquals(const QByteArray &a, const QByteArray &b) {
  if (a.size() != b.size())
    return false;
  unsigned char diff = 0;
  for (qsizetype i = 0; i < a.size(); ++i)
    diff |= static_cast<unsigned char>(a[i] ^ b[i]);
  return diff == 0;
}

} // namespace

bool httpAuthorized(const HttpRequest &request) {
  if (g::authUser.isEmpty())
    return true;
  const QByteArray header = request.headers.value(QHttpHeaders::WellKnownHeader::Authorization).toByteArray().trimmed();
  static const QByteArray kBasic = QByteArrayLiteral("basic ");
  if (header.size() <= kBasic.size() || header.left(kBasic.size()).toLower() != kBasic)
    return false;
  const auto decoded = QByteArray::fromBase64Encoding(header.mid(kBasic.size()).trimmed(),
                                                      QByteArray::AbortOnBase64DecodingErrors);
  if (!decoded)
    return false;
  const QByteArray expected = (g::authUser + QLatin1Char(':') + g::authPassword).toUtf8();
  return constantTimeEquals(*decoded, expected);
}

HttpResponse unauthorizedResponse() {
  QJsonObject e;
  e.insert(QStringLiteral("code"), QStringLiteral("unauthorized"));
  e.insert(QStringLiteral("message"), QStringLiteral("authentication required"));
  QJsonObject o;
  o.insert(QStringLiteral("ok"), false);
  o.insert(QStringLiteral("error"), e);
  HttpResponse r(o, 401);
  r.headers().append(QHttpHeaders::WellKnownHeader::WWWAuthenticate,
                     QByteArrayLiteral("Basic realm=\"ghammer\", charset=\"UTF-8\""));
  return r;
}
