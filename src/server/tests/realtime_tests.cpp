// End-to-end tests of API v2 with two editors connected at once (cmake-build-debug/src/server/server_tests).
// Runs the real server binary on a free port with --config-dir in a temporary directory holding a synthetic base
// pack; the real ~/.config/ghammer is never touched.

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QBuffer>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QProcess>

#include <zlib.h>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QWebSocket>
#include <QtTest/QtTest>

namespace {

// One connected editor.
class Client {
public:
  explicit Client(const QString &name) : m_name(name) {
    QObject::connect(&ws, &QWebSocket::textMessageReceived, [this](const QString &t) {
      const QJsonObject m = QJsonDocument::fromJson(t.toUtf8()).object();
      if (m.contains(QStringLiteral("ev")))
        events.append(m);
      else
        replies.insert(m.value(QStringLiteral("re")).toInt(), m);
    });
  }

  bool open(quint16 port) {
    ws.open(QUrl(QStringLiteral("ws://127.0.0.1:%1/ws").arg(port)));
    return QTest::qWaitFor([this] { return ws.state() == QAbstractSocket::ConnectedState; }, 5000);
  }

  QJsonObject call(const QString &op, const QJsonObject &args = {}, int timeoutMs = 10000) {
    const int id = ++m_id;
    QJsonObject msg;
    msg.insert(QStringLiteral("id"), id);
    msg.insert(QStringLiteral("op"), op);
    msg.insert(QStringLiteral("args"), args);
    ws.sendTextMessage(QString::fromUtf8(QJsonDocument(msg).toJson(QJsonDocument::Compact)));
    QTest::qWaitFor([this, id] { return replies.contains(id); }, timeoutMs);
    return replies.take(id);
  }

  QJsonObject hello(qint64 lastRev = 0) {
    QJsonObject a;
    a.insert(QStringLiteral("client"), QStringLiteral("test"));
    a.insert(QStringLiteral("name"), m_name);
    a.insert(QStringLiteral("last_rev"), double(lastRev));
    return call(QStringLiteral("hello"), a);
  }

  // Waits for an event of type `ev` (optionally matching a predicate).
  QJsonObject waitEvent(const QString &ev, std::function<bool(const QJsonObject &)> pred = {}, int ms = 5000) {
    QJsonObject found;
    QTest::qWaitFor(
        [&] {
          for (int i = 0; i < events.size(); ++i)
            if (events[i].value(QStringLiteral("ev")).toString() == ev && (!pred || pred(events[i]))) {
              found = events.takeAt(i);
              return true;
            }
          return false;
        },
        ms);
    return found;
  }

  QWebSocket ws;
  QList<QJsonObject> events;
  QHash<int, QJsonObject> replies;

private:
  QString m_name;
  int m_id = 0;
};

QJsonObject data(const QJsonObject &reply) { return reply.value(QStringLiteral("data")).toObject(); }
QString errorCode(const QJsonObject &reply) {
  return reply.value(QStringLiteral("error")).toObject().value(QStringLiteral("code")).toString();
}

quint16 freePort() {
  QTcpServer s;
  s.listen(QHostAddress::LocalHost, 0);
  return s.serverPort();
}

// Godot's neutral 3D LUT strip, N slices of N x N.
QImage lutStrip(int n) {
  QImage img(n * n, n, QImage::Format_RGB888);
  for (int b = 0; b < n; ++b)
    for (int g = 0; g < n; ++g)
      for (int r = 0; r < n; ++r)
        img.setPixelColor(b * n + r, g, QColor(r * 255 / (n - 1), g * 255 / (n - 1), b * 255 / (n - 1)));
  return img;
}

QByteArray pngBytes(const QImage &img) {
  QByteArray out;
  QBuffer b(&out);
  b.open(QIODevice::WriteOnly);
  img.save(&b, "PNG");
  return out;
}

// A zip of stored (uncompressed) files.
QByteArray makeZip(const QList<QPair<QString, QByteArray>> &files) {
  QByteArray out, central;
  auto u16 = [](QByteArray &b, quint16 v) { b.append(char(v & 0xff)).append(char(v >> 8)); };
  auto u32 = [&](QByteArray &b, quint32 v) {
    u16(b, quint16(v & 0xffff));
    u16(b, quint16(v >> 16));
  };
  for (const auto &[name, data]: files) {
    const QByteArray n = name.toUtf8();
    const quint32 crc = quint32(crc32(0, reinterpret_cast<const Bytef *>(data.constData()), uInt(data.size())));
    const quint32 offset = quint32(out.size());
    u32(out, 0x04034b50);
    u16(out, 20);
    u16(out, 0x800);
    u16(out, 0);
    u16(out, 0);
    u16(out, 0x21);
    u32(out, crc);
    u32(out, quint32(data.size()));
    u32(out, quint32(data.size()));
    u16(out, quint16(n.size()));
    u16(out, 0);
    out += n + data;
    u32(central, 0x02014b50);
    u16(central, 20);
    u16(central, 20);
    u16(central, 0x800);
    u16(central, 0);
    u16(central, 0);
    u16(central, 0x21);
    u32(central, crc);
    u32(central, quint32(data.size()));
    u32(central, quint32(data.size()));
    u16(central, quint16(n.size()));
    u16(central, 0);
    u16(central, 0);
    u16(central, 0);
    u16(central, 0);
    u32(central, 0);
    u32(central, offset);
    central += n;
  }
  const quint32 cdOffset = quint32(out.size());
  out += central;
  u32(out, 0x06054b50);
  u16(out, 0);
  u16(out, 0);
  u16(out, quint16(files.size()));
  u16(out, quint16(files.size()));
  u32(out, quint32(central.size()));
  u32(out, cdOffset);
  u16(out, 0);
  return out;
}

QByteArray packJson(const QString &name, const QString &type, const QJsonObject &meta = {}) {
  return QJsonDocument(QJsonObject{{"name", name}, {"type", type}, {"meta", meta}}).toJson();
}

void writeImage(const QString &path, const QColor &c, bool alpha = false) {
  QImage img(64, 64, alpha ? QImage::Format_ARGB32 : QImage::Format_RGB32);
  img.fill(c);
  if (alpha)
    for (int y = 0; y < 16; ++y)
      for (int x = 0; x < 64; ++x)
        img.setPixelColor(x, y, QColor(0, 0, 0, 0));
  QVERIFY(img.save(path));
}

} // namespace

