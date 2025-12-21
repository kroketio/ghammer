#include "reference_panel.h"

#include <QLabel>
#include <QStackedWidget>
#include <QVBoxLayout>

#include "client/ui/image_viewer.h"

namespace ui {

ReferencePanel::ReferencePanel(QWidget *parent) : QWidget(parent) {
  auto *v = new QVBoxLayout(this);
  v->setContentsMargins(0, 0, 0, 0);
  m_stack = new QStackedWidget(this);
  m_empty = new QLabel(tr("No reference image"), m_stack);
  m_empty->setAlignment(Qt::AlignCenter);
  m_empty->setEnabled(false);
  m_stack->addWidget(m_empty);
  v->addWidget(m_stack);
}

void ReferencePanel::setImage(const QString &path) {
  m_path = path;
  if (path.isEmpty()) {
    m_stack->setCurrentWidget(m_empty);
    return;
  }
  if (!m_viewer) {
    m_viewer = new ImageViewer(path, m_stack);
    m_viewer->setShowInfo(false);
    m_stack->addWidget(m_viewer);
  } else {
    m_viewer->setPath(path);
  }
  m_stack->setCurrentWidget(m_viewer);
}

} // namespace ui
