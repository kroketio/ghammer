#include "outliner.h"
#include "client/ui/scale.h"

#include <algorithm>
#include <set>

#include <QApplication>
#include <QColorDialog>
#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPixmap>
#include <QSettings>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>
#include <QVBoxLayout>

#include "client/engine/entity.h"
#include "client/engine/scene.h"
#include "client/ui/action_manager.h"
#include "client/vulkan/vk_engine.h"

namespace ui {

namespace {

QIcon swatch(const QColor &c, bool active) {
  QPixmap pm(32, 32);
  pm.fill(Qt::transparent);
  QPainter p(&pm);
  p.setRenderHint(QPainter::Antialiasing);
  p.setPen(QPen(c.darker(160), 2));
  p.setBrush(c);
  p.drawRoundedRect(QRectF(4, 4, 24, 24), 5, 5);
  if (active) {
    p.setPen(Qt::NoPen);
    p.setBrush(Qt::white);
    p.drawEllipse(QPointF(16, 16), 5.5, 5.5);
  }
  return QIcon(pm);
}

} // namespace

SceneTreeModel::SceneTreeModel(vk::Engine3D *engine, QObject *parent)
    : QAbstractItemModel(parent), m_engine(engine) {}

void SceneTreeModel::recount() {
  m_counts.clear();
  m_selCounts.clear();
  m_mirrorMembers.clear();
  m_hostMirrors.clear();
  m_mirrorHosts.clear();
  m_totalBrushes = 0;
  if (!m_engine)
    return;
  m_counts = m_engine->meshGroupCounts();
  m_selCounts = m_engine->meshGroupSelectedCounts();
  for (const int n: std::as_const(m_counts))
    m_totalBrushes += n;
  if (const auto &scene = m_engine->scene()) {
    for (const auto &st: scene->mirrorSets)
      m_mirrorMembers.insert(st.id, m_engine->mirrorSetMembers(st.id));
    // Modified object <-> mirror collection, through the sources.
    std::set<std::pair<quint32, quint32>> pairs; // host, set
    for (const auto &o: scene->gameObjects)
      if (o && !o->isDerived() && o->modHost() && o->mirrorLink())
        if (const auto *l = scene->findMirrorLink(o->mirrorLink()))
          pairs.insert({o->modHost(), l->set});
    for (const auto &[host, set]: pairs) {
      const eng::mods::Host *h = scene->findModHost(host);
      const eng::Scene::MirrorSet *st = scene->findMirrorSet(set);
      if (!h || !st)
        continue;
      m_hostMirrors[host].append(st->name);
      m_mirrorHosts[set].append(h->name);
    }
  }
}

const std::vector<quint32> &SceneTreeModel::mirrorMembers(const quint32 set) const {
  static const std::vector<quint32> kNone;
  const auto it = m_mirrorMembers.constFind(set);
  return it == m_mirrorMembers.constEnd() ? kNone : *it;
}

void SceneTreeModel::rebuild() {
  beginResetModel();
  m_sections.clear();
  m_groups.clear();
  recount();
  if (m_engine && m_engine->scene()) {
    SectionRows groups {SectionGroups, {}};
    for (const auto &g: m_engine->meshGroups()) {
      groups.rows.push_back({KindGroup, g.id});
      m_groups.push_back(g.id);
    }
    if (!groups.rows.empty())
      m_sections.push_back(std::move(groups));

    SectionRows procs {SectionGenerated, {}};
    for (const auto &p: m_engine->scene()->procObjects)
      if (!m_engine->procMembers(p.id).empty())
        procs.rows.push_back({KindProc, p.id});
    if (!procs.rows.empty())
      m_sections.push_back(std::move(procs));

    SectionRows modified {SectionModified, {}};
    for (const auto &h: m_engine->scene()->modHosts)
      if (!m_engine->modSources(h.id).empty())
        modified.rows.push_back({KindHost, h.id});
    if (!modified.rows.empty())
      m_sections.push_back(std::move(modified));

    SectionRows paths {SectionPaths, {}};
    for (const auto &p: m_engine->scene()->paths)
      paths.rows.push_back({KindPath, p.id});
    if (!paths.rows.empty())
      m_sections.push_back(std::move(paths));

    SectionRows mirrors {SectionMirrors, {}};
    for (const auto &st: m_engine->scene()->mirrorSets)
      if (!mirrorMembers(st.id).empty())
        mirrors.rows.push_back({KindMirror, st.id});
    if (!mirrors.rows.empty())
      m_sections.push_back(std::move(mirrors));

    // Point and brush entities.
    SectionRows ents {SectionEntities, {}};
    for (const auto &marker: m_engine->entityMarkers())
      ents.rows.push_back({KindEntity, marker.id});
    for (const auto &e: m_engine->scene()->entities)
      if (const eng::EntityClass *c = e.cls(); c && c->isBrushEntity() && !e.hidden)
        ents.rows.push_back({KindEntity, e.id});
    if (!ents.rows.empty())
      m_sections.push_back(std::move(ents));
  }
  endResetModel();
}

void SceneTreeModel::refreshRows() {
  recount();
  if (m_sections.empty())
    return;
  emit dataChanged(index(0, 0), index(int(m_sections.size()) - 1, ColumnCount - 1));
  for (int i = 0; i < int(m_sections.size()); ++i) {
    const QModelIndex parent = index(i, 0);
    const int n = int(m_sections[size_t(i)].rows.size());
    if (n)
      emit dataChanged(index(0, 0, parent), index(n - 1, ColumnCount - 1, parent));
  }
}

QModelIndex SceneTreeModel::indexForEntity(quint32 entityId) const {
  for (size_t si = 0; si < m_sections.size(); ++si) {
    const auto &sec = m_sections[si];
    if (sec.section != SectionEntities)
      continue;
    for (size_t i = 0; i < sec.rows.size(); ++i)
      if (sec.rows[i].id == entityId)
        return createIndex(int(i), 0, quintptr(si + 1));
  }
  return {};
}

QModelIndex SceneTreeModel::index(int row, int column, const QModelIndex &parent) const {
  if (row < 0 || column < 0 || column >= ColumnCount)
    return {};
  if (!parent.isValid())
    return row < int(m_sections.size()) ? createIndex(row, column, quintptr(0)) : QModelIndex();
  if (parent.internalId() != 0 || parent.row() >= int(m_sections.size()))
    return {};
  if (row >= int(m_sections[size_t(parent.row())].rows.size()))
    return {};
  return createIndex(row, column, quintptr(parent.row() + 1));
}

QModelIndex SceneTreeModel::parent(const QModelIndex &child) const {
  if (!child.isValid() || child.internalId() == 0)
    return {};
  return createIndex(int(child.internalId() - 1), 0, quintptr(0));
}

int SceneTreeModel::rowCount(const QModelIndex &parent) const {
  if (!parent.isValid())
    return int(m_sections.size());
  if (parent.internalId() != 0 || parent.column() != 0 || parent.row() >= int(m_sections.size()))
    return 0;
  return int(m_sections[size_t(parent.row())].rows.size());
}

SceneTreeModel::Row SceneTreeModel::rowAt(const QModelIndex &idx) const {
  if (!idx.isValid())
    return {KindHeader, quint32(~0u)};
  if (idx.internalId() == 0)
    return idx.row() < int(m_sections.size()) ? Row {KindHeader, quint32(m_sections[size_t(idx.row())].section)}
                                              : Row {KindHeader, quint32(~0u)};
  const size_t si = size_t(idx.internalId() - 1);
  if (si >= m_sections.size() || idx.row() >= int(m_sections[si].rows.size()))
    return {KindHeader, quint32(~0u)};
  return m_sections[si].rows[size_t(idx.row())];
}

QVariant SceneTreeModel::data(const QModelIndex &idx, int role) const {
  const Row row = rowAt(idx);
  if (row.kind == KindHeader && row.id == ~0u)
    return {};
  if (role == KindRole)
    return int(row.kind);
  if (role == IdRole)
    return row.id;

  if (row.kind == KindHeader) {
    if (idx.column() != ColName)
      return {};
    if (role == Qt::DisplayRole) {
      const int n = rowCount(idx.siblingAtColumn(0));
      switch (Section(row.id)) {
        case SectionGroups: return tr("GROUPS  ·  %1").arg(n);
        case SectionGenerated: return tr("GENERATED  ·  %1").arg(n);
        case SectionMirrors: return tr("MIRRORS  ·  %1").arg(n);
        case SectionPaths: return tr("PATHS  ·  %1").arg(n);
        case SectionModified: return tr("MODIFIED  ·  %1").arg(n);
        case SectionEntities: return tr("ENTITIES  ·  %1").arg(n);
      }
      return {};
    }
    if (role == Qt::ForegroundRole)
      return QColor(0x8a, 0x9a, 0xb8);
    if (role == Qt::FontRole) {
      QFont f = QApplication::font();
      f.setBold(true);
      return f;
    }
    return {};
  }

  if (row.kind == KindGroup) {
    const eng::Scene *scene = m_engine && m_engine->scene() ? m_engine->scene().data() : nullptr;
    const eng::MeshGroup *g = scene ? scene->findGroup(row.id) : nullptr;
    if (!g)
      return {};
    const bool active = m_engine->activeMeshGroup() == g->id;
    const int count = m_counts.value(g->id);
    const int sel = m_selCounts.value(g->id);
    switch (idx.column()) {
      case ColName:
        if (role == Qt::DisplayRole) {
          QString t = QStringLiteral("%1   %2").arg(g->name).arg(count);
          if (sel > 0)
            t += tr("  (%1 selected)").arg(sel);
          return t;
        }
        if (role == Qt::EditRole)
          return g->name;
        if (role == Qt::DecorationRole)
          return swatch(g->color, active);
        if (role == Qt::FontRole && active) {
          QFont f = QApplication::font();
          f.setBold(true);
          return f;
        }
        if (role == Qt::ForegroundRole && g->hidden)
          return QColor(0x6a, 0x74, 0x90);
        if (role == Qt::ToolTipRole)
          return tr("<b>%1</b> · %2 brushes%3")
              .arg(g->name.toHtmlEscaped())
              .arg(count)
              .arg(active ? tr(" · <i>active</i>") : QString());
        break;
      case ColVisible:
        if (role == Qt::DecorationRole)
          return ActionManager::icon(g->hidden ? QStringLiteral("hide_on") : QStringLiteral("hide_off"));
        if (role == Qt::ToolTipRole)
          return tr("Show / hide the group");
        break;
      case ColLocked:
        if (role == Qt::DecorationRole)
          return ActionManager::icon(g->locked ? QStringLiteral("locked") : QStringLiteral("unlocked"));
        if (role == Qt::ToolTipRole)
          return tr("Lock / unlock the group");
        break;
      default:
        break;
    }
    if (role == Qt::BackgroundRole && sel > 0)
      return QColor(72, 102, 197, 90);
    return {};
  }

  if (row.kind == KindProc) {
    const auto members = m_engine ? m_engine->procMembers(row.id) : std::vector<quint32> {};
    int sel = 0, hidden = 0, locked = 0;
    for (const quint32 id: members) {
      sel += m_engine->selectedObjects().count(id) ? 1 : 0;
      hidden += m_engine->objectHidden(id) ? 1 : 0;
      locked += m_engine->objectLocked(id) ? 1 : 0;
    }
    const bool allHidden = !members.empty() && hidden == int(members.size());
    const bool allLocked = !members.empty() && locked == int(members.size());
    switch (idx.column()) {
      case ColName:
        if (role == Qt::DisplayRole)
          return tr("%1   %n brush(es)", "", int(members.size())).arg(m_engine->procName(row.id));
        if (role == Qt::EditRole)
          return m_engine->procName(row.id);
        if (role == Qt::DecorationRole)
        {
          const QString t = m_engine->procType(row.id);
          return ActionManager::icon(t == QLatin1String("stairs")     ? QStringLiteral("mod_array")
                                     : t == QLatin1String("arch")     ? QStringLiteral("mod_bevel")
                                     : t == QLatin1String("cylinder") ? QStringLiteral("mesh_cylinder")
                                     : t == QLatin1String("capsule")  ? QStringLiteral("mesh_capsule")
                                     : t == QLatin1String("pyramid")  ? QStringLiteral("mesh_cone")
                                                                      : QStringLiteral("modifier"));
        }
        if (role == Qt::ForegroundRole && allHidden)
          return QColor(0x6a, 0x74, 0x90);
        if (role == Qt::ToolTipRole)
          return tr("<b>%1</b> · generated %2")
              .arg(m_engine->procName(row.id).toHtmlEscaped(), m_engine->procType(row.id));
        break;
      case ColVisible:
        if (role == Qt::DecorationRole)
          return ActionManager::icon(allHidden ? QStringLiteral("hide_on") : QStringLiteral("hide_off"));
        break;
      case ColLocked:
        if (role == Qt::DecorationRole)
          return ActionManager::icon(allLocked ? QStringLiteral("locked") : QStringLiteral("unlocked"));
        break;
      default:
        break;
    }
    if (role == Qt::BackgroundRole && sel > 0)
      return QColor(72, 102, 197, 90);
    return {};
  }

  if (row.kind == KindHost) {
    const eng::mods::Host *h = m_engine ? m_engine->modHost(row.id) : nullptr;
    if (!h)
      return {};
    const std::vector<quint32> sources = m_engine->modSources(row.id);
    int hidden = 0, locked = 0;
    for (const quint32 id: sources) {
      hidden += m_engine->objectHidden(id) ? 1 : 0;
      locked += m_engine->objectLocked(id) ? 1 : 0;
    }
    const bool allHidden = !sources.empty() && hidden == int(sources.size());
    const bool allLocked = !sources.empty() && locked == int(sources.size());
    const vk::Engine3D::ModStatus st = m_engine->modStatus(row.id);
    switch (idx.column()) {
      case ColName:
        if (role == Qt::DisplayRole) {
          QString t = tr("%1   %n modifier(s)", "", int(h->stack.size())).arg(h->name);
          if (st.error)
            t += tr("  · error");
          else if (st.active)
            t += tr("  · %n piece(s)", "", st.outputs);
          if (m_engine->editSourceHost() == row.id)
            t += tr("  · editing source");
          if (const auto it = m_hostMirrors.constFind(row.id); it != m_hostMirrors.constEnd())
            t += tr("  · mirrored in %1").arg(it->join(QStringLiteral(", ")));
          return t;
        }
        if (role == Qt::EditRole)
          return h->name;
        if (role == Qt::DecorationRole)
          return ActionManager::icon(QStringLiteral("modifier"));
        if (role == Qt::ForegroundRole && st.error)
          return QColor(0xff, 0x6b, 0x6b);
        if (role == Qt::ForegroundRole && allHidden)
          return QColor(0x6a, 0x74, 0x90);
        if (role == Qt::ToolTipRole) {
          const auto it = m_hostMirrors.constFind(row.id);
          return tr("<b>%1</b> · modified object%2")
              .arg(h->name.toHtmlEscaped(),
                   it == m_hostMirrors.constEnd()
                       ? QString()
                       : tr("<br>Its sources are mirrored in %1").arg(it->join(QStringLiteral(", ")).toHtmlEscaped()));
        }
        break;
      case ColVisible:
        if (role == Qt::DecorationRole)
          return ActionManager::icon(allHidden ? QStringLiteral("hide_on") : QStringLiteral("hide_off"));
        break;
      case ColLocked:
        if (role == Qt::DecorationRole)
          return ActionManager::icon(allLocked ? QStringLiteral("locked") : QStringLiteral("unlocked"));
        break;
      default:
        break;
    }
    if (role == Qt::BackgroundRole && m_engine->selectedMods().count(row.id))
      return QColor(72, 102, 197, 90);
    return {};
  }

  if (row.kind == KindPath) {
    const eng::path::Path *p = m_engine ? m_engine->path(row.id) : nullptr;
    if (!p)
      return {};
    switch (idx.column()) {
      case ColName:
        if (role == Qt::DisplayRole) {
          const QString name = p->name.isEmpty() ? tr("Path %1").arg(p->id) : p->name;
          if (p->kind == eng::path::Path::Kind::Connect)
            return tr("%1   connection").arg(name);
          return tr("%1   %n point(s)", "", int(p->points.size())).arg(name);
        }
        if (role == Qt::EditRole)
          return p->name;
        if (role == Qt::DecorationRole)
          return ActionManager::icon(p->kind == eng::path::Path::Kind::Connect ? QStringLiteral("con_followpath")
                                                                              : QStringLiteral("curve_bezcurve"));
        if (role == Qt::ForegroundRole && p->hidden)
          return QColor(0x6a, 0x74, 0x90);
        break;
      case ColVisible:
        if (role == Qt::DecorationRole)
          return ActionManager::icon(p->hidden ? QStringLiteral("hide_on") : QStringLiteral("hide_off"));
        break;
      case ColLocked:
        if (role == Qt::DecorationRole)
          return ActionManager::icon(p->locked ? QStringLiteral("locked") : QStringLiteral("unlocked"));
        break;
      default:
        break;
    }
    if (role == Qt::BackgroundRole && m_engine->selectedPaths().count(row.id))
      return QColor(72, 102, 197, 90);
    return {};
  }

  if (row.kind == KindMirror) {
    const eng::Scene *scene = m_engine && m_engine->scene() ? m_engine->scene().data() : nullptr;
    const eng::Scene::MirrorSet *st = scene ? scene->findMirrorSet(row.id) : nullptr;
    if (!st)
      return {};
    const std::vector<quint32> &members = mirrorMembers(row.id);
    int sel = 0, hidden = 0, locked = 0;
    for (const quint32 id: members) {
      sel += m_engine->selectedObjects().count(id) ? 1 : 0;
      hidden += m_engine->objectHidden(id) ? 1 : 0;
      locked += m_engine->objectLocked(id) ? 1 : 0;
    }
    const bool allHidden = !members.empty() && hidden == int(members.size());
    const bool allLocked = !members.empty() && locked == int(members.size());
    switch (idx.column()) {
      case ColName:
        if (role == Qt::DisplayRole) {
          QString t = tr("%1   %n brush(es)", "", int(members.size())).arg(st->name);
          if (st->hideMirrored)
            t += tr("  · copies hidden");
          if (const auto it = m_mirrorHosts.constFind(row.id); it != m_mirrorHosts.constEnd())
            t += tr("  · modified by %1").arg(it->join(QStringLiteral(", ")));
          return t;
        }
        if (role == Qt::EditRole)
          return st->name;
        if (role == Qt::DecorationRole)
          return ActionManager::icon(QStringLiteral("mod_mirror"));
        if (role == Qt::ForegroundRole && allHidden)
          return QColor(0x6a, 0x74, 0x90);
        if (role == Qt::ToolTipRole) {
          const auto it = m_mirrorHosts.constFind(row.id);
          return tr("<b>%1</b> · mirror%2")
              .arg(st->name.toHtmlEscaped(),
                   it == m_mirrorHosts.constEnd()
                       ? QString()
                       : tr("<br>Its brushes are sources of %1").arg(it->join(QStringLiteral(", ")).toHtmlEscaped()));
        }
        break;
      case ColVisible:
        if (role == Qt::DecorationRole)
          return ActionManager::icon(allHidden ? QStringLiteral("hide_on") : QStringLiteral("hide_off"));
        if (role == Qt::ToolTipRole)
          return tr("Show / hide the mirror (both sides)");
        break;
      case ColLocked:
        if (role == Qt::DecorationRole)
          return ActionManager::icon(allLocked ? QStringLiteral("locked") : QStringLiteral("unlocked"));
        break;
      default:
        break;
    }
    if (role == Qt::BackgroundRole && sel > 0)
      return QColor(72, 102, 197, 90);
    return {};
  }

  if (idx.column() == ColName) {
    if (role == Qt::DisplayRole) {
      QString t = m_engine ? m_engine->entityDisplayName(row.id) : QString();
      if (const eng::Entity *e = m_engine ? m_engine->entityById(row.id) : nullptr)
        if (const eng::EntityClass *c = e->cls(); c && c->isBrushEntity())
          t += tr("   %n brush(es)", "", int(m_engine->brushesOfEntity(row.id).size()));
      return t;
    }
    if (role == Qt::DecorationRole) {
      if (const eng::Entity *e = m_engine ? m_engine->entityById(row.id) : nullptr)
        if (const eng::EntityClass *c = e->cls())
          return ActionManager::icon(c->icon);
      return ActionManager::icon(QStringLiteral("light_point"));
    }
  }
  return {};
}

Qt::ItemFlags SceneTreeModel::flags(const QModelIndex &idx) const {
  const Row row = rowAt(idx);
  if (row.kind == KindHeader && row.id == ~0u)
    return Qt::NoItemFlags;
  switch (row.kind) {
    case KindEntity:
      return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
    case KindGroup:
      // Not selectable: a row acts on its brushes when clicked.
      return Qt::ItemIsEnabled;
    default:
      return Qt::ItemIsEnabled;
  }
}

QVariant SceneTreeModel::headerData(int section, Qt::Orientation o, int role) const {
  if (o != Qt::Horizontal || role != Qt::DisplayRole)
    return {};
  return section == ColName ? tr("Name") : QString();
}

Outliner::Outliner(vk::Engine3D *engine, QWidget *parent) : QWidget(parent), m_engine(engine) {
  auto *v = new QVBoxLayout(this);
  v->setContentsMargins(4, 4, 4, 4);
  v->setSpacing(4);

  auto *bar = new QHBoxLayout;
  bar->setSpacing(4);
  auto *newGroup = new QToolButton(this);
  newGroup->setText(tr("＋ Group"));
  newGroup->setToolTip(tr("New mesh group (the selected brushes move into it)"));
  newGroup->setToolButtonStyle(Qt::ToolButtonTextOnly);
  auto *moveHere = new QToolButton(this);
  moveHere->setText(tr("Move Here"));
  moveHere->setToolTip(tr("Move the selected brushes into the active group"));
  bar->addWidget(newGroup);
  bar->addWidget(moveHere);
  bar->addStretch(1);
  v->addLayout(bar);
  m_search = new QLineEdit(this);
  m_search->setPlaceholderText(tr("Filter…"));
  m_search->setClearButtonEnabled(true);
  v->addWidget(m_search);

  connect(newGroup, &QToolButton::clicked, this, [this] {
    if (!m_engine)
      return;
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("New Mesh Group"), tr("Name"), QLineEdit::Normal,
                                               m_engine->uniqueGroupName(QStringLiteral("group")), &ok);
    if (ok && !name.trimmed().isEmpty())
      m_engine->createMeshGroup(name, true, false); // never activates
  });
  connect(moveHere, &QToolButton::clicked, this, [this] {
    if (m_engine)
      m_engine->moveSelectionToGroup(m_engine->activeMeshGroup());
  });

