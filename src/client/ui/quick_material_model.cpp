#include "quick_material_model.h"

#include <QSet>

#include "client/net/texture_catalog.h"
#include "client/ui/texture_actions.h"
#include "client/ui/thumb_image_provider.h"
#include "client/vulkan/vk_engine.h"

namespace ui {

QuickMaterialModel::QuickMaterialModel(vk::Engine3D *engine, QObject *parent)
    : QAbstractListModel(parent), m_engine(engine) {
  connect(&ThumbCache::instance(), &ThumbCache::ready, this, [this](const QString &key) {
    for (int i = 0; i < m_rows.size(); ++i)
      if (m_rows[i].thumb.key() == key)
        touch(i);
  });
  connect(&net::TextureCatalog::instance(), &net::TextureCatalog::entryChanged, this, [this](const QString &ref) {
    for (int i = 0; i < m_rows.size(); ++i)
      if (m_rows[i].thumb.empty() && MaterialRef::parse(m_rows[i].ref).base().compare(ref, Qt::CaseInsensitive) == 0) {
        m_rows[i].thumb = thumbSourceOfRef(m_rows[i].ref);
        touch(i);
      }
  });
}

int QuickMaterialModel::rowCount(const QModelIndex &parent) const { return parent.isValid() ? 0 : int(m_rows.size()); }

QHash<int, QByteArray> QuickMaterialModel::roleNames() const {
  return {{RefRole, "ref"}, {NameRole, "name"}, {ThumbRole, "thumb"}, {RecentRole, "recent"},
          {FirstRecentRole, "firstRecent"}};
}

QVariant QuickMaterialModel::data(const QModelIndex &idx, const int role) const {
  if (!idx.isValid() || idx.row() >= m_rows.size())
    return {};
  const Row &r = m_rows[idx.row()];
  switch (role) {
    case RefRole: return r.ref;
    case NameRole: return r.name;
    case RecentRole: return r.recent;
    case FirstRecentRole: return r.recent && (idx.row() == 0 || !m_rows[idx.row() - 1].recent);
    case ThumbRole: {
      const QString id = ThumbImageProvider::thumbId(r.thumb);
      return id.isEmpty() ? QString() : QStringLiteral("image://thumb/%1?r=%2").arg(id).arg(r.rev);
    }
    default: return {};
  }
}

void QuickMaterialModel::touch(const int row) {
  ++m_rows[row].rev;
  emit dataChanged(index(row), index(row), {ThumbRole});
}

void QuickMaterialModel::refresh() {
  QVector<Row> rows;
  QSet<QString> seen;
  // Skip materials already on every selected face.
  const QStringList onSelection = m_engine ? m_engine->selectionMaterialRefs() : QStringList();
  if (onSelection.size() == 1)
    seen.insert(onSelection.front().toLower());
  QStringList missing;
  auto add = [&](const QString &ref, const bool recent) {
    if (ref.isEmpty() || seen.contains(ref.toLower()))
      return false;
    seen.insert(ref.toLower());
    Row r;
    r.ref = ref;
    const MaterialRef mr = MaterialRef::parse(ref);
    r.name = mr.isVariant() ? QStringLiteral("%1 · %2").arg(mr.name, mr.variant) : mr.name;
    r.thumb = thumbSourceOfRef(ref);
    if (r.thumb.empty())
      missing.append(mr.base());
    r.recent = recent;
    rows.append(r);
    return true;
  };
  if (m_engine)
    for (const QString &ref: m_engine->nearbyMaterialRefs(kNearby))
      add(ref, false);
  int recent = 0;
  for (const QString &ref: recentTextures())
    if (recent < kRecent && add(ref, true))
      ++recent;
  beginResetModel();
  m_rows = rows;
  endResetModel();
  if (!missing.isEmpty())
    net::TextureCatalog::instance().resolve(missing, this);
}

void QuickMaterialModel::apply(const int row) {
  if (!m_engine || row < 0 || row >= m_rows.size())
    return;
  const QString ref = m_rows[row].ref;
  m_engine->setCurrentMaterial(ref);
  m_engine->applyMaterialToSelection(ref);
}

} // namespace ui