class RealtimeTests : public QObject {
  Q_OBJECT

private:
  QTemporaryDir m_dir;
  QProcess m_server;
  quint16 m_port = 0;
  QNetworkAccessManager m_nam;

  bool startServer() {
    m_port = freePort();
    m_server.setProgram(QStringLiteral(SERVER_BINARY));
    m_server.setArguments({QStringLiteral("--port"), QString::number(m_port), QStringLiteral("--config-dir"),
                           m_dir.path(), QStringLiteral("--threads"), QStringLiteral("2")});
    m_server.setProcessChannelMode(QProcess::MergedChannels);
    m_server.start();
    if (!m_server.waitForStarted(5000))
      return false;
    // ready once a client can say hello and the catalogue is loaded
    return QTest::qWaitFor(
        [this] {
          Client probe(QStringLiteral("probe"));
          if (!probe.open(m_port))
            return false;
          return !data(probe.hello()).value(QStringLiteral("loading")).toBool(true);
        },
        15000);
  }

  void stopServer() {
    m_server.terminate();
    if (!m_server.waitForFinished(5000))
      m_server.kill();
  }

  QByteArray httpPost(const QString &path, const QJsonObject &body, int *status = nullptr) {
    QNetworkRequest req(QUrl(QStringLiteral("http://127.0.0.1:%1%2").arg(m_port).arg(path)));
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    QNetworkReply *r = m_nam.post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
    QTest::qWaitFor([r] { return r->isFinished(); }, 10000);
    if (status)
      *status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray b = r->readAll();
    r->deleteLater();
    return b;
  }

  QByteArray httpGet(const QString &path, QNetworkReply **keep = nullptr) {
    QNetworkReply *r = m_nam.get(QNetworkRequest(QUrl(QStringLiteral("http://127.0.0.1:%1%2").arg(m_port).arg(path))));
    QTest::qWaitFor([r] { return r->isFinished(); }, 10000);
    const QByteArray b = r->readAll();
    if (keep)
      *keep = r;
    else
      r->deleteLater();
    return b;
  }

