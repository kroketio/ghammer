#include "face_groups_panel.h"

#include <QApplication>
#include <QComboBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include "client/engine/face_select.h"
#include "client/ui/action_manager.h"
#include "client/vulkan/vk_engine.h"

namespace ui {

FaceGroupsPanel::FaceGroupsPanel(vk::Engine3D *engine, QWidget *parent) : QWidget(parent), m_engine(engine) {
  auto *v = new QVBoxLayout(this);
  v->setContentsMargins(0, 0, 0, 0);
  v->setSpacing(6);

  m_rows = new QWidget(this);
  m_rowsLayout = new QVBoxLayout(m_rows);
  m_rowsLayout->setContentsMargins(0, 0, 0, 0);
  m_rowsLayout->setSpacing(2);
  v->addWidget(m_rows);
  m_empty = new QLabel(tr("The selected faces are in no group."), this);
  m_empty->setStyleSheet(QStringLiteral("color:#8a9ab8;"));
  v->addWidget(m_empty);

  auto *addRow = new QHBoxLayout();
  auto *label = new QLabel(tr("Add to group"), this);
  m_name = new QComboBox(this);
  m_name->setEditable(true);
  m_name->setInsertPolicy(QComboBox::NoInsert);
  m_name->lineEdit()->setPlaceholderText(tr("name (new or existing)"));
  m_name->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  m_add = new QPushButton(tr("Add"), this);
  m_add->setToolTip(tr("Put the selected faces in this group (a new name makes a new group)"));
  addRow->addWidget(label);
  addRow->addWidget(m_name, 1);
  addRow->addWidget(m_add);
  v->addLayout(addRow);
  connect(m_add, &QPushButton::clicked, this, &FaceGroupsPanel::addToGroup);
  connect(m_name->lineEdit(), &QLineEdit::returnPressed, this, &FaceGroupsPanel::addToGroup);

  m_timer = new QTimer(this);
  m_timer->setSingleShot(true);
  m_timer->setInterval(40);
  connect(m_timer, &QTimer::timeout, this, &FaceGroupsPanel::refresh);
  for (auto sig: {&vk::Engine3D::selectionChanged, &vk::Engine3D::faceGroupsChanged, &vk::Engine3D::sceneChanged})
    connect(m_engine, sig, m_timer, qOverload<>(&QTimer::start));
  refresh();
}

void FaceGroupsPanel::addToGroup() {
  const QString name = m_name->currentText().trimmed();
  if (name.isEmpty())
    return;
  if (m_engine->addSelectionToFaceGroup(name))
    m_name->setEditText(QString());
}

void FaceGroupsPanel::rowMenu(const quint32 id, const bool builtin, const QPoint &globalPos) {
  QMenu menu(this);
  QAction *rename = menu.addAction(tr("Rename…"));
  QAction *del = menu.addAction(tr("Delete group"));
  rename->setEnabled(!builtin);
  del->setEnabled(!builtin);
  QAction *chosen = menu.exec(globalPos);
  const eng::FaceGroupTable *table = m_engine->faceGroupTable();
  if (!chosen || !table)
    return;
  const QString old = table->name(id);
  if (chosen == rename) {
    bool ok = false;
    const QString n = QInputDialog::getText(this, tr("Rename face group"), tr("Name"), QLineEdit::Normal, old, &ok);
    if (ok && !n.trimmed().isEmpty() && !m_engine->renameFaceGroup(id, n))
      QMessageBox::warning(this, tr("Rename face group"), tr("A group called \"%1\" already exists.").arg(n.trimmed()));
  } else if (chosen == del) {
    if (QMessageBox::question(this, tr("Delete face group"),
                              tr("Delete \"%1\"? Its faces stay as they are, only the group goes.").arg(old))
        == QMessageBox::Yes)
      m_engine->deleteFaceGroup(id);
  }
}

void FaceGroupsPanel::refresh() {
  while (QLayoutItem *item = m_rowsLayout->takeAt(0)) {
    if (QWidget *w = item->widget())
      w->deleteLater();
    delete item;
  }
  const int total = int(m_engine->selectedFaceRefs().size());
  const auto groups = m_engine->faceGroupsOfSelection();
  for (const auto &g: groups) {
    auto *rowW = new QWidget(m_rows);
    rowW->setContextMenuPolicy(Qt::CustomContextMenu);
    auto *h = new QHBoxLayout(rowW);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(4);
    auto *dot = new QLabel(QStringLiteral("●"), rowW);
    dot->setStyleSheet(QStringLiteral("color:%1;").arg(g.color.name()));
    auto *name = new QLabel(g.builtin ? QStringLiteral("<b>%1</b>").arg(g.name.toHtmlEscaped()) : g.name.toHtmlEscaped(), rowW);
    auto *count = new QLabel(QStringLiteral("%1/%2").arg(g.count).arg(total), rowW);
    count->setStyleSheet(QStringLiteral("color:#8a9ab8;"));
    count->setToolTip(tr("In %1 of the %2 selected faces").arg(g.count).arg(total));
    auto *sel = new QPushButton(tr("Select"), rowW);
    sel->setToolTip(tr("Select every face in \"%1\" (Shift adds)").arg(g.name));
    auto *contig = new QPushButton(tr("Contiguous"), rowW);
    contig->setToolTip(tr("Select the faces of \"%1\" connected to the selection").arg(g.name));
    auto *remove = new QToolButton(rowW);
    remove->setText(QStringLiteral("×"));
    remove->setAutoRaise(true);
    remove->setToolTip(tr("Take the selected faces out of \"%1\"").arg(g.name));
    h->addWidget(dot);
    h->addWidget(name, 1);
    h->addWidget(count);
    h->addWidget(sel);
    h->addWidget(contig);
    h->addWidget(remove);
    const quint32 id = g.id;
    const bool builtin = g.builtin;
    connect(sel, &QPushButton::clicked, this,
            [this, id] { m_engine->selectFaceGroup(id, QApplication::keyboardModifiers() & Qt::ShiftModifier); });
    connect(contig, &QPushButton::clicked, this, [this, id] {
      m_engine->clearAutoSelectRegion();
      m_engine->runAutoSelect(int(eng::sel::Intent::Group), 0, false, id, true, false);
    });
    connect(remove, &QToolButton::clicked, this, [this, id] { m_engine->removeSelectionFromFaceGroup(id); });
    connect(rowW, &QWidget::customContextMenuRequested, this,
            [this, id, builtin, rowW](const QPoint &p) { rowMenu(id, builtin, rowW->mapToGlobal(p)); });
    m_rowsLayout->addWidget(rowW);
  }
  m_empty->setVisible(groups.empty());

  if (!m_name->lineEdit()->hasFocus()) {
    const QString typed = m_name->currentText();
    m_name->clear();
    if (const eng::FaceGroupTable *table = m_engine->faceGroupTable())
      for (const auto &g: table->all())
        m_name->addItem(g.name, g.id);
    m_name->setEditText(typed);
  }
  m_add->setEnabled(total > 0);
}

} // namespace ui
