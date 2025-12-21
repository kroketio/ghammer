#pragma once

#include <QColor>
#include <QString>
#include <QtGlobal>

#include <vector>

namespace eng {

// A face's group ids into the scene's FaceGroupTable, sorted and unique.
using FaceGroupSet = std::vector<quint32>;

namespace facegroup {
constexpr quint32 kFloor = 1;
constexpr quint32 kWall = 2;
constexpr quint32 kCeiling = 3;
constexpr quint32 kVent = 4;
constexpr quint32 kStairFlat = 5;
constexpr quint32 kStairWall = 6;
constexpr quint32 kStairSide = 7;
constexpr quint32 kStairUnder = 8;
constexpr quint32 kStairStringer = 9;  // stair side brushes
constexpr quint32 kFirstUser = 16;

bool contains(const FaceGroupSet &set, quint32 id);
void insert(FaceGroupSet &set, quint32 id);
void erase(FaceGroupSet &set, quint32 id);
FaceGroupSet unite(const FaceGroupSet &a, const FaceGroupSet &b);
} // namespace facegroup

struct FaceGroup {
  quint32 id = 0;
  QString name;
  QColor color;
  bool builtin = false;
};

// Map-wide face group names. Built-ins (Floor, Wall, Ceiling, Vent, four stair parts) have fixed ids and
// cannot be renamed or removed.
class FaceGroupTable {
public:
  FaceGroupTable();

  [[nodiscard]] const std::vector<FaceGroup> &all() const { return m_groups; }
  [[nodiscard]] const FaceGroup *find(quint32 id) const;
  [[nodiscard]] const FaceGroup *findByName(const QString &name) const;
  [[nodiscard]] QString name(quint32 id) const;
  // Creates a user group when `name` has none.
  quint32 ensure(const QString &name);
  bool rename(quint32 id, const QString &name);
  bool remove(quint32 id);
  // Replaces the user groups (built-ins are kept / re-added).
  void assign(const std::vector<FaceGroup> &groups);
  void reset();
  [[nodiscard]] quint64 epoch() const { return m_epoch; }

private:
  std::vector<FaceGroup> m_groups;
  quint64 m_epoch = 1;
};

} // namespace eng