  int httpPut(const QString &path, const QByteArray &body, int clientId, qint64 ifRev, QJsonObject *json = nullptr) {
    QNetworkRequest req(QUrl(QStringLiteral("http://127.0.0.1:%1%2").arg(m_port).arg(path)));
    req.setRawHeader("X-Client-Id", QByteArray::number(clientId));
    if (ifRev >= 0)
      req.setRawHeader("If-Match", QByteArray::number(ifRev));
    QNetworkReply *r = m_nam.put(req, body);
    QTest::qWaitFor([r] { return r->isFinished(); }, 15000);
    const int status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (json)
      *json = QJsonDocument::fromJson(r->readAll()).object();
    r->deleteLater();
    return status;
  }

private slots:
  void initTestCase() {
    QVERIFY(m_dir.isValid());
    // A base pack ("base": true: read-only) of synthetic textures.
    const QString tex = m_dir.filePath(QStringLiteral("data/textures"));
    QVERIFY(QDir().mkpath(tex + QStringLiteral("/basepack")));
    writeImage(tex + QStringLiteral("/basepack/BrickWall_1K_Color.png"), QColor(150, 60, 40));
    writeImage(tex + QStringLiteral("/basepack/BrickWall_1K_NormalGL.png"), QColor(128, 128, 255));
    writeImage(tex + QStringLiteral("/basepack/BrickWall_1K_Roughness.png"), QColor(200, 200, 200));
    writeImage(tex + QStringLiteral("/basepack/BrickWall_1K_Displacement.png"), QColor(90, 90, 90));
    writeImage(tex + QStringLiteral("/basepack/MetalPlate_1K_Color.png"), QColor(120, 120, 130));
    writeImage(tex + QStringLiteral("/basepack/MetalPlate_1K_ARM.png"), QColor(255, 80, 255));
    writeImage(tex + QStringLiteral("/basepack/Leaves_1K_Color.png"), QColor(40, 120, 30), true);
    writeImage(tex + QStringLiteral("/basepack/Leaves_1K_NormalDX.png"), QColor(128, 100, 255));
    writeImage(tex + QStringLiteral("/basepack/Spam367046_1K_Color.png"), QColor(90, 90, 90));
    QJsonObject meta;
    meta.insert(QStringLiteral("BrickWall"), QJsonObject{{"id", "id-brick"}, {"tags", QJsonArray{0, 1}}});
    meta.insert(QStringLiteral("MetalPlate"), QJsonObject{{"id", "id-metal"}, {"tags", QJsonArray{2}}});
    meta.insert(QStringLiteral("Leaves"), QJsonObject{{"id", "id-leaves"}, {"tags", QJsonArray{3, 1}}});
    // tag spam: more than max_tags (40) tags, among them "wall"
    QJsonArray spam;
    for (int i = 0; i < 4; ++i)
      spam.append(i);
    for (int i = 0; i < 45; ++i)
      spam.append(4 + i);
    meta.insert(QStringLiteral("Spam367046"), QJsonObject{{"id", "id-spam"}, {"tags", spam}});
    QJsonObject pack;
    pack.insert(QStringLiteral("name"), QStringLiteral("basepack"));
    pack.insert(QStringLiteral("description"), QStringLiteral("test base pack"));
    pack.insert(QStringLiteral("path"), QStringLiteral("basepack"));
    pack.insert(QStringLiteral("type"), QStringLiteral("texture"));
    pack.insert(QStringLiteral("base"), true);
    QJsonArray tagTable{"bricks", "wall", "metal", "leaves"};
    for (int i = 0; i < 45; ++i)
      tagTable.append(QStringLiteral("junk%1").arg(i));
    pack.insert(QStringLiteral("tags"), tagTable);
    pack.insert(QStringLiteral("meta"), meta);
    QFile f(tex + QStringLiteral("/basepack.json"));
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(QJsonDocument(pack).toJson());
    f.close();
    // A hand-made pack without the key: editable.
    QVERIFY(QDir().mkpath(tex + QStringLiteral("/handmade")));
    QFile h(tex + QStringLiteral("/handmade.json"));
    QVERIFY(h.open(QIODevice::WriteOnly));
    h.write(QJsonDocument(QJsonObject{{"name", "handmade"}, {"path", "handmade"}, {"type", "texture"}, {"meta", QJsonObject()}})
                .toJson());
    h.close();
    // A base colour LUT pack: one 2^3 strip, and a file that is no strip (skipped).
    QVERIFY(QDir().mkpath(tex + QStringLiteral("/baseluts")));
    QVERIFY(lutStrip(2).save(tex + QStringLiteral("/baseluts/Neutral.png")));
    writeImage(tex + QStringLiteral("/baseluts/Broken.png"), QColor(1, 2, 3));
    QFile l(tex + QStringLiteral("/baseluts.json"));
    QVERIFY(l.open(QIODevice::WriteOnly));
    l.write(QJsonDocument(QJsonObject{{"name", "baseluts"}, {"path", "baseluts"}, {"type", "lut"}, {"base", true},
                                      {"meta", QJsonObject{{"Neutral", QJsonObject{{"id", "id-neutral"}}}}}})
                .toJson());
    l.close();
    QVERIFY2(startServer(), m_server.readAll().constData());
  }

  void cleanupTestCase() { stopServer(); }

