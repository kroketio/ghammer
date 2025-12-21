#include "history_panel.h"

#include <QListWidget>
#include <QTimer>
#include <QVBoxLayout>

#include "client/engine/undo_stack.h"
#include "client/ui/action_manager.h"
#include "client/vulkan/vk_engine.h"

namespace ui {

HistoryPanel::HistoryPanel(vk::Engine3D *engine, QWidget *parent) : QWidget(parent), m_engine(engine) {
  auto *v = new QVBoxLayout(this);
  v->setContentsMargins(4, 4, 4, 4);
  m_list = new QListWidget(this);
  m_list->setUniformItemSizes(true);
  v->addWidget(m_list);

  auto *timer = new QTimer(this);
  timer->setSingleShot(true);
  timer->setInterval(50);
  connect(timer, &QTimer::timeout, this, &HistoryPanel::rebuild);
  connect(m_engine, &vk::Engine3D::undoRedoChanged, timer, qOverload<>(&QTimer::start));
  connect(m_engine, &vk::Engine3D::sceneChanged, timer, qOverload<>(&QTimer::start));

  connect(m_list, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
    auto *stack = m_engine->undoStack();
    const int target = item->data(Qt::UserRole).toInt(); // desired undoDepth
    m_jumping = true;
    while (stack->undoDepth() > target && m_engine->undo()) {}
    while (stack->undoDepth() < target && m_engine->redo()) {}
    m_jumping = false;
    rebuild();
  });
  rebuild();
}

void HistoryPanel::rebuild() {
  if (m_jumping)
    return;
  auto *stack = m_engine->undoStack();
  m_list->clear();
  auto *origin = new QListWidgetItem(ActionManager::icon(QStringLiteral("file_new")), tr("Opened map"));
  origin->setData(Qt::UserRole, 0);
  m_list->addItem(origin);
  const int depth = stack->undoDepth();
  for (int i = 0; i < stack->stepCount(); ++i) {
    auto *item = new QListWidgetItem(stack->textAt(i));
    item->setData(Qt::UserRole, i + 1);
    if (i >= depth)
      item->setForeground(QColor(0x6a, 0x74, 0x90));
    m_list->addItem(item);
  }
  if (QListWidgetItem *cur = m_list->item(depth)) {
    QFont f = cur->font();
    f.setBold(true);
    cur->setFont(f);
    m_list->setCurrentItem(cur);
    m_list->scrollToItem(cur);
  }
}

} // namespace ui
