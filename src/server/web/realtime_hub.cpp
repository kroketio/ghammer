#include "realtime_hub.h"

#include <QJsonDocument>
#include <QPointer>
#include <QtConcurrent>
#include <QtDebug>

#include "server/catalog/catalog.h"
#include "server/catalog/derivatives.h"
#include "server/catalog/mutations.h"

namespace {

RealtimeHub *s_hub = nullptr;

QJsonArray pairsJson(const QVector<QPair<QString, int>> &v) {
  QJsonArray out;
  for (const auto &[name, count]: v)
    out.append(QJsonArray{name, count});
  return out;
}

QJsonObject queryResultJson(const CatalogQueryResult &r, const CatalogQuery &q) {
  QJsonObject o;
  o.insert(QStringLiteral("total"), r.total);
  o.insert(QStringLiteral("offset"), q.offset);
  o.insert(QStringLiteral("limit"), q.limit);
  QJsonArray items;
  for (const auto &e: r.items)
    items.append(e.toJson());
  o.insert(QStringLiteral("items"), items);
  if (q.facets) {
    o.insert(QStringLiteral("facets"), pairsJson(r.facets));
    o.insert(QStringLiteral("distinct_tags"), r.distinctTags);
    o.insert(QStringLiteral("license_facets"), pairsJson(r.licenseFacets));
  }
  o.insert(QStringLiteral("rev"), double(Catalog::instance().revision()));
  return o;
}

} // namespace

RealtimeHub::RealtimeHub(QObject *parent) : QObject(parent) {
  s_hub = this;
  Mutations::instance().broadcast = [](const QJsonObject &event) {
    if (s_hub)
      s_hub->broadcast(event);
  };
}

RealtimeHub::~RealtimeHub() {
  Mutations::instance().broadcast = nullptr;
  s_hub = nullptr;
}

RealtimeHub *RealtimeHub::instance() { return s_hub; }

void RealtimeHub::addClient(std::unique_ptr<QWebSocket> ws) {
  const int id = m_nextId++;
  QWebSocket *raw = ws.get();
  raw->setParent(nullptr);
  Client c;
  c.id = id;
  c.peer = raw->peerAddress().toString();
  c.ws = std::move(ws);
  m_clients.emplace(id, std::move(c));
  connect(raw, &QWebSocket::textMessageReceived, this, [this, id](const QString &t) { onText(id, t); });
  connect(raw, &QWebSocket::binaryMessageReceived, this,
          [this, id](const QByteArray &b) { onText(id, QString::fromUtf8(b)); });
  connect(raw, &QWebSocket::disconnected, this, [this, id] { removeClient(id); });
  qInfo().noquote() << QStringLiteral("ws: client %1 connected from %2").arg(id).arg(m_clients.at(id).peer);
}

void RealtimeHub::removeClient(int clientId) {
  auto it = m_clients.find(clientId);
  if (it == m_clients.end())
    return;
  const bool greeted = it->second.greeted;
  const QString name = it->second.name;
  it->second.ws->deleteLater();
  (void)it->second.ws.release();
  m_clients.erase(it);
  Mutations::instance().clientDisconnected(clientId);
  qInfo().noquote() << QStringLiteral("ws: client %1 (%2) disconnected").arg(clientId).arg(name);
  if (greeted) {
    QJsonObject ev;
    ev.insert(QStringLiteral("ev"), QStringLiteral("client.left"));
    ev.insert(QStringLiteral("rev"), double(Catalog::instance().revision()));
    ev.insert(QStringLiteral("by"), ClientRef{clientId, name}.toJson());
    QJsonObject d;
    d.insert(QStringLiteral("clients"), clientsJson());
    ev.insert(QStringLiteral("data"), d);
    broadcast(ev);
  }
}

void RealtimeHub::broadcast(const QJsonObject &event) {
  QMetaObject::invokeMethod(this, [this, event] {
    for (auto &[id, c]: m_clients)
      if (c.greeted)
        sendRaw(c, event);
  }, Qt::QueuedConnection);
}

void RealtimeHub::sendTo(int clientId, const QJsonObject &msg) {
  QMetaObject::invokeMethod(this, [this, clientId, msg] {
    auto it = m_clients.find(clientId);
    if (it != m_clients.end())
      sendRaw(it->second, msg);
  }, Qt::QueuedConnection);
}

void RealtimeHub::sendRaw(Client &c, const QJsonObject &msg) {
  if (c.ws && c.ws->state() == QAbstractSocket::ConnectedState)
    c.ws->sendTextMessage(QString::fromUtf8(QJsonDocument(msg).toJson(QJsonDocument::Compact)));
}

void RealtimeHub::reply(int clientId, const QJsonValue &reqId, const QJsonObject &data) {
  QJsonObject m;
  m.insert(QStringLiteral("re"), reqId);
  m.insert(QStringLiteral("ok"), true);
  m.insert(QStringLiteral("data"), data);
  sendTo(clientId, m);
}