  void catalogueAndQueries() {
    Client a(QStringLiteral("alice"));
    QVERIFY(a.open(m_port));
    const QJsonObject w = data(a.hello());
    QCOMPARE(w.value(QStringLiteral("textures")).toInt(), 4);
    const QJsonObject packs = data(a.call(QStringLiteral("packs.list")));
    QCOMPARE(packs.value(QStringLiteral("packs")).toArray().size(), 3);
    for (const auto &v: packs.value(QStringLiteral("packs")).toArray()) {
      const QJsonObject p = v.toObject();
      const QString name = p.value(QStringLiteral("name")).toString();
      // only "base": true is read-only
      QCOMPARE(p.value(QStringLiteral("base")).toBool(), name != QLatin1String("handmade"));
      QCOMPARE(p.value(QStringLiteral("type")).toString(),
               name == QLatin1String("baseluts") ? QStringLiteral("lut") : QStringLiteral("texture"));
    }

    QJsonObject q = data(a.call(QStringLiteral("textures.query"), {{"facets", true}}));
    QCOMPARE(q.value(QStringLiteral("total")).toInt(), 4);
    const QJsonObject facetResult = q;
    // tag spam does not match tag filters
    q = data(a.call(QStringLiteral("textures.query"), {{"tags", QJsonArray{"wall"}}}));
    QCOMPARE(q.value(QStringLiteral("total")).toInt(), 2);  // BrickWall, Leaves; not Spam367046
    // a seeded shuffle: the same seed gives the same order on every page
    auto order = [&a](quint32 seed, int offset, int limit) {
      QStringList refs;
      for (const auto &v: data(a.call(QStringLiteral("textures.query"),
                                      {{"sort", "random"}, {"seed", double(seed)}, {"offset", offset}, {"limit", limit}}))
                          .value(QStringLiteral("items")).toArray())
        refs << v.toObject().value(QStringLiteral("ref")).toString();
      return refs;
    };
    const QStringList all = order(7, 0, 4);
    QCOMPARE(all.size(), 4);
    QCOMPARE(order(7, 0, 2) + order(7, 2, 2), all);
    // transparent textures (alpha found while building derivatives)
    QTRY_COMPARE_WITH_TIMEOUT(data(a.call(QStringLiteral("textures.query"), {{"alpha", true}}))
                                  .value(QStringLiteral("total")).toInt(),
                              1, 5000);
    // aliases fold "bricks" into "brick"
    bool brick = false;
    for (const auto &f: facetResult.value(QStringLiteral("facets")).toArray())
      brick = brick || f.toArray().at(0).toString() == QLatin1String("brick");
    QVERIFY(brick);
    q = data(a.call(QStringLiteral("textures.query"), {{"tags", QJsonArray{"wall"}}, {"q", "-leaves"}}));
    QCOMPARE(q.value(QStringLiteral("total")).toInt(), 1);
    const QJsonObject brickEntry = q.value(QStringLiteral("items")).toArray().at(0).toObject();
    QCOMPARE(brickEntry.value(QStringLiteral("ref")).toString(), QStringLiteral("basepack/BrickWall"));
    QCOMPARE(brickEntry.value(QStringLiteral("layout")).toString(), QStringLiteral("separate"));
    QVERIFY(brickEntry.value(QStringLiteral("has_height")).toBool());
    // every map is listed; displacement has no editor derivative
    const QJsonObject maps = brickEntry.value(QStringLiteral("maps")).toObject();
    QVERIFY(maps.value(QStringLiteral("displacement")).toObject().value(QStringLiteral("editor")).isNull());
    QVERIFY(!maps.value(QStringLiteral("color")).toObject().value(QStringLiteral("editor")).toString().isEmpty());

    const QJsonObject r = data(a.call(QStringLiteral("textures.resolve"), {{"refs", QJsonArray{"BASEPACK/brickwall", "leaves", "x/y"}}}));
    QCOMPARE(r.value(QStringLiteral("entries")).toObject().size(), 2);  // case-insensitive + legacy bare name
    QCOMPARE(r.value(QStringLiteral("missing")).toArray().size(), 1);
    QVERIFY(!data(a.call(QStringLiteral("tags.popular"))).value(QStringLiteral("tags")).toArray().isEmpty());
  }

  void filesAndSafety() {
    Client a(QStringLiteral("alice"));
    QVERIFY(a.open(m_port));
    a.hello();
    const QJsonObject e = data(a.call(QStringLiteral("textures.resolve"), {{"refs", QJsonArray{"basepack/Leaves"}}}))
                              .value(QStringLiteral("entries")).toObject().value(QStringLiteral("basepack/Leaves")).toObject();
    const QJsonObject color = e.value(QStringLiteral("maps")).toObject().value(QStringLiteral("color")).toObject();
    // real transparency was detected while building derivatives
    QTRY_VERIFY_WITH_TIMEOUT(data(a.call(QStringLiteral("textures.resolve"), {{"refs", QJsonArray{"basepack/Leaves"}}}))
                                 .value(QStringLiteral("entries")).toObject().value(QStringLiteral("basepack/Leaves"))
                                 .toObject().value(QStringLiteral("alpha")).toBool(),
                             5000);
    const QByteArray pack = httpPost(QStringLiteral("/api/2/files"),
                                     {{"kind", "editor"},
                                      {"files", QJsonArray{color.value(QStringLiteral("editor")), "../../../etc/passwd",
                                                           "/etc/passwd"}}});
    const QJsonArray header = QJsonDocument::fromJson(pack.left(pack.indexOf('\n'))).array();
    QCOMPARE(header.size(), 1);  // only the known derivative, never a path
    QVERIFY(header.at(0).toObject().value(QStringLiteral("fileName")).toString().endsWith(QLatin1String("_512.png")));
    // a base pack refuses every change
    QCOMPARE(errorCode(a.call(QStringLiteral("textures.update"), {{"ref", "basepack/Leaves"}, {"tags", QJsonArray{"x"}}})),
             QStringLiteral("read_only"));
    QCOMPARE(errorCode(a.call(QStringLiteral("packs.delete"), {{"name", "basepack"}})), QStringLiteral("read_only"));
    QJsonObject json;
    QCOMPARE(httpPut(QStringLiteral("/api/2/source/basepack/Leaves/color"), QByteArray("x"), 0, -1, &json), 403);
  }