  m_model = new SceneTreeModel(engine, this);
  m_proxy = new QSortFilterProxyModel(this);
  m_proxy->setSourceModel(m_model);
  m_proxy->setFilterCaseSensitivity(Qt::CaseInsensitive);
  m_proxy->setFilterKeyColumn(SceneTreeModel::ColName);
  m_proxy->setRecursiveFilteringEnabled(true); // a matching row keeps its section header

  m_view = new QTreeView(this);
  m_view->setModel(m_proxy);
  m_view->setUniformRowHeights(true);
  m_view->setRootIsDecorated(true);
  m_view->setItemsExpandable(true);
  // Clicks go through activateIndex; the view only shows the engine's selection.
  m_view->setSelectionMode(QAbstractItemView::NoSelection);
  m_view->setEditTriggers(QAbstractItemView::NoEditTriggers);
  m_view->setContextMenuPolicy(Qt::CustomContextMenu);
  m_view->header()->setStretchLastSection(false);
  m_view->header()->setSectionResizeMode(SceneTreeModel::ColName, QHeaderView::Stretch);
  m_view->header()->setSectionResizeMode(SceneTreeModel::ColVisible, QHeaderView::Fixed);
  m_view->header()->setSectionResizeMode(SceneTreeModel::ColLocked, QHeaderView::Fixed);
  scale::bind(m_view, [this] {
    m_view->setIndentation(scale::px(14));
    m_view->header()->setMinimumSectionSize(scale::px(20));
    m_view->header()->resizeSection(SceneTreeModel::ColVisible, scale::px(26));
    m_view->header()->resizeSection(SceneTreeModel::ColLocked, scale::px(26));
    m_view->setIconSize(scale::icon(scale::Icon::Small));
  });
  m_view->header()->hide();
  v->addWidget(m_view, 1);