void RealtimeHub::replyError(int clientId, const QJsonValue &reqId, const QString &code, const QString &message,
                             const QJsonObject &current) {
  QJsonObject e;
  e.insert(QStringLiteral("code"), code);
  e.insert(QStringLiteral("message"), message);
  if (!current.isEmpty())
    e.insert(QStringLiteral("current"), current);
  QJsonObject m;
  m.insert(QStringLiteral("re"), reqId);
  m.insert(QStringLiteral("ok"), false);
  m.insert(QStringLiteral("error"), e);
  sendTo(clientId, m);
}

QJsonArray RealtimeHub::clientsJson() const {
  QJsonArray out;
  for (const auto &[id, c]: m_clients) {
    if (!c.greeted)
      continue;
    QJsonObject o;
    o.insert(QStringLiteral("client"), id);
    o.insert(QStringLiteral("name"), c.name);
    o.insert(QStringLiteral("app"), c.app);
    out.append(o);
  }
  return out;
}

QJsonObject RealtimeHub::statusJson() const {
  const Catalog &cat = Catalog::instance();
  const auto p = DerivativeBuilder::instance().progress();
  QJsonObject d;
  d.insert(QStringLiteral("queued"), double(p.queued));
  d.insert(QStringLiteral("done"), double(p.done));
  d.insert(QStringLiteral("failed"), double(p.failed));
  QJsonObject o;
  o.insert(QStringLiteral("rev"), double(cat.revision()));
  o.insert(QStringLiteral("loading"), !cat.isLoaded());
  o.insert(QStringLiteral("textures"), cat.textureCount());
  o.insert(QStringLiteral("packs"), int(cat.packs().size()));
  o.insert(QStringLiteral("derivatives"), d);
  o.insert(QStringLiteral("clients"), clientsJson());
  o.insert(QStringLiteral("protocol"), kProtocolVersion);
  return o;
}

void RealtimeHub::onText(int clientId, const QString &text) {
  auto it = m_clients.find(clientId);
  if (it == m_clients.end())
    return;
  Client &c = it->second;
  QJsonParseError pe{};
  const QJsonObject msg = QJsonDocument::fromJson(text.toUtf8(), &pe).object();
  if (pe.error != QJsonParseError::NoError) {
    replyError(clientId, QJsonValue(), QStringLiteral("invalid"), QStringLiteral("malformed JSON"));
    return;
  }
  const QJsonValue reqId = msg.value(QStringLiteral("id"));
  const QString op = msg.value(QStringLiteral("op")).toString();
  const QJsonObject args = msg.value(QStringLiteral("args")).toObject();
  if (op == QLatin1String("hello")) {
    handleHello(c, reqId, args);
    return;
  }
  if (!c.greeted) {
    replyError(clientId, reqId, QStringLiteral("invalid"), QStringLiteral("send hello first"));
    return;
  }
  dispatch(c, reqId, op, args);
}

void RealtimeHub::handleHello(Client &c, const QJsonValue &reqId, const QJsonObject &args) {
  c.name = args.value(QStringLiteral("name")).toString(QStringLiteral("client %1").arg(c.id)).left(80);
  c.app = args.value(QStringLiteral("client")).toString().left(40);
  const bool firstHello = !c.greeted;
  c.greeted = true;
  Mutations::instance().clientConnected(ClientRef{c.id, c.name});

  const qint64 lastRev = qint64(args.value(QStringLiteral("last_rev")).toDouble(0));
  QJsonObject w = statusJson();
  w.insert(QStringLiteral("client_id"), c.id);
  w.insert(QStringLiteral("server_version"), QStringLiteral("asset_server 2"));
  w.insert(QStringLiteral("locks"), Mutations::instance().locksJson());
  if (lastRev > 0) {
    bool covered = false;
    const QJsonArray replay = Mutations::instance().eventsSince(lastRev, &covered);
    if (covered)
      w.insert(QStringLiteral("replay"), replay);
    else
      w.insert(QStringLiteral("resync"), true);
  }
  reply(c.id, reqId, w);
  qInfo().noquote() << QStringLiteral("ws: client %1 is '%2' (%3), last_rev %4").arg(c.id).arg(c.name, c.app).arg(lastRev);

  if (firstHello) {
    QJsonObject ev;
    ev.insert(QStringLiteral("ev"), QStringLiteral("client.joined"));
    ev.insert(QStringLiteral("rev"), double(Catalog::instance().revision()));
    ev.insert(QStringLiteral("by"), ClientRef{c.id, c.name}.toJson());
    QJsonObject d;
    d.insert(QStringLiteral("clients"), clientsJson());
    ev.insert(QStringLiteral("data"), d);
    broadcast(ev);
  }
}

