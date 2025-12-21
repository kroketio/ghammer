#pragma once

#include <QAbstractListModel>
#include <QPointer>
#include <QVector>

#include "client/ui/texture_ui.h"

namespace vk {
class Engine3D;
}

namespace ui {

// The Quick Material strip: the nearest faces' materials (kNearby), then recent ones (kRecent); never one
// already on every selected face. apply(row) is one undo step.
class QuickMaterialModel final : public QAbstractListModel {
  Q_OBJECT

public:
  static constexpr int kNearby = 8;
  static constexpr int kRecent = 4;
  // FirstRecentRole: the strip puts a divider before it.
  enum Role { RefRole = Qt::UserRole + 1, NameRole, ThumbRole, RecentRole, FirstRecentRole };

  explicit QuickMaterialModel(vk::Engine3D *engine, QObject *parent = nullptr);

  [[nodiscard]] int rowCount(const QModelIndex &parent = {}) const override;
  [[nodiscard]] QVariant data(const QModelIndex &idx, int role) const override;
  [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

  void refresh();
  Q_INVOKABLE void apply(int row);

private:
  struct Row {
    QString ref;
    QString name;
    ThumbSource thumb;
    int rev = 0; // bumped when the thumbnail arrives, so QML reloads it
    bool recent = false;
  };
  void touch(int row);

  QPointer<vk::Engine3D> m_engine;
  QVector<Row> m_rows;
};

} // namespace ui
