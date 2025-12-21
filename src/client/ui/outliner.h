#pragma once

#include <QAbstractItemModel>
#include <QHash>
#include <QPointer>
#include <QSet>
#include <QSortFilterProxyModel>
#include <QWidget>

#include <vector>

class QTreeView;
class QLineEdit;
class QLabel;
class QTimer;

namespace vk {
class Engine3D;
}

namespace ui {

// The scene tree, one collapsible section per kind. Brushes are never listed one by one (a map has
// thousands): a group / generated / mirror row stands for its brushes.
class SceneTreeModel final : public QAbstractItemModel {
  Q_OBJECT

public:
  enum Column { ColName = 0, ColVisible, ColLocked, ColumnCount };
  enum Kind : quint32 { KindGroup = 1, KindHeader = 2, KindEntity = 3, KindProc = 4, KindMirror = 5, KindPath = 6,
                       KindHost = 7 };
  enum Section : quint32 { SectionEntities = 0, SectionGenerated = 1, SectionGroups = 2, SectionMirrors = 3,
                           SectionPaths = 4, SectionModified = 5 };
  enum Roles { KindRole = Qt::UserRole + 1, IdRole };

  explicit SceneTreeModel(vk::Engine3D *engine, QObject *parent = nullptr);

  void rebuild();
  // Repaints names/counts/flags without changing the shape.
  void refreshRows();

  [[nodiscard]] QModelIndex indexForEntity(quint32 entityId) const;
  [[nodiscard]] int brushCount() const { return m_totalBrushes; }
  [[nodiscard]] int groupCount() const { return int(m_groups.size()); }
  [[nodiscard]] const std::vector<quint32> &mirrorMembers(quint32 set) const;

  QModelIndex index(int row, int column, const QModelIndex &parent = {}) const override;
  QModelIndex parent(const QModelIndex &child) const override;
  int rowCount(const QModelIndex &parent = {}) const override;
  int columnCount(const QModelIndex &parent = {}) const override { Q_UNUSED(parent); return ColumnCount; }
  QVariant data(const QModelIndex &index, int role) const override;
  Qt::ItemFlags flags(const QModelIndex &index) const override;
  QVariant headerData(int section, Qt::Orientation o, int role) const override;

private:
  struct Row {
    Kind kind;
    quint32 id;
  };
  struct SectionRows {
    Section section;
    std::vector<Row> rows;
  };
  // Internal id 0 = a header, else its section's index + 1.
  [[nodiscard]] Row rowAt(const QModelIndex &idx) const;
  void recount();

  vk::Engine3D *m_engine;
  std::vector<SectionRows> m_sections;
  std::vector<quint32> m_groups;
  QHash<quint32, int> m_counts;
  QHash<quint32, int> m_selCounts;
  QHash<quint32, std::vector<quint32>> m_mirrorMembers;
  // Names: a modified object's mirror collections, a mirror collection's modified objects.
  QHash<quint32, QStringList> m_hostMirrors, m_mirrorHosts;
  int m_totalBrushes = 0;
};

class Outliner final : public QWidget {
  Q_OBJECT

public:
  explicit Outliner(vk::Engine3D *engine, QWidget *parent = nullptr);

private:
  void pullSelectionFromEngine();
  void showContextMenu(const QPoint &pos);
  void activateIndex(const QModelIndex &idx);
  // Toggle removes the row's brushes when all are selected.
  enum class Pick { Replace, Toggle, Add };
  void pickRow(const QModelIndex &sourceIdx, Pick mode);
  // The visible, unlocked brushes a group / generated / mirror row stands for.
  [[nodiscard]] std::vector<quint32> rowBrushes(int kind, quint32 id) const;
  void renameGroup(quint32 id);
  void updateSummary();
  // Expands every section except the collapsed ones (QSettings "Outliner/Collapsed").
  void restoreExpansion();

  vk::Engine3D *m_engine;
  SceneTreeModel *m_model;
  QSortFilterProxyModel *m_proxy;
  QTreeView *m_view;
  QLineEdit *m_search;
  QLabel *m_summary;
  QTimer *m_rebuildTimer;
  bool m_syncing = false;
  QPersistentModelIndex m_anchor; // last row clicked (proxy, column 0): Shift ranges start here
  bool m_restoring = false;
  QSet<int> m_collapsed; // SceneTreeModel::Section values
};

} // namespace ui