  m_summary = new QLabel(this);
  m_summary->setStyleSheet(QStringLiteral("color:#8a9ab8;"));
  v->addWidget(m_summary);

  // Structure changes come in bursts (a CSG op adds and removes); coalesce.
  m_rebuildTimer = new QTimer(this);
  m_rebuildTimer->setSingleShot(true);
  m_rebuildTimer->setInterval(60);
  connect(m_rebuildTimer, &QTimer::timeout, this, [this] {
    m_model->rebuild();
    restoreExpansion();
    pullSelectionFromEngine();
  });

  // Collapsed sections persist across rebuilds and sessions.
  {
    const QStringList saved = QSettings().value(QStringLiteral("Outliner/Collapsed")).toStringList();
    for (const QString &k: saved)
      m_collapsed.insert(k.toInt());
  }
  auto noteExpansion = [this](const QModelIndex &pidx, const bool expanded) {
    if (m_restoring)
      return;
    const QModelIndex idx = m_proxy->mapToSource(pidx);
    if (idx.data(SceneTreeModel::KindRole).toInt() != SceneTreeModel::KindHeader)
      return;
    const int section = int(idx.data(SceneTreeModel::IdRole).toUInt());
    if (expanded)
      m_collapsed.remove(section);
    else
      m_collapsed.insert(section);
    QStringList keys;
    for (const int k: std::as_const(m_collapsed))
      keys << QString::number(k);
    QSettings().setValue(QStringLiteral("Outliner/Collapsed"), keys);
  };
  connect(m_view, &QTreeView::expanded, this, [noteExpansion](const QModelIndex &i) { noteExpansion(i, true); });
  connect(m_view, &QTreeView::collapsed, this, [noteExpansion](const QModelIndex &i) { noteExpansion(i, false); });

