#include "face_groups.h"

#include <algorithm>

namespace eng {

namespace facegroup {

bool contains(const FaceGroupSet &set, quint32 id) { return std::binary_search(set.begin(), set.end(), id); }

void insert(FaceGroupSet &set, quint32 id) {
  const auto it = std::lower_bound(set.begin(), set.end(), id);
  if (it == set.end() || *it != id)
    set.insert(it, id);
}

void erase(FaceGroupSet &set, quint32 id) {
  const auto it = std::lower_bound(set.begin(), set.end(), id);
  if (it != set.end() && *it == id)
    set.erase(it);
}

FaceGroupSet unite(const FaceGroupSet &a, const FaceGroupSet &b) {
  FaceGroupSet out;
  out.reserve(a.size() + b.size());
  std::set_union(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(out));
  return out;
}

} // namespace facegroup

namespace {
std::vector<FaceGroup> builtins() {
  return {
    {facegroup::kFloor, QStringLiteral("Floor"), QColor(214, 170, 92), true},
    {facegroup::kWall, QStringLiteral("Wall"), QColor(120, 170, 230), true},
    {facegroup::kCeiling, QStringLiteral("Ceiling"), QColor(170, 140, 220), true},
    {facegroup::kVent, QStringLiteral("Vent"), QColor(230, 140, 80), true},
    {facegroup::kStairFlat, QStringLiteral("Stair step flat"), QColor(120, 200, 140), true},
    {facegroup::kStairWall, QStringLiteral("Stair step wall"), QColor(90, 170, 170), true},
    {facegroup::kStairSide, QStringLiteral("Stair step side"), QColor(160, 190, 110), true},
    {facegroup::kStairUnder, QStringLiteral("Stair underside"), QColor(200, 160, 120), true},
    {facegroup::kStairStringer, QStringLiteral("Stair side brush"), QColor(180, 130, 170), true},
  };
}

QColor colorFor(quint32 id) {
  return QColor::fromHsv(int((id * 67u) % 360u), 150, 220);
}
} // namespace

FaceGroupTable::FaceGroupTable() { reset(); }

void FaceGroupTable::reset() {
  m_groups = builtins();
  ++m_epoch;
}

const FaceGroup *FaceGroupTable::find(quint32 id) const {
  for (const auto &g: m_groups)
    if (g.id == id)
      return &g;
  return nullptr;
}

const FaceGroup *FaceGroupTable::findByName(const QString &name) const {
  const QString n = name.trimmed();
  for (const auto &g: m_groups)
    if (g.name.compare(n, Qt::CaseInsensitive) == 0)
      return &g;
  return nullptr;
}

QString FaceGroupTable::name(quint32 id) const {
  const FaceGroup *g = find(id);
  return g ? g->name : QStringLiteral("#%1").arg(id);
}

quint32 FaceGroupTable::ensure(const QString &name) {
  const QString n = name.trimmed();
  if (n.isEmpty())
    return 0;
  if (const FaceGroup *g = findByName(n))
    return g->id;
  quint32 next = facegroup::kFirstUser;
  for (const auto &g: m_groups)
    next = std::max(next, g.id + 1);
  m_groups.push_back({next, n, colorFor(next), false});
  ++m_epoch;
  return next;
}

bool FaceGroupTable::rename(quint32 id, const QString &name) {
  const QString n = name.trimmed();
  if (n.isEmpty())
    return false;
  if (const FaceGroup *other = findByName(n); other && other->id != id)
    return false;
  for (auto &g: m_groups)
    if (g.id == id && !g.builtin) {
      g.name = n;
      ++m_epoch;
      return true;
    }
  return false;
}

bool FaceGroupTable::remove(quint32 id) {
  const auto it = std::find_if(m_groups.begin(), m_groups.end(),
                               [id](const FaceGroup &g) { return g.id == id && !g.builtin; });
  if (it == m_groups.end())
    return false;
  m_groups.erase(it);
  ++m_epoch;
  return true;
}

void FaceGroupTable::assign(const std::vector<FaceGroup> &groups) {
  m_groups = builtins();
  for (const auto &g: groups) {
    if (g.id < facegroup::kFirstUser || g.name.trimmed().isEmpty() || find(g.id))
      continue;
    FaceGroup copy = g;
    copy.builtin = false;
    if (!copy.color.isValid())
      copy.color = colorFor(copy.id);
    m_groups.push_back(copy);
  }
  ++m_epoch;
}

} // namespace eng
