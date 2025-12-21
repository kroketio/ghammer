#pragma once

#include <QTabBar>

namespace proj {
class DocumentManager;
}

namespace ui {

// The DocumentManager's documents as tabs; closing asks the window first (closeRequested).
class DocumentTabBar final : public QTabBar {
  Q_OBJECT

public:
  explicit DocumentTabBar(proj::DocumentManager *docs, QWidget *parent = nullptr);

signals:
  void closeRequested(int index);
  void closeOthersRequested(int index);
  void closeAllRequested();
  void revealRequested(int index); // Show in Project Panel

protected:
  void mouseReleaseEvent(QMouseEvent *event) override;
  void contextMenuEvent(QContextMenuEvent *event) override;

private:
  void rebuild();
  void refreshTexts();
  [[nodiscard]] QString textFor(int index) const;

  proj::DocumentManager *m_docs = nullptr;
  bool m_rebuilding = false;
};

} // namespace ui