  connect(m_search, &QLineEdit::textChanged, this, [this](const QString &t) {
    m_proxy->setFilterFixedString(t);
    restoreExpansion();
  });
  connect(m_view, &QTreeView::doubleClicked, this, [this](const QModelIndex &pidx) {
    const QModelIndex idx = m_proxy->mapToSource(pidx);
    const int kind = idx.data(SceneTreeModel::KindRole).toInt();
    if (kind == SceneTreeModel::KindGroup && idx.column() == SceneTreeModel::ColName)
      renameGroup(idx.data(SceneTreeModel::IdRole).toUInt());
    else if (kind == SceneTreeModel::KindProc && idx.column() == SceneTreeModel::ColName) {
      const quint32 id = idx.data(SceneTreeModel::IdRole).toUInt();
      bool ok = false;
      const QString name = QInputDialog::getText(this, tr("Rename"), tr("Name"), QLineEdit::Normal,
                                                 m_engine->procName(id), &ok);
      if (ok)
        m_engine->renameProc(id, name);
    }
    else if (kind == SceneTreeModel::KindHost && idx.column() == SceneTreeModel::ColName) {
      const quint32 id = idx.data(SceneTreeModel::IdRole).toUInt();
      const auto *h = m_engine->modHost(id);
      bool ok = false;
      const QString name = QInputDialog::getText(this, tr("Rename"), tr("Name"), QLineEdit::Normal,
                                                 h ? h->name : QString(), &ok);
      if (ok)
        m_engine->renameModHost(id, name);
    }
    else if (kind == SceneTreeModel::KindPath && idx.column() == SceneTreeModel::ColName) {
      const quint32 id = idx.data(SceneTreeModel::IdRole).toUInt();
      const auto *p = m_engine->path(id);
      bool ok = false;
      const QString name = QInputDialog::getText(this, tr("Rename Path"), tr("Name"), QLineEdit::Normal,
                                                 p ? p->name : QString(), &ok);
      if (ok)
        m_engine->renamePath(id, name);
    }
    else if (kind == SceneTreeModel::KindMirror && idx.column() == SceneTreeModel::ColName) {
      const quint32 id = idx.data(SceneTreeModel::IdRole).toUInt();
      const auto *st = m_engine->scene() ? m_engine->scene()->findMirrorSet(id) : nullptr;
      bool ok = false;
      const QString name = QInputDialog::getText(this, tr("Rename Mirror"), tr("Name"), QLineEdit::Normal,
                                                 st ? st->name : QString(), &ok);
      if (ok)
        m_engine->renameMirrorSet(id, name);
    }
    else if (kind == SceneTreeModel::KindEntity)
      m_engine->focusSelection();
  });
  connect(m_view, &QTreeView::clicked, this, [this](const QModelIndex &pidx) { activateIndex(pidx); });
  connect(m_view, &QTreeView::customContextMenuRequested, this, &Outliner::showContextMenu);