  void twoEditorsSeeEachOther() {
    Client a(QStringLiteral("alice")), b(QStringLiteral("bob"));
    QVERIFY(a.open(m_port) && b.open(m_port));
    const int aid = data(a.hello()).value(QStringLiteral("client_id")).toInt();
    const int bid = data(b.hello()).value(QStringLiteral("client_id")).toInt();
    QVERIFY(!a.waitEvent(QStringLiteral("client.joined")).isEmpty());

    // A creates a pack and copies a base texture into it; B hears both
    QVERIFY(a.call(QStringLiteral("packs.create"), {{"name", "mymap"}}).value(QStringLiteral("ok")).toBool());
    QVERIFY(!b.waitEvent(QStringLiteral("pack.created")).isEmpty());
    QJsonObject r = a.call(QStringLiteral("textures.copy"), {{"from", "basepack/BrickWall"}, {"pack", "mymap"}});
    QVERIFY2(r.value(QStringLiteral("ok")).toBool(), qPrintable(errorCode(r)));
    const QJsonObject copy = data(r).value(QStringLiteral("entry")).toObject();
    QCOMPARE(copy.value(QStringLiteral("ref")).toString(), QStringLiteral("mymap/BrickWall"));
    QCOMPARE(copy.value(QStringLiteral("derived_from")).toString(), QStringLiteral("basepack/BrickWall"));
    QVERIFY(copy.value(QStringLiteral("maps")).toObject().contains(QStringLiteral("displacement")));  // every map
    const QJsonObject created = b.waitEvent(QStringLiteral("texture.created"));
    QCOMPARE(created.value(QStringLiteral("data")).toObject().value(QStringLiteral("entry")).toObject()
                 .value(QStringLiteral("ref")).toString(),
             QStringLiteral("mymap/BrickWall"));
    QCOMPARE(qint64(created.value(QStringLiteral("rev")).toDouble()), qint64(copy.value(QStringLiteral("rev")).toDouble()));

    // properties: A saves, B's stale save is a conflict carrying the current entry
    const qint64 rev0 = qint64(copy.value(QStringLiteral("rev")).toDouble());
    r = a.call(QStringLiteral("props.set"), {{"ref", "mymap/BrickWall"}, {"if_rev", double(rev0)}, {"props", QJsonObject{{"metallic", 0.4}}}});
    QVERIFY(r.value(QStringLiteral("ok")).toBool());
    QVERIFY(!b.waitEvent(QStringLiteral("texture.props")).isEmpty());
    r = b.call(QStringLiteral("props.set"), {{"ref", "mymap/BrickWall"}, {"if_rev", double(rev0)}, {"props", QJsonObject{{"metallic", 0.9}}}});
    QCOMPARE(errorCode(r), QStringLiteral("conflict"));
    QCOMPARE(r.value(QStringLiteral("error")).toObject().value(QStringLiteral("current")).toObject()
                 .value(QStringLiteral("props")).toObject().value(QStringLiteral("metallic")).toDouble(),
             0.4);
    QCOMPARE(errorCode(b.call(QStringLiteral("props.set"), {{"ref", "mymap/BrickWall"}, {"props", QJsonObject{{"shiny", 1}}}})),
             QStringLiteral("invalid"));
    // base textures may carry server overrides (sqlite, not the pack)
    QVERIFY(a.call(QStringLiteral("props.set"), {{"ref", "basepack/MetalPlate"}, {"props", QJsonObject{{"roughness", 0.3}}}})
                .value(QStringLiteral("ok")).toBool());

    // locks: A edits; B is refused, sees the badge; B's upload gets 423
    QVERIFY(a.call(QStringLiteral("textures.lock"), {{"ref", "mymap/BrickWall"}, {"purpose", "edit color"}})
                .value(QStringLiteral("ok")).toBool());
    QVERIFY(!b.waitEvent(QStringLiteral("texture.locked")).isEmpty());
    QCOMPARE(errorCode(b.call(QStringLiteral("textures.lock"), {{"ref", "mymap/BrickWall"}})), QStringLiteral("locked"));
    QNetworkReply *src = nullptr;
    const QByteArray original = httpGet(QStringLiteral("/api/2/source/mymap/BrickWall/color"), &src);
    QVERIFY(!original.isEmpty());
    const qint64 srcRev = src->rawHeader("X-Rev").toLongLong();
    src->deleteLater();
    QImage edited(64, 64, QImage::Format_RGB32);
    edited.fill(QColor(10, 200, 10));
    QByteArray png;
    QBuffer buf(&png);
    buf.open(QIODevice::WriteOnly);
    edited.save(&buf, "PNG");
    QCOMPARE(httpPut(QStringLiteral("/api/2/source/mymap/BrickWall/color"), png, bid, srcRev), 423);
    // A uploads; the color map gets a new checksum and B hears it
    QJsonObject put;
    QCOMPARE(httpPut(QStringLiteral("/api/2/source/mymap/BrickWall/color"), png, aid, srcRev, &put), 200);
    const QString oldSum = copy.value(QStringLiteral("maps")).toObject().value(QStringLiteral("color")).toObject()
                               .value(QStringLiteral("checksum")).toString();
    const QString newSum = put.value(QStringLiteral("entry")).toObject().value(QStringLiteral("maps")).toObject()
                               .value(QStringLiteral("color")).toObject().value(QStringLiteral("checksum")).toString();
    QVERIFY(!newSum.isEmpty() && newSum != oldSum);
    QVERIFY(!b.waitEvent(QStringLiteral("texture.updated")).isEmpty());
    // the old file went to .history, not away
    QVERIFY(!QDir(m_dir.filePath(QStringLiteral("data/textures/.history/mymap/BrickWall"))).entryList(QDir::Files).isEmpty());
    // a stale If-Match is a conflict
    QCOMPARE(httpPut(QStringLiteral("/api/2/source/mymap/BrickWall/color"), png, aid, srcRev), 409);

    // rename: the old ref keeps resolving
    const qint64 rev1 = qint64(put.value(QStringLiteral("entry")).toObject().value(QStringLiteral("rev")).toDouble());
    r = a.call(QStringLiteral("textures.update"), {{"ref", "mymap/BrickWall"}, {"name", "BrickWallRed"}, {"if_rev", double(rev1)}});
    QVERIFY2(r.value(QStringLiteral("ok")).toBool(), qPrintable(errorCode(r)));
    QCOMPARE(data(r).value(QStringLiteral("old_ref")).toString(), QStringLiteral("mymap/BrickWall"));
    const QJsonObject resolved = data(a.call(QStringLiteral("textures.resolve"), {{"refs", QJsonArray{"mymap/BrickWall"}}}));
    QCOMPARE(resolved.value(QStringLiteral("entries")).toObject().value(QStringLiteral("mymap/BrickWall")).toObject()
                 .value(QStringLiteral("ref")).toString(),
             QStringLiteral("mymap/BrickWallRed"));
    // the server props followed the rename (keyed by uuid)
    QCOMPARE(resolved.value(QStringLiteral("entries")).toObject().value(QStringLiteral("mymap/BrickWall")).toObject()
                 .value(QStringLiteral("props")).toObject().value(QStringLiteral("metallic")).toDouble(),
             0.4);

    // reconnect: B missed two changes and gets them replayed
    const qint64 bRev = qint64(data(b.call(QStringLiteral("status"))).value(QStringLiteral("rev")).toDouble());
    b.ws.close();
    QTest::qWaitFor([&] { return b.ws.state() == QAbstractSocket::UnconnectedState; }, 3000);
    QVERIFY(a.call(QStringLiteral("textures.update"), {{"ref", "mymap/BrickWallRed"}, {"tags", QJsonArray{"red"}}}).value(QStringLiteral("ok")).toBool());
    QVERIFY(a.call(QStringLiteral("packs.create"), {{"name", "other"}}).value(QStringLiteral("ok")).toBool());
    Client b2(QStringLiteral("bob"));
    QVERIFY(b2.open(m_port));
    const QJsonObject w = data(b2.hello(bRev));
    QStringList replayed;
    for (const auto &ev: w.value(QStringLiteral("replay")).toArray())
      replayed << ev.toObject().value(QStringLiteral("ev")).toString();
    QCOMPARE(replayed, (QStringList{QStringLiteral("texture.updated"), QStringLiteral("pack.created")}));
    QVERIFY(!w.value(QStringLiteral("resync")).toBool());
    // A's lock is still listed for newcomers
    QCOMPARE(w.value(QStringLiteral("locks")).toArray().size(), 1);

    // delete: files go to the trash
    QVERIFY(a.call(QStringLiteral("textures.delete"), {{"ref", "mymap/BrickWallRed"}}).value(QStringLiteral("ok")).toBool());
    QVERIFY(!QDir(m_dir.filePath(QStringLiteral("data/textures/.trash"))).entryList(QDir::Dirs | QDir::NoDotAndDotDot).isEmpty());
    QVERIFY(!b2.waitEvent(QStringLiteral("texture.deleted")).isEmpty());
  }

