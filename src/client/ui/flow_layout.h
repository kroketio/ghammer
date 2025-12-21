#pragma once

#include <QLayout>
#include <QToolButton>

namespace ui {

// Left to right, wrapping like words; height follows width.
class FlowLayout final : public QLayout {
public:
  explicit FlowLayout(QWidget *parent = nullptr, int hSpacing = 4, int vSpacing = 4);
  ~FlowLayout() override;

  void addItem(QLayoutItem *item) override;
  [[nodiscard]] int count() const override;
  [[nodiscard]] QLayoutItem *itemAt(int index) const override;
  QLayoutItem *takeAt(int index) override;
  [[nodiscard]] Qt::Orientations expandingDirections() const override { return {}; }
  [[nodiscard]] bool hasHeightForWidth() const override { return true; }
  [[nodiscard]] int heightForWidth(int width) const override;
  [[nodiscard]] QSize minimumSize() const override;
  [[nodiscard]] QSize sizeHint() const override { return minimumSize(); }
  void setGeometry(const QRect &rect) override;

  // Deletes every item and widget.
  void clear();

private:
  int doLayout(const QRect &rect, bool testOnly) const;
  QList<QLayoutItem *> m_items;
  int m_h;
  int m_v;
};

// A checkable tag pill ("rock  1,316"); right click requests exclude.
class TagChip final : public QToolButton {
  Q_OBJECT

public:
  enum class State { Off, Include, Exclude };

  TagChip(const QString &tag, int count, QWidget *parent = nullptr);

  [[nodiscard]] QString tag() const { return m_tag; }
  void setCount(int count);
  void setState(State s);
  [[nodiscard]] State state() const { return m_state; }
  // Shows an active filter with a ×.
  void setRemovable(bool on);
  [[nodiscard]] QSize sizeHint() const override;

signals:
  void toggledInclude(const QString &tag, bool on);
  void excludeRequested(const QString &tag);
  void removeRequested(const QString &tag);

protected:
  void mousePressEvent(QMouseEvent *e) override;
  void paintEvent(QPaintEvent *e) override;

private:
  QString label() const;
  QString m_tag;
  int m_count = -1;
  State m_state = State::Off;
  bool m_removable = false;
};

} // namespace ui