  if (engine) {
    connect(engine, &vk::Engine3D::sceneChanged, m_rebuildTimer, qOverload<>(&QTimer::start));
    connect(engine, &vk::Engine3D::entitiesChanged, m_rebuildTimer, qOverload<>(&QTimer::start));
    connect(engine, &vk::Engine3D::meshGroupsChanged, m_rebuildTimer, qOverload<>(&QTimer::start));
    connect(engine, &vk::Engine3D::procObjectsChanged, m_rebuildTimer, qOverload<>(&QTimer::start));
    connect(engine, &vk::Engine3D::mirrorChanged, m_rebuildTimer, qOverload<>(&QTimer::start));
    connect(engine, &vk::Engine3D::modHostsChanged, m_rebuildTimer, qOverload<>(&QTimer::start));
    connect(engine, &vk::Engine3D::pathsChanged, m_rebuildTimer, qOverload<>(&QTimer::start));
    connect(engine, &vk::Engine3D::editSourceChanged, m_rebuildTimer, qOverload<>(&QTimer::start));
    connect(engine, &vk::Engine3D::modOutputsChanged, this, [this] { m_model->refreshRows(); });
    connect(engine, &vk::Engine3D::objectStateChanged, this, [this] {
      m_model->refreshRows();
      updateSummary();
    });
    connect(engine, &vk::Engine3D::selectionChanged, this, &Outliner::pullSelectionFromEngine);
  }
  m_rebuildTimer->start();
}

