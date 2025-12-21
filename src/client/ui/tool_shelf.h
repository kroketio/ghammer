#pragma once

#include <QList>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QToolBar>

class QScrollArea;
class QVBoxLayout;
class QWidget;

namespace ui {

// The far-left tool shelf: one column of ActionManager action buttons per mode. Unregistered ids are skipped.
class ToolShelf final : public QToolBar {
  Q_OBJECT

public:
  struct Entry {
    QString id;    // action id ("csg.subtract")
    QString label; // short text under the icon; empty = the action's text
  };
  struct Section {
    QString title;
    QList<Entry> entries;
  };

  explicit ToolShelf(QWidget *parent = nullptr);

  void setSections(const QList<Section> &sections);

protected:
  bool event(QEvent *e) override;

private:
  QWidget *makeButton(const Entry &e, QWidget *parent);
  // Shelf width, button widths and icon sizes at the current UI scale.
  void applyScale();
  // Coalesced applyScale() on the next event loop pass.
  void scheduleScale();

  QScrollArea *m_scroll = nullptr;
  QPointer<QWidget> m_content;
  bool m_scalePending = false;
};

} // namespace ui
