#pragma once

#include <QObject>
#include <QPointF>
#include <QPointer>
#include <QVariantList>

class QEvent;
class QQuickWidget;

namespace ui {

class QuickMaterialModel;
class ViewportNotices;

// A Blender-style pie of ActionManager actions per mode (qml/PieMenu.qml), plus the Quick Material strip
// (qml/QuickMaterialStrip.qml). While either is open, MainWindow's key filter hands every key to handleKey.
class PieMenu final : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool pieOpen READ pieOpen WRITE setPieOpen NOTIFY pieOpenChanged)
  Q_PROPERTY(bool stripOpen READ stripOpen WRITE setStripOpen NOTIFY stripOpenChanged)

public:
  PieMenu(QQuickWidget *view, QuickMaterialModel *materials, ViewportNotices *notices, QObject *parent = nullptr);

  // At the cursor; refused while a mouse button is held.
  void popup();
  // At the pie's centre when picked in the pie, else at the cursor, else mid-view.
  void openQuickMaterial();
  void close() { emit closeRequested(); }

  // {number, id, label, icon, enabled}
  Q_INVOKABLE [[nodiscard]] QVariantList items() const;
  // The pie has closed itself first.
  Q_INVOKABLE void trigger(const QString &id);

  [[nodiscard]] bool pieOpen() const { return m_pieOpen; }
  [[nodiscard]] bool stripOpen() const { return m_stripOpen; }
  void setPieOpen(bool open);
  void setStripOpen(bool open);
  // While true, every key goes to handleKey.
  [[nodiscard]] bool capturesKeys() const { return m_pieOpen || m_stripOpen; }
  bool handleKey(QEvent *event);

signals:
  void openRequested(qreal x, qreal y, const QString &title);
  void quickMaterialRequested(qreal x, qreal y);
  void closeRequested();
  void numberPressed(int number);
  void pieOpenChanged();
  void stripOpenChanged();

private:
  [[nodiscard]] bool cursorInView(QPointF &at) const;

  QPointer<QQuickWidget> m_view;
  QPointer<QuickMaterialModel> m_materials;
  QPointer<ViewportNotices> m_notices;
  QPointF m_center;       // view coordinates
  bool m_fromPie = false; // trigger() is running a pie button
  bool m_pieOpen = false;
  bool m_stripOpen = false;
};

} // namespace ui