void Outliner::restoreExpansion() {
  m_restoring = true;
  for (int r = 0; r < m_proxy->rowCount(); ++r) {
    const QModelIndex pidx = m_proxy->index(r, 0);
    const int section = int(m_proxy->mapToSource(pidx).data(SceneTreeModel::IdRole).toUInt());
    // While filtering, every section with a match is open.
    m_view->setExpanded(pidx, !m_search->text().isEmpty() || !m_collapsed.contains(section));
  }
  m_restoring = false;
}

std::vector<quint32> Outliner::rowBrushes(const int kind, const quint32 id) const {
  std::vector<quint32> out;
  if (!m_engine || !m_engine->scene())
    return out;
  const auto &scene = m_engine->scene();
  auto keep = [&](const quint32 sid) {
    const auto o = scene->findById(sid);
    if (o && !o->isDerived() && !o->hidden() && !o->locked())
      out.push_back(sid);
  };
  if (kind == SceneTreeModel::KindGroup) {
    for (const auto &o: scene->gameObjects)
      if (o->group() == id)
        keep(o->stableId());
  } else if (kind == SceneTreeModel::KindProc) {
    for (const quint32 sid: m_engine->procMembers(id))
      keep(sid);
  } else if (kind == SceneTreeModel::KindMirror) {
    for (const quint32 sid: m_model->mirrorMembers(id))
      keep(sid);
  }
  return out;
}

void Outliner::pickRow(const QModelIndex &idx, const Pick mode) {
  if (!m_engine)
    return;
  const int kind = idx.data(SceneTreeModel::KindRole).toInt();
  const quint32 id = idx.data(SceneTreeModel::IdRole).toUInt();
  switch (kind) {
    case SceneTreeModel::KindEntity:
      if (mode == Pick::Replace)
        m_engine->selectMarker(id, false);
      else if (mode == Pick::Toggle || !m_engine->markerSelected(id))
        m_engine->selectMarker(id, true);
      return;
    case SceneTreeModel::KindHost:
      if (mode == Pick::Replace)
        m_engine->selectModHost(id, false);
      else if (mode == Pick::Toggle || !m_engine->selectedMods().count(id))
        m_engine->selectModHost(id, true);
      return;
    case SceneTreeModel::KindPath:
      if (mode == Pick::Replace)
        m_engine->selectPath(id, false);
      else if (mode == Pick::Toggle || !m_engine->selectedPaths().count(id))
        m_engine->selectPath(id, true);
      return;
    case SceneTreeModel::KindGroup:
    case SceneTreeModel::KindProc:
    case SceneTreeModel::KindMirror: {
      // The row's visible, unlocked brushes.
      const std::vector<quint32> members = rowBrushes(kind, id);
      if (mode == Pick::Replace) {
        m_engine->clearSelection();
        m_engine->setSelected(members, {});
        return;
      }
      const std::set<quint32> &cur = m_engine->selectedObjects();
      const bool all = !members.empty()
                       && std::all_of(members.begin(), members.end(), [&](quint32 m) { return cur.count(m) != 0; });
      std::vector<quint32> sel;
      if (mode == Pick::Toggle && all) {
        const std::set<quint32> drop(members.begin(), members.end());
        for (const quint32 m: cur)
          if (!drop.count(m))
            sel.push_back(m);
      } else {
        sel.assign(cur.begin(), cur.end());
        sel.insert(sel.end(), members.begin(), members.end());
      }
      m_engine->setSelected(sel, {});
      return;
    }
    default:
      return;
  }
}

