#include "document_manager.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QImageReader>
#include <QSet>

#include "client/engine/map_loader.h"
#include "client/engine/scene.h"
#include "client/prefs/preferences.h"

namespace proj {

QString Document::fileName() const {
  if (!path.isEmpty())
    return QFileInfo(path).fileName();
  return untitled > 1 ? QStringLiteral("Untitled %1").arg(untitled) : QStringLiteral("Untitled");
}

DocumentManager::DocumentManager(vk::Engine3D *engine, QObject *parent) : QObject(parent), m_engine(engine) {
  connect(m_engine, &vk::Engine3D::dirtyChanged, this, &DocumentManager::dirtyChanged);
  installLevelStore();
}

DocumentManager::~DocumentManager() = default;

bool DocumentManager::isLevelFile(const QString &path) {
  const QString suffix = QFileInfo(path).suffix().toLower();
  return suffix == QLatin1String("ghmap") || suffix == QLatin1String("vmf");
}

bool DocumentManager::isImageFile(const QString &path) {
  const QByteArray suffix = QFileInfo(path).suffix().toLower().toLatin1();
  if (suffix.isEmpty())
    return false;
  if (suffix == "exr")
    return true;
  static const QList<QByteArray> formats = QImageReader::supportedImageFormats();
  return formats.contains(suffix);
}

Document *DocumentManager::at(const int i) const {
  return i >= 0 && i < count() ? m_docs[size_t(i)].get() : nullptr;
}

int DocumentManager::indexOf(const Document *d) const {
  for (int i = 0; i < count(); ++i)
    if (m_docs[size_t(i)].get() == d)
      return i;
  return -1;
}

Document *DocumentManager::find(const QString &path) const {
  if (path.isEmpty())
    return nullptr;
  const QString clean = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
  for (const auto &d: m_docs)
    if (!d->path.isEmpty() && QDir::cleanPath(d->path) == clean)
      return d.get();
  return nullptr;
}

Document *DocumentManager::byId(const quint64 id) const {
  for (const auto &d: m_docs)
    if (d->id == id)
      return d.get();
  return nullptr;
}

Document *DocumentManager::bySerial(const quint64 serial) const {
  for (const auto &d: m_docs)
    if (d->isLevel() && d->loaded && d->serial == serial)
      return d.get();
  return nullptr;
}

QList<Document *> DocumentManager::levels() const {
  QList<Document *> out;
  for (const auto &d: m_docs)
    if (d->isLevel())
      out << d.get();
  return out;
}

Document *DocumentManager::open(const QString &path, const bool activate, QString *error) {
  if (Document *d = find(path)) {
    if (activate && !this->activate(indexOf(d), error))
      return nullptr;
    return d;
  }
  const bool level = isLevelFile(path);
  if (!level && !isImageFile(path)) {
    if (error)
      *error = tr("%1 is not a level or an image").arg(QFileInfo(path).fileName());
    return nullptr;
  }
  auto doc = std::make_unique<Document>();
  doc->kind = level ? Document::Kind::Level : Document::Kind::Image;
  doc->path = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
  doc->id = m_nextId++;
  Document *d = doc.get();
  m_docs.push_back(std::move(doc));
  emit documentsChanged();
  if (activate && !this->activate(count() - 1, error)) {
    close(indexOf(d));
    return nullptr;
  }
  return d;
}

Document *DocumentManager::newLevel(const QString &path) {
  if (!m_engine->canSwitchDocument())
    return nullptr;
  auto doc = std::make_unique<Document>();
  doc->kind = Document::Kind::Level;
  doc->path = path.isEmpty() ? QString() : QDir::cleanPath(QFileInfo(path).absoluteFilePath());
  doc->id = m_nextId++;
  if (doc->path.isEmpty()) {
    // The lowest free number.
    QSet<int> used;
    for (const auto &o: m_docs)
      if (o->path.isEmpty())
        used.insert(o->untitled);
    doc->untitled = 1;
    while (used.contains(doc->untitled))
      ++doc->untitled;
  }
  doc->loaded = true;
  doc->parked = m_engine->newDocumentState();
  doc->serial = doc->parked->serial();
  Document *d = doc.get();
  m_docs.push_back(std::move(doc));
  bringIntoEngine(d);
  // Start like a loaded map: world_env, default grid, clean history.
  m_engine->setScene(QSharedPointer<eng::Scene>(new eng::Scene()));
  m_engine->setGridLevelOffset(vk::Engine3D::kGridLevelDefaultOffset);
  m_engine->markClean();
  m_active = count() - 1;
  installLevelStore();
  emit documentsChanged();
  emit activeChanged();
  return d;
}

bool DocumentManager::loadInto(Document *d, QString *error) {
  QElapsedTimer t;
  t.start();
  const auto loader = eng::MapLoader::forPath(d->path);
  if (!loader) {
    if (error)
      *error = tr("%1: unknown map format").arg(d->fileName());
    return false;
  }
  eng::MapLoadResult result;
  QString why;
  if (!loader->load(d->path, result, &why)) {
    qWarning().noquote() << QStringLiteral("[map] failed to load '%1': %2").arg(d->path, why);
    if (error)
      *error = tr("Could not open %1: %2").arg(d->fileName(), why);
    return false;
  }
  d->parked = m_engine->newDocumentState();
  d->serial = d->parked->serial();
  d->loaded = true;
  bringIntoEngine(d);
  m_engine->setScene(result.scene);
  if (result.hasDocument)
    m_engine->applyMapDocument(result.doc);
  m_engine->markClean();
  qInfo().noquote() << QStringLiteral("[map] opened '%1' as %2 in %3 ms")
                           .arg(d->path, result.formatName)
                           .arg(t.elapsed());
  emit levelLoaded(d);
  return true;
}

void DocumentManager::bringIntoEngine(Document *d) {
  if (d == m_engineDoc)
    return;
  std::unique_ptr<vk::Engine3D::DocumentState> st = std::move(d->parked);
  if (!st)
    st = m_engine->newDocumentState();
  m_engine->switchDocument(*st);
  // `st` now holds the previous level: parked with its tab, or dropped when it has none.
  if (m_engineDoc)
    m_engineDoc->parked = std::move(st);
  m_engineDoc = d;
  d->serial = m_engine->levelSerial();
}

bool DocumentManager::activate(const int index, QString *error) {
  Document *d = at(index);
  if (!d)
    return false;
  if (d->isLevel() && d != m_engineDoc) {
    if (!m_engine->canSwitchDocument()) {
      if (error)
        *error = tr("Finish the current edit first");
      return false;
    }
    if (!d->loaded) {
      if (!loadInto(d, error))
        return false;
    } else {
      bringIntoEngine(d);
    }
  }
  const bool changed = m_active != index;
  m_active = index;
  installLevelStore();
  if (changed)
    emit activeChanged();
  return true;
}

void DocumentManager::close(const int index) {
  if (index < 0 || index >= count())
    return;
  std::unique_ptr<Document> gone = std::move(m_docs[size_t(index)]);
  m_docs.erase(m_docs.begin() + index);
  const bool wasActive = index == m_active;
  if (index < m_active)
    --m_active;
  else if (wasActive)
    m_active = std::min(index, count() - 1);

  if (gone.get() == m_engineDoc) {
    // The engine takes the next tab if a level, else the nearest level, else an empty level nobody sees.
    m_engineDoc = nullptr;
    Document *next = at(m_active);
    if (!next || !next->isLevel()) {
      next = nullptr;
      for (int dist = 0; dist < count() && !next; ++dist)
        for (const int i: {index - 1 - dist, index + dist})
          if (Document *c = at(i); c && c->isLevel() && c->loaded && !next)
            next = c;
    }
    bool brought = false;
    if (next && next->loaded) {
      bringIntoEngine(next);
      brought = true;
    } else if (next && loadInto(next, nullptr)) {
      brought = true;
    }
    if (!brought) {
      auto empty = m_engine->newDocumentState();
      m_engine->switchDocument(*empty); // `empty` takes the closed level away
    }
  }
  gone.reset();
  if (Document *a = active(); a && a->isLevel() && a != m_engineDoc) {
    if (!a->loaded)
      loadInto(a, nullptr);
    else
      bringIntoEngine(a);
  }
  installLevelStore();
  emit documentsChanged();
  emit activeChanged();
  emit dirtyChanged();
}

void DocumentManager::closeAll(Document *keep) {
  if (keep && indexOf(keep) < 0)
    keep = nullptr;
  if (keep) {
    activate(indexOf(keep));
  } else if (m_engineDoc) {
    // An empty level nobody sees.
    m_engineDoc = nullptr;
    auto empty = m_engine->newDocumentState();
    m_engine->switchDocument(*empty);
  }
  for (int i = count() - 1; i >= 0; --i)
    if (m_docs[size_t(i)].get() != keep)
      m_docs.erase(m_docs.begin() + i);
  m_active = keep ? 0 : -1;
  installLevelStore();
  emit documentsChanged();
  emit activeChanged();
  emit dirtyChanged();
}

void DocumentManager::move(const int from, const int to) {
  if (from == to || from < 0 || to < 0 || from >= count() || to >= count())
    return;
  Document *activeDoc = active();
  auto d = std::move(m_docs[size_t(from)]);
  m_docs.erase(m_docs.begin() + from);
  m_docs.insert(m_docs.begin() + to, std::move(d));
  m_active = indexOf(activeDoc);
  emit documentsChanged();
}

void DocumentManager::renamed(const QString &from, const QString &to) {
  const QString f = QDir::cleanPath(from), t = QDir::cleanPath(to);
  bool any = false;
  for (const auto &d: m_docs) {
    if (d->path == f) {
      d->path = t;
      any = true;
    } else if (d->path.startsWith(f + QLatin1Char('/'))) {
      d->path = t + d->path.mid(f.size());
      any = true;
    }
  }
  if (any) {
    installLevelStore();
    emit documentsChanged();
  }
}

bool DocumentManager::dirty(const Document *d) const {
  if (!d || !d->isLevel())
    return false;
  if (d == m_engineDoc)
    return m_engine->dirty();
  return d->parked && d->parked->dirty();
}

QList<Document *> DocumentManager::dirtyLevels() const {
  QList<Document *> out;
  for (const auto &d: m_docs)
    if (dirty(d.get()))
      out << d.get();
  return out;
}

bool DocumentManager::withLevel(const Document *d, const std::function<void()> &fn) {
  if (!d || !d->isLevel())
    return false;
  if (d == m_engineDoc) {
    fn();
    return true;
  }
  if (!d->parked)
    return false;
  m_engine->withDocument(*d->parked, fn);
  return true;
}

bool DocumentManager::withLevel(const quint64 serial, const std::function<void()> &fn) {
  return withLevel(bySerial(serial), fn);
}

void DocumentManager::markClean(Document *d) {
  if (!d || !d->isLevel())
    return;
  if (d == m_engineDoc)
    m_engine->markClean();
  else if (d->parked && d->parked->m_undoStack)
    d->parked->m_undoStack->markClean();
  emit dirtyChanged();
}

void DocumentManager::setPath(Document *d, const QString &path) {
  if (!d)
    return;
  d->path = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
  installLevelStore();
  emit documentsChanged();
}

void DocumentManager::installLevelStore() {
  prefs::get().setStore(prefs::Scope::Level, m_engineDoc ? m_engine->levelStore() : nullptr,
                        m_engineDoc ? m_engineDoc->fileName() : QString());
}

} // namespace proj
