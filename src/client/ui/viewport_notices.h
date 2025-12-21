#pragma once

#include <QAbstractListModel>
#include <QVector>

#include <functional>

class QTimer;

namespace ui {

// Every viewport message (3D view, lower left): lasting notices, then the one-off flash() row.
// A source is a key + text function, empty = off. refreshOn() names the signals that can change them;
// refreshes are coalesced per event loop pass. All are listed in MainWindow::setupViewportNotices.
class ViewportNotices final : public QAbstractListModel {
  Q_OBJECT

public:
  enum Role { KeyRole = Qt::UserRole + 1, TextRole };

  explicit ViewportNotices(QObject *parent = nullptr);

  using Source = std::function<QString()>;
  void addSource(const QString &key, Source text);
  template <class Sender, class Signal> void refreshOn(Sender *sender, Signal signal) {
    connect(sender, signal, this, &ViewportNotices::scheduleRefresh);
  }
  void scheduleRefresh();
  void refresh();
  // Replaces the last one; gone after `ms`.
  void flash(const QString &text, int ms = 2200);

  [[nodiscard]] int rowCount(const QModelIndex &parent = {}) const override;
  [[nodiscard]] QVariant data(const QModelIndex &index, int role) const override;
  [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

private:
  struct Notice {
    QString key;
    QString text;
  };
  QVector<Notice> m_notices; // shown
  std::vector<std::pair<QString, Source>> m_sources;
  bool m_pending = false;
  QTimer *m_flashTimer = nullptr;

  void set(const QString &key, const QString &text);
};

} // namespace ui
