#include "entity_browser.h"
#include "client/ui/scale.h"

#include <QDrag>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMimeData>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "client/engine/entity.h"
#include "client/ui/action_manager.h"
#include "client/vulkan/vk_engine.h"

namespace ui {

namespace {

class DragTree final : public QTreeWidget {
public:
  using QTreeWidget::QTreeWidget;

protected:
  QStringList mimeTypes() const override { return {QStringLiteral("application/x-editor-entity")}; }
  QMimeData *mimeData(const QList<QTreeWidgetItem *> &items) const override {
    if (items.isEmpty() || items.first()->data(0, Qt::UserRole).toString().isEmpty())
      return nullptr;
    auto *md = new QMimeData();
    const QString cls = items.first()->data(0, Qt::UserRole).toString();
    md->setData(QStringLiteral("application/x-editor-entity"), cls.toUtf8());
    md->setText(cls);
    return md;
  }
};

} // namespace

EntityBrowser::EntityBrowser(vk::Engine3D *engine, QWidget *parent) : QWidget(parent), m_engine(engine) {
  auto *v = new QVBoxLayout(this);
  v->setContentsMargins(4, 4, 4, 4);
  m_search = new QLineEdit(this);
  m_search->setPlaceholderText(tr("Search entities…"));
  m_search->setClearButtonEnabled(true);
  v->addWidget(m_search);
  m_tree = new DragTree(this);
  m_tree->setHeaderHidden(true);
  m_tree->setDragEnabled(true);
  m_tree->setDragDropMode(QAbstractItemView::DragOnly);
  scale::bind(m_tree, [this] {
    m_tree->setIconSize(scale::icon(scale::Icon::Small));
    m_tree->setIndentation(scale::px(12));
  });
  v->addWidget(m_tree, 1);
  m_info = new QLabel(this);
  m_info->setWordWrap(true);
  m_info->setStyleSheet(QStringLiteral("color:#8a9ab8;"));
  m_info->hide();
  v->addWidget(m_info);

  connect(m_search, &QLineEdit::textChanged, this, &EntityBrowser::rebuild);
  connect(m_tree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item) {
    const QString cls = item->data(0, Qt::UserRole).toString();
    if (!cls.isEmpty())
      m_engine->placeEntityAt(cls);
  });
  connect(m_tree, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *item) {
    if (!item)
      return;
    if (const auto *c = eng::EntityRegistry::instance().find(item->data(0, Qt::UserRole).toString())) {
      m_info->show();
      m_info->setText(QStringLiteral("<b>%1</b> <span style='color:#6c7898'>%2</span><br>%3")
                          .arg(c->label.toHtmlEscaped(), c->name, c->description.toHtmlEscaped()));
    }
  });
  rebuild();
}

void EntityBrowser::rebuild() {
  m_tree->clear();
  const QString needle = m_search->text().trimmed();
  auto &reg = eng::EntityRegistry::instance();
  for (const QString &cat: reg.categories()) {
    auto *group = new QTreeWidgetItem(m_tree, {cat});
    group->setFlags(Qt::ItemIsEnabled);
    QFont f = group->font(0);
    f.setBold(true);
    group->setFont(0, f);
    for (const auto &c: reg.classes()) {
      if (!c.placeable)
        continue;
      if (c.category != cat)
        continue;
      if (!needle.isEmpty() && !c.label.contains(needle, Qt::CaseInsensitive) && !c.name.contains(needle, Qt::CaseInsensitive))
        continue;
      auto *it = new QTreeWidgetItem(group, {c.label});
      it->setIcon(0, ActionManager::icon(c.icon));
      it->setData(0, Qt::UserRole, c.name);
      it->setToolTip(0, QStringLiteral("%1\n%2").arg(c.name, c.description));
      it->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled);
    }
    if (group->childCount() == 0)
      delete group;
  }
  m_tree->expandAll();
}

} // namespace ui