  void lutPacks() {
    Client a(QStringLiteral("alice"));
    QVERIFY(a.open(m_port));
    const int aid = data(a.hello()).value(QStringLiteral("client_id")).toInt();
    // LUTs only when asked for: texture lists stay as they were
    QCOMPARE(data(a.call(QStringLiteral("textures.query"))).value(QStringLiteral("total")).toInt(),
             data(a.call(QStringLiteral("textures.query"), {{"lut", false}})).value(QStringLiteral("total")).toInt());
    QJsonObject q = data(a.call(QStringLiteral("textures.query"), {{"lut", true}}));
    QCOMPARE(q.value(QStringLiteral("total")).toInt(), 1);  // Broken.png is no strip
    const QJsonObject neutral = q.value(QStringLiteral("items")).toArray().at(0).toObject();
    QCOMPARE(neutral.value(QStringLiteral("ref")).toString(), QStringLiteral("baseluts/Neutral"));
    QVERIFY(neutral.value(QStringLiteral("maps")).toObject().contains(QStringLiteral("lut")));
    QVERIFY(!neutral.value(QStringLiteral("thumb")).toString().isEmpty());
    // the source strip comes back as is
    QVERIFY(!QImage::fromData(httpGet(QStringLiteral("/api/2/source/baseluts/Neutral/lut"))).isNull());

    QByteArray png;
    {
      QBuffer b(&png);
      b.open(QIODevice::WriteOnly);
      QVERIFY(lutStrip(4).save(&b, "PNG"));
    }
    const QJsonObject grade{{"steps", QJsonArray{QJsonObject{{"kind", "basic"}, {"id", 1}}}}};
    const auto putLut = [&](const QString &path, const QByteArray &body, bool withGrade, QJsonObject *json) {
      QNetworkRequest req(QUrl(QStringLiteral("http://127.0.0.1:%1%2").arg(m_port).arg(path)));
      req.setRawHeader("X-Client-Id", QByteArray::number(aid));
      if (withGrade)
        req.setRawHeader("X-Lut-Grade", QJsonDocument(grade).toJson(QJsonDocument::Compact).toBase64());
      QNetworkReply *r = m_nam.put(req, body);
      QTest::qWaitFor([r] { return r->isFinished(); }, 10000);
      const int status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
      *json = QJsonDocument::fromJson(r->readAll()).object();
      r->deleteLater();
      return status;
    };
    QJsonObject json;
    QCOMPARE(putLut(QStringLiteral("/api/2/lut/baseluts/Mine"), png, true, &json), 403);  // base pack
    QCOMPARE(putLut(QStringLiteral("/api/2/lut/handmade/Mine"), png, true, &json), 404);  // a texture pack
    QVERIFY(a.call(QStringLiteral("packs.create"), {{"name", "myluts"}, {"type", "lut"}}).value(QStringLiteral("ok")).toBool());
    QByteArray notStrip;
    {
      QBuffer b(&notStrip);
      b.open(QIODevice::WriteOnly);
      QImage(16, 8, QImage::Format_RGB888).save(&b, "PNG");
    }
    QCOMPARE(putLut(QStringLiteral("/api/2/lut/myluts/Mine"), notStrip, true, &json), 400);
    QCOMPARE(putLut(QStringLiteral("/api/2/lut/myluts/Mine"), png, true, &json), 200);
    QJsonObject entry = json.value(QStringLiteral("entry")).toObject();
    QCOMPARE(entry.value(QStringLiteral("ref")).toString(), QStringLiteral("myluts/Mine"));
    QCOMPARE(entry.value(QStringLiteral("lut")).toObject().value(QStringLiteral("grade")).toObject(), grade);
    QVERIFY(!a.waitEvent(QStringLiteral("texture.created")).isEmpty());
    // replaced in place
    QCOMPARE(putLut(QStringLiteral("/api/2/lut/myluts/Mine"), png, true, &json), 200);
    QVERIFY(!a.waitEvent(QStringLiteral("texture.updated")).isEmpty());
    // a base LUT copies into a LUT pack only
    QCOMPARE(errorCode(a.call(QStringLiteral("textures.copy"),
                              {{"from", "baseluts/Neutral"}, {"pack", "mymap"}, {"type", "lut"}})),
             QStringLiteral("not_found"));
    QJsonObject r =
        a.call(QStringLiteral("textures.copy"), {{"from", "baseluts/Neutral"}, {"pack", "myluts"}, {"type", "lut"}});
    QVERIFY2(r.value(QStringLiteral("ok")).toBool(), qPrintable(errorCode(r)));
    QCOMPARE(data(r).value(QStringLiteral("entry")).toObject().value(QStringLiteral("ref")).toString(),
             QStringLiteral("myluts/Neutral"));
    // renamed with its file
    r = a.call(QStringLiteral("textures.update"), {{"ref", "myluts/Neutral"}, {"name", "Plain"}, {"type", "lut"}});
    QVERIFY2(r.value(QStringLiteral("ok")).toBool(), qPrintable(errorCode(r)));
    QVERIFY(QFileInfo::exists(m_dir.filePath(QStringLiteral("data/textures/myluts/Plain.png"))));
    QCOMPARE(data(a.call(QStringLiteral("textures.query"), {{"lut", true}})).value(QStringLiteral("total")).toInt(), 3);
  }

