#include "route_v2.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrlQuery>
#include <QtConcurrent>

#include "server/catalog/catalog.h"
#include "server/catalog/derivatives.h"
#include "server/catalog/installer.h"
#include "server/catalog/mutations.h"
#include "server/web/http/http_server.h"
#include "server/web/http_auth.h"
#include "shared/lib/file_packer.h"

namespace RouteV2 {

namespace {

constexpr int kMaxFilesPerPack = 64;

HttpResponse jsonResponse(const QJsonObject &o, int status = 200) {
  return HttpResponse(o, status);
}

HttpResponse errorResponse(const QString &code, const QString &message, int http,
                                  const QJsonObject &current = {}) {
  QJsonObject e;
  e.insert(QStringLiteral("code"), code);
  e.insert(QStringLiteral("message"), message);
  if (!current.isEmpty())
    e.insert(QStringLiteral("current"), current);
  QJsonObject o;
  o.insert(QStringLiteral("ok"), false);
  o.insert(QStringLiteral("error"), e);
  return jsonResponse(o, http);
}

QString headerValue(const HttpRequest &req, const char *name) {
  return QString::fromUtf8(req.header(QByteArrayView(name))).trimmed();
}

} // namespace

void install(HttpServer *server) {
  // Derivative files (thumbnails / 512 px editor maps); names come from the catalogue, never a request path.
  server->route(HttpMethod::Post, QStringLiteral("/api/2/files"), [](const HttpRequest &request, const QStringList &) {
    const QByteArray body = request.body;  // copied: the request dies with this call
    const bool authorized = httpAuthorized(request);
    return QtConcurrent::run([body, authorized] {
      if (!authorized)
        return unauthorizedResponse();
      const QJsonObject obj = QJsonDocument::fromJson(body).object();
      const bool thumb = obj.value(QStringLiteral("kind")).toString() == QLatin1String("thumb");
      const QJsonArray files = obj.value(QStringLiteral("files")).toArray();
      if (files.size() > kMaxFilesPerPack)
        return errorResponse(QStringLiteral("invalid"), QStringLiteral("at most %1 files per request").arg(kMaxFilesPerPack),
                             400);
      QStringList paths;
      for (const auto &v: files) {
        DerivativeSource src;
        if (!Catalog::instance().derivative(v.toString(), src) || src.thumb != thumb)
          continue;
        if (DerivativeBuilder::instance().ensure(src))
          paths.append(DerivativeBuilder::instance().pathFor(src));
      }
      return HttpResponse(QByteArrayLiteral("application/octet-stream"), packFiles(paths));
    });
  });

  server->route(HttpMethod::Get, QStringLiteral("/api/2/source/<arg>/<arg>/<arg>"),
                [](const HttpRequest &req, const QStringList &args) {
                  const QString pack = args.value(0), name = args.value(1), type = args.value(2);
                  const bool authorized = httpAuthorized(req);
                  return QtConcurrent::run([pack, name, type, authorized] {
                    if (!authorized)
                      return unauthorizedResponse();
                    MapType t;
                    if (!mapTypeFromKey(type, t))
                      return errorResponse(QStringLiteral("invalid"), QStringLiteral("unknown map type"), 400);
                    const TextureEntry e = Catalog::instance().entry(MaterialRef::make(pack, name), t == MapType::Lut);
                    const MapFile *f = e.map(t);
                    if (!f)
                      return errorResponse(QStringLiteral("not_found"), QStringLiteral("no such map"), 404);
                    QFile file(Catalog::instance().sourcePath(e.ref, t));
                    if (!file.open(QIODevice::ReadOnly))
                      return errorResponse(QStringLiteral("io"), QStringLiteral("cannot read the source"), 500);
                    const QByteArray mime = f->file.endsWith(QLatin1String(".png"), Qt::CaseInsensitive)
                                                ? QByteArrayLiteral("image/png")
                                            : f->file.endsWith(QLatin1String(".exr"), Qt::CaseInsensitive)
                                                ? QByteArrayLiteral("image/x-exr")
                                                : QByteArrayLiteral("image/jpeg");
                    HttpResponse r(mime, file.readAll());
                    r.headers().append(QByteArrayLiteral("X-Rev"), QByteArray::number(e.rev));
                    r.headers().append(QByteArrayLiteral("X-File-Name"), f->file.toUtf8());
                    r.headers().append(QByteArrayLiteral("X-Checksum"), f->checksum.toUtf8());
                    return r;
                  });
                });

  server->route(HttpMethod::Put, QStringLiteral("/api/2/source/<arg>/<arg>/<arg>"),
                [](const HttpRequest &req, const QStringList &args) {
                  const QString pack = args.value(0), name = args.value(1), type = args.value(2);
                  if (!httpAuthorized(req))
                    return QtFuture::makeReadyValueFuture(unauthorizedResponse());
                  const QByteArray body = req.body;
                  const int clientId = headerValue(req, "X-Client-Id").toInt();
                  const QString ifMatch = headerValue(req, "If-Match");
                  const qint64 ifRev = ifMatch.isEmpty() ? -1 : ifMatch.toLongLong();
                  const ClientRef by{clientId, Mutations::instance().clientName(clientId)};
                  return Mutations::instance()
                      .uploadMap(MaterialRef::make(pack, name), type, body, ifRev, by)
                      .then([](const MutationResult &r) {
                        if (!r.ok)
                          return errorResponse(r.code, r.message, r.httpStatus, r.current);
                        QJsonObject o = r.data;
                        o.insert(QStringLiteral("ok"), true);
                        return jsonResponse(o);
                      });
                });

  // Asset packs (zips): GET with ?name=&type=&size= checks first, POST uploads and installs.
  server->route(HttpMethod::Get, QStringLiteral("/api/2/packs/install"), [](const HttpRequest &req, const QStringList &) {
    if (!httpAuthorized(req))
      return unauthorizedResponse();
    const QUrlQuery q(req.url);
    const MutationResult r = PackInstall::precheck(q.queryItemValue(QStringLiteral("name")),
                                                   q.queryItemValue(QStringLiteral("type")),
                                                   q.queryItemValue(QStringLiteral("size")).toLongLong());
    if (!r.ok)
      return errorResponse(r.code, r.message, r.httpStatus);
    return jsonResponse(QJsonObject{{QStringLiteral("ok"), true}});
  });
  server->setUploadDir(PackInstall::incomingDir());
  server->uploadRoute(
      HttpMethod::Post, QStringLiteral("/api/2/packs/install"),
      [](const HttpRequest &req, const QStringList &) -> std::optional<HttpResponse> {
        if (!httpAuthorized(req))
          return unauthorizedResponse();
        if (!Catalog::instance().isLoaded())
          return errorResponse(QStringLiteral("loading"), QStringLiteral("the catalogue is still loading"), 503);
        if (!PackInstall::hasRoomFor(req.header("Content-Length").toLongLong()))
          return errorResponse(QStringLiteral("no_space"), QStringLiteral("not enough disk space on the asset server"),
                               507);
        return std::nullopt;
      },
      [](const HttpRequest &req, const QStringList &, const QString &file) {
        const int clientId = headerValue(req, "X-Client-Id").toInt();
        const ClientRef by{clientId, Mutations::instance().clientName(clientId)};
        return PackInstall::install(file, by).then([](const MutationResult &r) {
          if (!r.ok)
            return errorResponse(r.code, r.message, r.httpStatus);
          QJsonObject o;
          o.insert(QStringLiteral("ok"), true);
          o.insert(QStringLiteral("pack"), r.data);
          return jsonResponse(o);
        });
      });

  server->route(HttpMethod::Put, QStringLiteral("/api/2/lut/<arg>/<arg>"),
                [](const HttpRequest &req, const QStringList &args) {
                  if (!httpAuthorized(req))
                    return QtFuture::makeReadyValueFuture(unauthorizedResponse());
                  QJsonObject grade;
                  if (const QByteArray g = req.header("X-Lut-Grade"); !g.isEmpty()) {
                    QJsonParseError pe{};
                    grade = QJsonDocument::fromJson(QByteArray::fromBase64(g), &pe).object();
                    if (pe.error != QJsonParseError::NoError)
                      return QtFuture::makeReadyValueFuture(
                          errorResponse(QStringLiteral("invalid"), QStringLiteral("X-Lut-Grade is not base64 JSON"), 400));
                  }
                  const int clientId = headerValue(req, "X-Client-Id").toInt();
                  const QString ifMatch = headerValue(req, "If-Match");
                  const qint64 ifRev = ifMatch.isEmpty() ? -1 : ifMatch.toLongLong();
                  const ClientRef by{clientId, Mutations::instance().clientName(clientId)};
                  return Mutations::instance()
                      .putLut(args.value(0), args.value(1), req.body, grade, ifRev, by)
                      .then([](const MutationResult &r) {
                        if (!r.ok)
                          return errorResponse(r.code, r.message, r.httpStatus, r.current);
                        QJsonObject o = r.data;
                        o.insert(QStringLiteral("ok"), true);
                        return jsonResponse(o);
                      });
                });
}

} // namespace RouteV2
