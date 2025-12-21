#include "model_browser.h"

#include <QFileSystemModel>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMimeData>
#include <QPushButton>
#include <QTreeView>
#include <QVBoxLayout>

#include "client/engine/model_loader.h"
#include "client/prefs/preferences.h"
#include "client/prefs/preferences_dialog.h"
#include "client/vulkan/vk_engine.h"

namespace ui {

namespace {

class ModelFsModel final : public QFileSystemModel {
public:
  using QFileSystemModel::QFileSystemModel;
  QStringList mimeTypes() const override { return {QStringLiteral("application/x-editor-model")}; }
  QMimeData *mimeData(const QModelIndexList &idx) const override {
    if (idx.isEmpty() || isDir(idx.first()))
      return nullptr;
    auto *md = new QMimeData();
    md->setData(QStringLiteral("application/x-editor-model"), filePath(idx.first()).toUtf8());
    md->setText(filePath(idx.first()));
    return md;
  }
};

} // namespace

ModelBrowser::ModelBrowser(vk::Engine3D *engine, QWidget *parent) : QWidget(parent), m_engine(engine) {
  auto *v = new QVBoxLayout(this);
  v->setContentsMargins(4, 4, 4, 4);
  m_filter = new QLineEdit(this);
  m_filter->setPlaceholderText(tr("Filter models…"));
  m_filter->setClearButtonEnabled(true);
  v->addWidget(m_filter);

  m_fs = new ModelFsModel(this);
  m_fs->setNameFilters(eng::ModelCache::fileFilters());
  m_fs->setNameFilterDisables(false);
  m_view = new QTreeView(this);
  m_view->setModel(m_fs);
  m_view->setDragEnabled(true);
  m_view->setDragDropMode(QAbstractItemView::DragOnly);
  for (int c = 1; c < m_fs->columnCount(); ++c)
    m_view->hideColumn(c);
  m_view->header()->hide();
  v->addWidget(m_view, 1);

  m_info = new QLabel(this);
  m_info->setWordWrap(true);
  m_info->setStyleSheet(QStringLiteral("color:#8a9ab8;"));
  v->addWidget(m_info);
  auto *choose = new QPushButton(tr("Choose model folder…"), this);
  v->addWidget(choose);
  connect(choose, &QPushButton::clicked, this,
          [this] { prefs::PreferencesDialog::showDialog(this, QStringLiteral("Paths")); });

  connect(m_filter, &QLineEdit::textChanged, this, [this](const QString &t) {
    QStringList f;
    for (const QString &ext: eng::ModelCache::fileFilters())
      f << (t.isEmpty() ? ext : QStringLiteral("*%1%2").arg(t, ext.mid(1)));
    m_fs->setNameFilters(f);
  });
  connect(m_view, &QTreeView::doubleClicked, this, [this](const QModelIndex &idx) {
    if (m_fs->isDir(idx))
      return;
    const glm::vec3 at = m_engine->placementPoint(-1, -1, 0.0f);
    m_engine->addEntity(QStringLiteral("prop_static"), at, {{QStringLiteral("model"), m_fs->filePath(idx)}});
  });
}

void ModelBrowser::setRoot(const QString &dir) {
  if (dir.isEmpty()) {
    m_view->setRootIndex({});
    m_info->setText(tr("No model folder (Preferences › Paths)"));
    m_info->show();
    m_view->setEnabled(false);
    return;
  }
  m_view->setEnabled(true);
  m_view->setRootIndex(m_fs->setRootPath(dir));
  m_info->clear();
  m_info->hide();
}

} // namespace ui