  int installZip(const QByteArray &zip, QJsonObject *json) {
    QNetworkRequest req(QUrl(QStringLiteral("http://127.0.0.1:%1/api/2/packs/install").arg(m_port)));
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/zip"));
    QNetworkReply *r = m_nam.post(req, zip);
    QTest::qWaitFor([r] { return r->isFinished(); }, 20000);
    const int status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    *json = QJsonDocument::fromJson(r->readAll()).object();
    r->deleteLater();
    return status;
  }

  // Packs are unique by name + type: a texture pack and a LUT pack may both be "foo", even with a texture and a LUT
  // of the same name.
  void installPacks() {
    Client a(QStringLiteral("alice"));
    QVERIFY(a.open(m_port));
    a.hello();
    QImage color(64, 64, QImage::Format_RGB32);
    color.fill(QColor(100, 110, 120));
    const QByteArray texZip = makeZip({{"foo.json", packJson("foo", "texture",
                                                             {{"Stone", QJsonObject{{"author", "test"}}}})},
                                       {"foo/Stone_1K_Color.png", pngBytes(color)},
                                       {"foo/readme.txt", "hello"},
                                       {"elsewhere/ignored.png", pngBytes(color)}});
    const QByteArray lutZip = makeZip({{"foo.json", packJson("foo", "lut")}, {"foo/Stone.png", pngBytes(lutStrip(4))}});

    QByteArray body = httpGet(QStringLiteral("/api/2/packs/install?name=foo&type=texture&size=1000"));
    QVERIFY(QJsonDocument::fromJson(body).object().value(QStringLiteral("ok")).toBool());

    QJsonObject json;
    QCOMPARE(installZip(texZip, &json), 200);
    QCOMPARE(json.value(QStringLiteral("pack")).toObject().value(QStringLiteral("type")).toString(),
             QStringLiteral("texture"));
    QVERIFY(!a.waitEvent(QStringLiteral("pack.created")).isEmpty());
    QCOMPARE(installZip(lutZip, &json), 200);
    QCOMPARE(json.value(QStringLiteral("pack")).toObject().value(QStringLiteral("type")).toString(),
             QStringLiteral("lut"));
    QVERIFY(QFileInfo::exists(m_dir.filePath(QStringLiteral("data/textures/foo.lut.json"))));
    QVERIFY(QFileInfo::exists(m_dir.filePath(QStringLiteral("data/textures/foo.lut/Stone.png"))));
    QVERIFY(QFileInfo::exists(m_dir.filePath(QStringLiteral("data/textures/foo/Stone_1K_Color.png"))));
    QVERIFY(!QFileInfo::exists(m_dir.filePath(QStringLiteral("data/textures/elsewhere"))));
    QVERIFY(!QFileInfo::exists(m_dir.filePath(QStringLiteral("data/textures/foo/elsewhere"))));

    int foos = 0;
    for (const auto &v: data(a.call(QStringLiteral("packs.list"))).value(QStringLiteral("packs")).toArray())
      foos += v.toObject().value(QStringLiteral("name")).toString() == QLatin1String("foo");
    QCOMPARE(foos, 2);
    // the same ref, told apart by type
    const QJsonObject tex = data(a.call(QStringLiteral("textures.resolve"), {{"refs", QJsonArray{"foo/Stone"}}}));
    QVERIFY(tex.value(QStringLiteral("entries")).toObject().value(QStringLiteral("foo/Stone")).toObject()
                .value(QStringLiteral("maps")).toObject().contains(QStringLiteral("color")));
    const QJsonObject lut =
        data(a.call(QStringLiteral("textures.resolve"), {{"refs", QJsonArray{"foo/Stone"}}, {"type", "lut"}}));
    QVERIFY(lut.value(QStringLiteral("entries")).toObject().value(QStringLiteral("foo/Stone")).toObject()
                .value(QStringLiteral("maps")).toObject().contains(QStringLiteral("lut")));
    QVERIFY(!QImage::fromData(httpGet(QStringLiteral("/api/2/source/foo/Stone/lut"))).isNull());
    QVERIFY(!QImage::fromData(httpGet(QStringLiteral("/api/2/source/foo/Stone/color"))).isNull());

    // never replaced
    QCOMPARE(installZip(texZip, &json), 409);
    body = httpGet(QStringLiteral("/api/2/packs/install?name=foo&type=lut&size=1000"));
    QCOMPARE(QJsonDocument::fromJson(body).object().value(QStringLiteral("error")).toObject()
                 .value(QStringLiteral("code")).toString(),
             QStringLiteral("exists"));
    // not packs
    QCOMPARE(installZip("this is no zip file at all, not even close", &json), 400);
    QCOMPARE(installZip(makeZip({{"bar/Stone_1K_Color.png", pngBytes(color)}}), &json), 400);
    QCOMPARE(installZip(makeZip({{"bar.json", packJson("bar", "texture")}, {"../bar/Stone_1K_Color.png", "x"}}), &json),
             400);
    QCOMPARE(installZip(makeZip({{"bar.json", packJson("bar", "texture")}, {"bar/notes.txt", "x"}}), &json), 400);
    // nothing left behind
    QCOMPARE(QDir(m_dir.filePath(QStringLiteral("data/textures/.incoming")))
                 .entryList(QDir::AllEntries | QDir::NoDotAndDotDot)
                 .size(),
             0);
  }

  void restartKeepsRevisionAndAsksForResync() {
    Client a(QStringLiteral("alice"));
    QVERIFY(a.open(m_port));
    const qint64 rev = qint64(data(a.hello()).value(QStringLiteral("rev")).toDouble());
    a.ws.close();
    stopServer();
    QVERIFY(startServer());
    Client c(QStringLiteral("carol"));
    QVERIFY(c.open(m_port));
    const QJsonObject w = data(c.hello(rev - 1));
    QCOMPARE(qint64(w.value(QStringLiteral("rev")).toDouble()), rev);  // persisted
    QVERIFY(w.value(QStringLiteral("resync")).toBool());               // journal is empty after a restart
    // the user packs survived the restart, the base pack is still read-only
    const QJsonArray packs = data(c.call(QStringLiteral("packs.list"))).value(QStringLiteral("packs")).toArray();
    QCOMPARE(packs.size(), 8);
  }
};

QTEST_GUILESS_MAIN(RealtimeTests)
#include "realtime_tests.moc"