void RealtimeHub::dispatch(Client &c, const QJsonValue &reqId, const QString &op, const QJsonObject &args) {
  const int clientId = c.id;
  const ClientRef by{c.id, c.name};
  QPointer<RealtimeHub> self(this);

  if (op == QLatin1String("status")) {
    reply(clientId, reqId, statusJson());
    return;
  }

  // reads: off the hub thread, reply queued back
  auto read = [this, clientId, reqId](std::function<QJsonObject()> fn) {
    (void)QtConcurrent::run([this, clientId, reqId, fn = std::move(fn)] {
      if (!Catalog::instance().isLoaded()) {
        replyError(clientId, reqId, QStringLiteral("loading"), QStringLiteral("the catalogue is still loading"));
        return;
      }
      reply(clientId, reqId, fn());
    });
  };

  if (op == QLatin1String("packs.list")) {
    read([] {
      QJsonArray packs;
      for (const auto &p: Catalog::instance().packs())
        packs.append(p.toJson());
      QJsonObject o;
      o.insert(QStringLiteral("packs"), packs);
      return o;
    });
    return;
  }
  if (op == QLatin1String("textures.query")) {
    read([args] {
      const CatalogQuery q = CatalogQuery::fromJson(args);
      return queryResultJson(Catalog::instance().query(q), q);
    });
    return;
  }
  if (op == QLatin1String("tags.query")) {
    read([args] {
      const CatalogQuery q = CatalogQuery::fromJson(args.value(QStringLiteral("filter")).toObject());
      QJsonObject o;
      o.insert(QStringLiteral("tags"),
               pairsJson(Catalog::instance().tagSearch(q, args.value(QStringLiteral("q")).toString(),
                                                       std::clamp(args.value(QStringLiteral("limit")).toInt(50), 1, 500))));
      return o;
    });
    return;
  }
  if (op == QLatin1String("tags.popular")) {
    read([args] {
      const CatalogQuery q = CatalogQuery::fromJson(args.value(QStringLiteral("filter")).toObject());
      QJsonObject o;
      o.insert(QStringLiteral("tags"), pairsJson(Catalog::instance().popular(q)));
      return o;
    });
    return;
  }
  if (op == QLatin1String("textures.resolve")) {
    read([args] {
      const Catalog &cat = Catalog::instance();
      const bool lut = args.value(QStringLiteral("type")).toString() == QLatin1String("lut");
      QJsonObject found;
      QJsonArray missing;
      for (const auto &v: args.value(QStringLiteral("refs")).toArray()) {
        const QString ref = v.toString();
        const TextureEntry e = cat.entry(ref, lut);
        if (e.isValid())
          found.insert(ref, e.toJson());
        else
          missing.append(ref);
      }
      QJsonObject byId;
      for (const auto &v: args.value(QStringLiteral("ids")).toArray()) {
        const TextureEntry e = cat.entryById(v.toString());
        if (e.isValid())
          byId.insert(v.toString(), e.toJson());
      }
      QJsonObject o;
      o.insert(QStringLiteral("entries"), found);
      o.insert(QStringLiteral("missing"), missing);
      o.insert(QStringLiteral("by_id"), byId);
      o.insert(QStringLiteral("rev"), double(cat.revision()));
      return o;
    });
    return;
  }

  // writes: the mutation thread
  using Op = QFuture<MutationResult> (Mutations::*)(const QJsonObject &, const ClientRef &);
  static const QHash<QString, Op> writes = {
      {QStringLiteral("packs.create"), &Mutations::createPack},
      {QStringLiteral("packs.update"), &Mutations::updatePack},
      {QStringLiteral("packs.delete"), &Mutations::deletePack},
      {QStringLiteral("textures.copy"), &Mutations::copyTexture},
      {QStringLiteral("textures.update"), &Mutations::updateTexture},
      {QStringLiteral("textures.delete"), &Mutations::deleteTexture},
      {QStringLiteral("props.set"), &Mutations::setProps},
      {QStringLiteral("props.clear"), &Mutations::clearProps},
      {QStringLiteral("textures.lock"), &Mutations::lockTexture},
      {QStringLiteral("textures.unlock"), &Mutations::unlockTexture},
  };
  auto w = writes.constFind(op);
  if (w == writes.constEnd()) {
    replyError(clientId, reqId, QStringLiteral("invalid"), QStringLiteral("unknown op '%1'").arg(op));
    return;
  }
  if (!Catalog::instance().isLoaded()) {
    replyError(clientId, reqId, QStringLiteral("loading"), QStringLiteral("the catalogue is still loading"));
    return;
  }
  (Mutations::instance().*(w.value()))(args, by).then(this, [self, clientId, reqId](const MutationResult &r) {
    if (!self)
      return;
    if (r.ok)
      self->reply(clientId, reqId, r.data);
    else
      self->replyError(clientId, reqId, r.code, r.message, r.current);
  });
}
