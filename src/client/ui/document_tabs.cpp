#include "document_tabs.h"

#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QMenu>
#include <QMouseEvent>
#include <QUrl>

#include "client/project/document_manager.h"
#include "client/ui/action_manager.h"
#include "client/ui/file_icons.h"
#include "client/ui/scale.h"

namespace ui {

DocumentTabBar::DocumentTabBar(proj::DocumentManager *docs, QWidget *parent) : QTabBar(parent), m_docs(docs) {
  setObjectName(QStringLiteral("documentTabs"));
  setTabsClosable(true);
  setMovable(true);
  setExpanding(false);
  setDocumentMode(true);
  setUsesScrollButtons(true);
  setElideMode(Qt::ElideMiddle);
  setDrawBase(false);
  setFocusPolicy(Qt::NoFocus);
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  scale::bind(this, [this] { setIconSize(scale::icon(scale::Icon::Small)); });

  connect(m_docs, &proj::DocumentManager::documentsChanged, this, &DocumentTabBar::rebuild);
  connect(m_docs, &proj::DocumentManager::activeChanged, this, [this] {
    const QSignalBlocker block(this);
    setCurrentIndex(m_docs->activeIndex());
  });
  connect(m_docs, &proj::DocumentManager::dirtyChanged, this, &DocumentTabBar::refreshTexts);
  connect(this, &QTabBar::currentChanged, this, [this](int index) {
    if (m_rebuilding || index < 0 || index == m_docs->activeIndex())
      return;
    if (!m_docs->activate(index)) {
      const QSignalBlocker block(this);
      setCurrentIndex(m_docs->activeIndex()); // the engine refused the switch
    }
  });
  connect(this, &QTabBar::tabCloseRequested, this, &DocumentTabBar::closeRequested);
  connect(this, &QTabBar::tabMoved, this, [this](int from, int to) {
    if (m_rebuilding)
      return;
    m_docs->move(from, to);
  });
  rebuild();
}

QString DocumentTabBar::textFor(const int index) const {
  const proj::Document *d = m_docs->at(index);
  if (!d)
    return {};
  return d->fileName() + (m_docs->dirty(d) ? QStringLiteral(" *") : QString());
}

void DocumentTabBar::rebuild() {
  m_rebuilding = true;
  const QSignalBlocker block(this);
  while (count() > m_docs->count())
    removeTab(count() - 1);
  while (count() < m_docs->count())
    addTab(QString());
  for (int i = 0; i < m_docs->count(); ++i) {
    const proj::Document *d = m_docs->at(i);
    setTabText(i, textFor(i));
    setTabIcon(i, d->isLevel() ? ActionManager::icon(QStringLiteral("file_3d")) : iconForFile(d->path));
    setTabToolTip(i, d->path.isEmpty() ? tr("Not saved yet") : QDir::toNativeSeparators(d->path));
  }
  setCurrentIndex(m_docs->activeIndex());
  m_rebuilding = false;
}

void DocumentTabBar::refreshTexts() {
  for (int i = 0; i < count() && i < m_docs->count(); ++i)
    setTabText(i, textFor(i));
}

void DocumentTabBar::mouseReleaseEvent(QMouseEvent *event) {
  if (event->button() == Qt::MiddleButton) {
    if (const int i = tabAt(event->position().toPoint()); i >= 0) {
      emit closeRequested(i);
      return;
    }
  }
  QTabBar::mouseReleaseEvent(event);
}

void DocumentTabBar::contextMenuEvent(QContextMenuEvent *event) {
  const int i = tabAt(event->pos());
  QMenu menu(this);
  if (i >= 0) {
    menu.addAction(tr("Close"), this, [this, i] { emit closeRequested(i); });
    menu.addAction(tr("Close Others"), this, [this, i] { emit closeOthersRequested(i); });
  }
  menu.addAction(tr("Close All"), this, [this] { emit closeAllRequested(); });
  if (const proj::Document *d = m_docs->at(i); d && !d->path.isEmpty()) {
    const QString path = d->path;
    menu.addSeparator();
    menu.addAction(ActionManager::icon(QStringLiteral("file_folder")), tr("Show in Project Panel"), this,
                   [this, i] { emit revealRequested(i); });
    menu.addAction(tr("Show in File Manager"), this, [path] {
      QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
    });
    menu.addAction(tr("Copy Path"), this,
                   [path] { QApplication::clipboard()->setText(QDir::toNativeSeparators(path)); });
  }
  menu.exec(event->globalPos());
}

} // namespace ui