void Outliner::activateIndex(const QModelIndex &pidx) {
  const QModelIndex idx = m_proxy->mapToSource(pidx);
  const int kind = idx.data(SceneTreeModel::KindRole).toInt();
  if (!m_engine || kind == SceneTreeModel::KindHeader)
    return;
  const quint32 id = idx.data(SceneTreeModel::IdRole).toUInt();

  if (idx.column() == SceneTreeModel::ColVisible || idx.column() == SceneTreeModel::ColLocked) {
    const bool eye = idx.column() == SceneTreeModel::ColVisible;
    if (kind == SceneTreeModel::KindPath) {
      if (const eng::path::Path *p = m_engine->path(id)) {
        if (eye)
          m_engine->setPathHidden(id, !p->hidden);
        else
          m_engine->setPathLocked(id, !p->locked);
      }
      return;
    }
    if (kind == SceneTreeModel::KindGroup) {
      if (const eng::MeshGroup *g = m_engine->scene() ? m_engine->scene()->findGroup(id) : nullptr) {
        if (eye)
          m_engine->setMeshGroupHidden(id, !g->hidden);
        else
          m_engine->setMeshGroupLocked(id, !g->locked);
      }
      return;
    }
    std::vector<quint32> members;
    if (kind == SceneTreeModel::KindHost)
      members = m_engine->modSources(id);
    else if (kind == SceneTreeModel::KindMirror)
      members = m_model->mirrorMembers(id);
    else if (kind == SceneTreeModel::KindProc)
      members = m_engine->procMembers(id);
    if (members.empty())
      return;
    bool all = true;
    for (const quint32 m: members)
      all = all && (eye ? m_engine->objectHidden(m) : m_engine->objectLocked(m));
    if (eye)
      m_engine->setObjectsHidden(members, !all);
    else
      m_engine->setObjectsLocked(members, !all);
    return;
  }

  // Plain click replaces, Ctrl toggles, Shift adds the rows from the last one clicked.
  const Qt::KeyboardModifiers mods = QApplication::keyboardModifiers();
  if (mods.testFlag(Qt::ShiftModifier) && m_anchor.isValid() && m_anchor != pidx.siblingAtColumn(0)) {
    std::vector<QModelIndex> rows;
    for (int r = 0; r < m_proxy->rowCount(); ++r) {
      const QModelIndex head = m_proxy->index(r, 0);
      if (!m_view->isExpanded(head))
        continue;
      for (int c = 0; c < m_proxy->rowCount(head); ++c)
        rows.push_back(m_proxy->index(c, 0, head));
    }
    const auto a = std::find(rows.begin(), rows.end(), QModelIndex(m_anchor));
    const auto b = std::find(rows.begin(), rows.end(), pidx.siblingAtColumn(0));
    if (a != rows.end() && b != rows.end()) {
      const auto [lo, hi] = std::minmax(a, b);
      m_syncing = true; // one refresh at the end, not one per row
      for (auto it = lo; it != hi + 1; ++it)
        pickRow(m_proxy->mapToSource(*it), Pick::Add);
      m_syncing = false;
      pullSelectionFromEngine();
      return;
    }
  }
  m_anchor = pidx.siblingAtColumn(0);
  pickRow(idx, mods & (Qt::ControlModifier | Qt::ShiftModifier) ? Pick::Toggle : Pick::Replace);
}

void Outliner::renameGroup(const quint32 id) {
  if (!m_engine || !m_engine->scene() || id == eng::kWorldspawnGroup)
    return;
  const eng::MeshGroup *g = m_engine->scene()->findGroup(id);
  if (!g)
    return;
  bool ok = false;
  const QString name = QInputDialog::getText(this, tr("Rename Group"), tr("Name"), QLineEdit::Normal, g->name, &ok);
  if (ok)
    m_engine->renameMeshGroup(id, name);
}

void Outliner::updateSummary() {
  if (!m_engine)
    return;
  const size_t n = m_engine->selectedObjects().size();
  const QString groups = tr("%n group(s)", "", m_model->groupCount());
  m_summary->setText(n == 0 ? tr("%1 brushes · %2").arg(m_model->brushCount()).arg(groups)
                            : tr("%1 selected of %2 brushes · %3").arg(n).arg(m_model->brushCount()).arg(groups));
}

void Outliner::pullSelectionFromEngine() {
  if (m_syncing || !m_engine)
    return;
  m_syncing = true;
  m_model->refreshRows();
  QItemSelection sel;
  QModelIndex first;
  auto add = [&](quint32 eid) {
    const QModelIndex p = m_proxy->mapFromSource(m_model->indexForEntity(eid));
    if (!p.isValid())
      return;
    sel.select(p, p.siblingAtColumn(SceneTreeModel::ColumnCount - 1));
    if (!first.isValid())
      first = p;
  };
  for (const quint32 eid: m_engine->selectedEntities())
    add(eid);
  m_view->selectionModel()->select(sel, QItemSelectionModel::ClearAndSelect);
  if (first.isValid())
    m_view->scrollTo(first);
  updateSummary();
  m_syncing = false;
}

