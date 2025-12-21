#include "texture_library.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QImageReader>
#include <QLoggingCategory>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QSet>
#include <QThreadPool>

namespace eng {

static QLoggingCategory libLog("eng.texlib", QtInfoMsg);

namespace {

enum class MapKind { Albedo, Normal, NormalDX, Roughness, Metallic, AO, Arm, Height, Emissive, Unknown };

struct SuffixRule {
  const char *suffix;
  MapKind kind;
};

// Longest suffixes first so "_normalgl" wins over "_normal".
const SuffixRule kRules[] = {
    {"_basecolor", MapKind::Albedo},    {"_base_color", MapKind::Albedo}, {"_albedo", MapKind::Albedo},
    {"_diffuse", MapKind::Albedo},      {"_color", MapKind::Albedo},      {"_col", MapKind::Albedo},
    {"_diff", MapKind::Albedo},         {"_normalgl", MapKind::Normal},   {"_normaldx", MapKind::NormalDX},
    {"_normal", MapKind::Normal},       {"_nor_gl", MapKind::Normal},     {"_nor_dx", MapKind::NormalDX},
    {"_nrm", MapKind::Normal},          {"_norm", MapKind::Normal},       {"_n", MapKind::Normal},
    {"_roughness", MapKind::Roughness}, {"_rough", MapKind::Roughness},   {"_rgh", MapKind::Roughness},
    {"_metalness", MapKind::Metallic},  {"_metallic", MapKind::Metallic}, {"_metal", MapKind::Metallic},
    {"_ambientocclusion", MapKind::AO}, {"_occlusion", MapKind::AO},      {"_ao", MapKind::AO},
    {"_arm", MapKind::Arm},             {"_orm", MapKind::Arm},           {"_displacement", MapKind::Height},
    {"_height", MapKind::Height},       {"_disp", MapKind::Height},       {"_emission", MapKind::Emissive},
    {"_emissive", MapKind::Emissive},
};

MapKind classify(const QString &stemLower, QString &base) {
  for (const auto &r: kRules) {
    const QLatin1String suf(r.suffix);
    if (stemLower.endsWith(suf)) {
      base = stemLower.left(stemLower.size() - suf.size());
      return r.kind;
    }
  }
  base = stemLower;
  return MapKind::Unknown;
}

} // namespace

TextureLibrary &TextureLibrary::instance() {
  static TextureLibrary *lib = new TextureLibrary();
  return *lib;
}

TextureLibrary::TextureLibrary() : QObject(nullptr) {}

QVector<TextureLibrary::Entry> TextureLibrary::scan(const QString &root) {
  QVector<Entry> out;
  if (root.isEmpty() || !QFileInfo(root).isDir())
    return out;

  static const QStringList kExt = {QStringLiteral("*.png"), QStringLiteral("*.jpg"), QStringLiteral("*.jpeg"),
                                   QStringLiteral("*.tga"), QStringLiteral("*.bmp"), QStringLiteral("*.webp"),
                                   QStringLiteral("*.tif"), QStringLiteral("*.tiff")};
  const QDir rootDir(root);
  QHash<QString, Entry> byName;
  QDirIterator it(root, kExt, QDir::Files, QDirIterator::Subdirectories | QDirIterator::FollowSymlinks);
  int files = 0;
  while (it.hasNext()) {
    const QString path = it.next();
    ++files;
    const QFileInfo fi(path);
    QString rel = rootDir.relativeFilePath(fi.absolutePath());
    if (rel == QLatin1String("."))
      rel.clear();
    QString base;
    const MapKind kind = classify(fi.completeBaseName().toLower(), base);
    const QString name = (rel.isEmpty() ? base : rel.toLower() + QLatin1Char('/') + base);
    Entry &e = byName[name];
    if (e.name.isEmpty()) {
      e.name = name;
      e.category = rel.section(QLatin1Char('/'), 0, 0);
    }
    switch (kind) {
      case MapKind::Albedo:
      case MapKind::Unknown:
        // An unsuffixed image is the albedo unless a real one turns up.
        if (e.albedo.isEmpty() || kind == MapKind::Albedo) {
          e.albedo = path;
          e.display = fi.completeBaseName();
        }
        break;
      case MapKind::Normal: e.normal = path; e.normalIsDirectX = false; break;
      case MapKind::NormalDX:
        if (e.normal.isEmpty()) {
          e.normal = path;
          e.normalIsDirectX = true;
        }
        break;
      case MapKind::Roughness: e.roughness = path; break;
      case MapKind::Metallic: e.metallic = path; break;
      case MapKind::AO: e.ao = path; break;
      case MapKind::Arm: e.arm = path; break;
      case MapKind::Height: e.height = path; break;
      case MapKind::Emissive: e.emissive = path; break;
    }
  }

  out.reserve(byName.size());
  for (auto &e: byName) {
    if (e.albedo.isEmpty())
      continue; // maps without a colour image are not a material
    QImageReader reader(e.albedo);
    e.size = reader.size();
    if (e.display.isEmpty())
      e.display = QFileInfo(e.albedo).completeBaseName();
    out.push_back(e);
  }
  std::sort(out.begin(), out.end(), [](const Entry &a, const Entry &b) { return a.name < b.name; });
  qCInfo(libLog).noquote() << QStringLiteral("[texlib] %1: %2 files -> %3 materials").arg(root).arg(files).arg(out.size());
  return out;
}

void TextureLibrary::setRoot(const QString &dir) {
  m_root = dir;
  m_scanning = true;
  const quint64 gen = ++m_generation;
  QThreadPool::globalInstance()->start([this, dir, gen] {
    QVector<Entry> entries = scan(dir);
    QMetaObject::invokeMethod(this, [this, entries = std::move(entries), gen]() mutable {
      if (gen != m_generation)
        return;
      {
        QMutexLocker lock(&m_mutex);
        m_entries = std::move(entries);
        m_index.clear();
        for (int i = 0; i < m_entries.size(); ++i)
          m_index.insert(m_entries[i].name, i);
      }
      m_scanning = false;
      emit changed();
    }, Qt::QueuedConnection);
  });
}

bool TextureLibrary::find(const QString &name, Entry &out) const {
  QMutexLocker lock(&m_mutex);
  auto it = m_index.constFind(name.toLower());
  if (it == m_index.constEnd()) {
    // Maps store "dev/foo" while a library may hold "foo" at its root (or vice versa): try the last component.
    const QString leaf = name.section(QLatin1Char('/'), -1).toLower();
    it = m_index.constFind(leaf);
    if (it == m_index.constEnd())
      return false;
  }
  out = m_entries[*it];
  return true;
}

QVector<TextureLibrary::Entry> TextureLibrary::entries() const {
  QMutexLocker lock(&m_mutex);
  return m_entries;
}

QStringList TextureLibrary::categories() const {
  QMutexLocker lock(&m_mutex);
  QSet<QString> cats;
  for (const auto &e: m_entries)
    cats.insert(e.category);
  QStringList out(cats.begin(), cats.end());
  out.sort();
  return out;
}

} // namespace eng
