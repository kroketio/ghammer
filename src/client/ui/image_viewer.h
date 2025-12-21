#pragma once

#include <QImage>
#include <QPointF>
#include <QWidget>

#include <functional>

class QFileSystemWatcher;

namespace ui {

// An image tab. .exr files are tone-mapped; decoded off the GUI thread and reloaded when the file changes.
class ImageViewer final : public QWidget {
  Q_OBJECT

public:
  explicit ImageViewer(const QString &path, QWidget *parent = nullptr);
  [[nodiscard]] QString path() const { return m_path; }
  void setPath(const QString &path);
  void fit();
  void actualSize();
  // Set: the context menu offers "As Reference Image".
  std::function<void(const QString &path)> asReference;
  // The size · format · zoom line (off: only read errors are shown).
  void setShowInfo(bool on) {
    m_showInfo = on;
    update();
  }

protected:
  void paintEvent(QPaintEvent *event) override;
  void resizeEvent(QResizeEvent *event) override;
  void wheelEvent(QWheelEvent *event) override;
  void mousePressEvent(QMouseEvent *event) override;
  void mouseMoveEvent(QMouseEvent *event) override;
  void mouseReleaseEvent(QMouseEvent *event) override;
  void mouseDoubleClickEvent(QMouseEvent *event) override;
  void keyPressEvent(QKeyEvent *event) override;
  void contextMenuEvent(QContextMenuEvent *event) override;

private:
  void load();
  void zoomAt(double factor, const QPointF &at);
  [[nodiscard]] QString infoText() const;

  QString m_path;
  QImage m_image;
  QString m_format;
  QString m_error;
  bool m_loading = false;
  quint64 m_loadGen = 0;
  double m_zoom = 1.0;
  QPointF m_offset; // image top-left in widget pixels
  bool m_fitted = true;
  bool m_showInfo = true;
  bool m_panning = false;
  QPointF m_panFrom;
  QFileSystemWatcher *m_watcher = nullptr;
};

} // namespace ui