void Outliner::showContextMenu(const QPoint &pos) {
  QMenu menu(this);
  const QModelIndex idx = m_proxy->mapToSource(m_view->indexAt(pos));
  if (idx.data(SceneTreeModel::KindRole).toInt() == SceneTreeModel::KindGroup && m_engine && m_engine->scene()) {
    const quint32 gid = idx.data(SceneTreeModel::IdRole).toUInt();
    const eng::MeshGroup *g = m_engine->scene()->findGroup(gid);
    if (!g)
      return;
    const eng::MeshGroup group = *g;
    const bool world = gid == eng::kWorldspawnGroup;
    menu.addSection(group.name);
    QAction *active = menu.addAction(tr("Set as Active Group"), [this, gid] { m_engine->setActiveMeshGroup(gid); });
    active->setCheckable(true);
    active->setChecked(m_engine->activeMeshGroup() == gid);
    menu.addAction(tr("Select Brushes"), [this, gid] { m_engine->selectMeshGroup(gid, false); });
    menu.addAction(tr("Move Selection Here"), [this, gid] { m_engine->moveSelectionToGroup(gid); })
        ->setEnabled(!m_engine->selectedObjects().empty());
    menu.addSeparator();
    menu.addAction(group.hidden ? tr("Show") : tr("Hide"),
                   [this, gid, h = group.hidden] { m_engine->setMeshGroupHidden(gid, !h); });
    menu.addAction(tr("Solo (hide other groups)"), [this, gid] { m_engine->soloMeshGroup(gid); });
    menu.addAction(group.locked ? tr("Unlock") : tr("Lock"),
                   [this, gid, l = group.locked] { m_engine->setMeshGroupLocked(gid, !l); });
    menu.addSeparator();
    menu.addAction(tr("Rename…"), [this, gid] { renameGroup(gid); })->setEnabled(!world);
    menu.addAction(tr("Colour…"), [this, gid, c = group.color] {
      const QColor picked = QColorDialog::getColor(c, this, tr("Group Colour"));
      if (picked.isValid())
        m_engine->setMeshGroupColor(gid, picked);
    });
    menu.addSeparator();
    menu.addAction(tr("Delete Group (brushes to worldspawn)"), [this, gid] { m_engine->deleteMeshGroup(gid, false); })
        ->setEnabled(!world);
    menu.addAction(tr("Delete Group and Its Brushes…"), [this, gid, group] {
          if (QMessageBox::question(this, tr("Delete Group"),
                                    tr("Delete \"%1\" and all of its brushes?").arg(group.name))
              == QMessageBox::Yes)
            m_engine->deleteMeshGroup(gid, true);
        })
        ->setEnabled(!world);
  } else if (idx.data(SceneTreeModel::KindRole).toInt() == SceneTreeModel::KindHost && m_engine) {
    const quint32 id = idx.data(SceneTreeModel::IdRole).toUInt();
    const auto *h = m_engine->modHost(id);
    if (!h)
      return;
    menu.addSection(h->name);
    menu.addAction(tr("Select"), [this, id] { m_engine->selectModHost(id, false); });
    menu.addAction(tr("Edit Source"), [this, id] { m_engine->setEditSourceHost(id); });
    menu.addSeparator();
    menu.addAction(tr("Apply All Modifiers"), [this, id] { m_engine->applyModifiers(id, false); });
    menu.addAction(tr("Remove Modifiers (keep the originals)"), [this, id] { m_engine->dissolveModHost(id); });
  } else if (idx.data(SceneTreeModel::KindRole).toInt() == SceneTreeModel::KindPath && m_engine) {
    const quint32 id = idx.data(SceneTreeModel::IdRole).toUInt();
    const auto *p = m_engine->path(id);
    if (!p)
      return;
    menu.addSection(p->name.isEmpty() ? tr("Path %1").arg(id) : p->name);
    menu.addAction(tr("Select"), [this, id] { m_engine->selectPath(id, false); });
    if (p->kind == eng::path::Path::Kind::Connect)
      menu.addAction(tr("Convert to Drawn Path"), [this, id] { m_engine->convertPathToDrawn(id); });
    menu.addAction(tr("Delete"), [this, id] { m_engine->deletePaths({id}); });
  } else if (idx.data(SceneTreeModel::KindRole).toInt() == SceneTreeModel::KindMirror && m_engine && m_engine->scene()) {
    const quint32 id = idx.data(SceneTreeModel::IdRole).toUInt();
    const eng::Scene::MirrorSet *st = m_engine->scene()->findMirrorSet(id);
    if (!st)
      return;
    const QString name = st->name;
    const bool copiesHidden = st->hideMirrored;
    menu.addSection(name);
    menu.addAction(tr("Select"), [this, id] { m_engine->setSelected(m_model->mirrorMembers(id), {}); });
    menu.addAction(tr("Rename…"), [this, id, name] {
      bool ok = false;
      const QString n = QInputDialog::getText(this, tr("Rename Mirror"), tr("Name"), QLineEdit::Normal, name, &ok);
      if (ok)
        m_engine->renameMirrorSet(id, n);
    });
    QAction *hide = menu.addAction(tr("Hide Mirrored Copies"),
                                   [this, id, copiesHidden] { m_engine->setMirrorSetsHidden({id}, !copiesHidden); });
    hide->setCheckable(true);
    hide->setChecked(copiesHidden);
    menu.addSeparator();
    menu.addAction(tr("Detach (plain brushes)"), [this, id] { m_engine->dissolveMirrorSet(id); });
  } else if (idx.data(SceneTreeModel::KindRole).toInt() == SceneTreeModel::KindProc && m_engine) {
    const quint32 id = idx.data(SceneTreeModel::IdRole).toUInt();
    menu.addSection(m_engine->procName(id));
    menu.addAction(tr("Select"), [this, id] { m_engine->setSelected(m_engine->procMembers(id), {}); });
    menu.addAction(tr("Convert to Plain Brushes"), [this, id] { m_engine->dissolveProc(id); });
  } else {
    static const char *const kItems[] = {"group.new", "group.toActive", "group.toWorldspawn", nullptr,
                                         "view.focus", "edit.duplicate", "edit.delete", nullptr, "view.hide",
                                         "view.isolate", "view.showAll", nullptr, "edit.lock", "edit.unlock"};
    static const char *const kEntityItems[] = {"io.editor", nullptr, "view.focus", "edit.delete", nullptr, "io.selectTargets",
                                               "io.selectSources", "io.selectSameName", nullptr, "io.fireInput"};
    const bool entityRow = idx.data(SceneTreeModel::KindRole).toInt() == SceneTreeModel::KindEntity;
    std::vector<const char *> items;
    if (entityRow)
      items.assign(std::begin(kEntityItems), std::end(kEntityItems));
    else
      items.assign(std::begin(kItems), std::end(kItems));
    for (const char *id: items) {
      if (!id) {
        menu.addSeparator();
        continue;
      }
      if (QAction *a = act(QString::fromLatin1(id)))
        menu.addAction(a);
    }
  }
  menu.exec(m_view->viewport()->mapToGlobal(pos));
}

} // namespace ui
