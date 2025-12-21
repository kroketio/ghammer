#include "layout_gen.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <map>

#include <QJsonArray>
#include <QSet>

namespace eng::layout {

namespace {

using Iv = std::vector<std::pair<int, int>>;

Iv normalized(Iv v) {
  std::sort(v.begin(), v.end());
  Iv out;
  for (const auto &p: v) {
    if (p.second <= p.first)
      continue;
    if (!out.empty() && p.first <= out.back().second)
      out.back().second = std::max(out.back().second, p.second);
    else
      out.push_back(p);
  }
  return out;
}

Iv subtract(const Iv &a, const Iv &b) {
  Iv out;
  for (auto [s, e]: a) {
    int cur = s;
    for (const auto &[bs, be]: b) {
      if (be <= cur || bs >= e)
        continue;
      if (bs > cur)
        out.emplace_back(cur, bs);
      cur = std::max(cur, be);
      if (cur >= e)
        break;
    }
    if (cur < e)
      out.emplace_back(cur, e);
  }
  return out;
}

// One axis-aligned piece of a shape: [x0, x1) x [y0, y1) in Source units.
struct Prim {
  int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  Kind kind = Corridor;
  int shape = 0, seg = -1;
  Segment s;
  bool diagonal = false;
  glm::dvec2 a {0.0}, dir {1.0, 0.0};
  int contact = ContactOpen;
  int roomHeight = 0, roomElevation = 0;
};

constexpr int kDiagonalStep = 8;

void pathPrims(const Path &path, std::vector<Prim> &out) {
  for (size_t i = 0; i + 1 < path.nodes.size() && i < path.segs.size(); ++i) {
    const Segment &s = path.segs[i];
    const QPoint na = path.nodes[i], nb = path.nodes[i + 1];
    Prim p;
    p.kind = path.kind;
    p.shape = path.id;
    p.seg = int(i);
    p.s = s;
    p.a = glm::dvec2(na.x(), na.y());
    const glm::dvec2 b(nb.x(), nb.y());
    const double len = glm::length(b - p.a);
    p.dir = len > 1e-9 ? (b - p.a) / len : glm::dvec2(1.0, 0.0);
    const int w = std::max(1, s.width);
    const int lo = w / 2, hi = w - w / 2;
    if (na.x() == nb.x() || na.y() == nb.y()) {
      p.x0 = std::min(na.x(), nb.x()) - lo;
      p.x1 = std::max(na.x(), nb.x()) + hi;
      p.y0 = std::min(na.y(), nb.y()) - lo;
      p.y1 = std::max(na.y(), nb.y()) + hi;
      out.push_back(p);
      continue;
    }
    // Diagonal: rows of a capsule around the centre-line.
    p.diagonal = true;
    const int r = kDiagonalStep;
    const double half = std::max(r, w) * 0.5;
    const glm::dvec2 mn = glm::min(p.a, b) - half, mx = glm::max(p.a, b) + half;
    const int ix0 = int(std::floor(mn.x / r)), ix1 = int(std::ceil(mx.x / r));
    const int iy0 = int(std::floor(mn.y / r)), iy1 = int(std::ceil(mx.y / r));
    for (int y = iy0; y < iy1; ++y) {
      int first = INT_MAX, last = INT_MIN;
      for (int x = ix0; x < ix1; ++x) {
        const glm::dvec2 c((x + 0.5) * r, (y + 0.5) * r);
        const double t = std::clamp(glm::dot(c - p.a, p.dir), 0.0, len);
        if (glm::length(c - (p.a + p.dir * t)) <= half) {
          first = std::min(first, x);
          last = std::max(last, x);
        }
      }
      if (first > last)
        continue;
      Prim row = p;
      row.x0 = first * r;
      row.x1 = (last + 1) * r;
      row.y0 = y * r;
      row.y1 = (y + 1) * r;
      out.push_back(row);
    }
  }
}

// A shared-wall room side short of a corridor wall is extended to it, leaving no void sliver.
void attachBridges(const Prim &room, const std::vector<Prim> &corridors, int wall, std::vector<Prim> &out) {
  for (const Prim &c: corridors) {
    if (c.diagonal)
      continue;
    const int dx0 = c.x0 - wall, dx1 = c.x1 + wall, dy0 = c.y0 - wall, dy1 = c.y1 + wall;
    const int ox0 = std::max(room.x0, dx0), ox1 = std::min(room.x1, dx1);
    const int oy0 = std::max(room.y0, dy0), oy1 = std::min(room.y1, dy1);
    auto add = [&](int x0, int y0, int x1, int y1) {
      if (x1 <= x0 || y1 <= y0)
        return;
      Prim b = room;
      b.x0 = x0;
      b.y0 = y0;
      b.x1 = x1;
      b.y1 = y1;
      out.push_back(b);
    };
    if (ox1 > ox0) {
      if (dy1 < room.y0 && room.y0 - dy1 < kAttachDistance)
        add(ox0, dy1, ox1, room.y0);
      if (dy0 > room.y1 && dy0 - room.y1 < kAttachDistance)
        add(ox0, room.y1, ox1, dy0);
    }
    if (oy1 > oy0) {
      if (dx1 < room.x0 && room.x0 - dx1 < kAttachDistance)
        add(dx1, oy0, room.x0, oy1);
      if (dx0 > room.x1 && dx0 - room.x1 < kAttachDistance)
        add(room.x1, oy0, dx0, oy1);
    }
  }
}

// Index range of the cells inside [c0, c1] (conservative when c0 / c1 are not grid lines).
std::pair<int, int> cellRange(const std::vector<int> &lines, int c0, int c1) {
  const int lo = int(std::lower_bound(lines.begin(), lines.end(), c0) - lines.begin());
  const int hi = int(std::upper_bound(lines.begin(), lines.end(), c1) - lines.begin()) - 1;
  return {lo, std::max(lo, hi)};
}

template <typename F>
void forCells(const Raster &ras, int x0, int y0, int x1, int y1, F &&f) {
  const auto [i0, i1] = cellRange(ras.xs, x0, x1);
  const auto [j0, j1] = cellRange(ras.ys, y0, y1);
  for (int j = j0; j < j1; ++j)
    for (int i = i0; i < i1; ++i)
      f(i, j, cellKey(i, j));
}

struct Columns {
  std::vector<int> xs, ys;
  QHash<quint64, Iv> solidS, solidV;
  QHash<quint64, Iv> openS, openV;
  // Floor / ceiling levels of the neighbouring spaces: solids are cut there so walls stop at them and slabs run
  // under and over.
  QHash<quint64, std::vector<int>> cuts;
};

Columns computeColumns(const Doc &doc) {
  Columns out;
  const Raster ras = rasterize(doc);
  const Config &cfg = doc.config;
  out.xs = ras.xs;
  out.ys = ras.ys;
  const int wall = std::max(1, cfg.wall);
  const int ventWall = std::max(1, cfg.ventWall);

  QHash<quint64, Iv> open;
  for (auto it = ras.cells.constBegin(); it != ras.cells.constEnd(); ++it) {
    const CellInfo &c = it.value();
    Iv o;
    Iv os, ov;
    if (c.kind >= 0) {
      int top = c.elevation + c.height;
      bool sealed = c.sealed;
      if (c.kind == Corridor && c.doorZone) {
        if (c.lateral * 2 > cfg.doorWidth)
          sealed = true;
        else
          top = c.elevation + std::min(c.height, cfg.doorHeight);
      }
      if (!sealed) {
        o.emplace_back(c.elevation, top);
        os.emplace_back(c.elevation, top);
      }
    }
    if (c.vent) {
      o.emplace_back(c.ventElevation, c.ventElevation + c.ventHeight);
      ov.emplace_back(c.ventElevation, c.ventElevation + c.ventHeight);
    }
    if (!o.empty())
      open[it.key()] = normalized(o);
    if (!os.empty())
      out.openS[it.key()] = normalized(os);
    if (!ov.empty())
      out.openV[it.key()] = subtract(normalized(ov), out.openS.value(it.key()));
  }

  // Envelope: every space cell stamps floor-to-ceiling onto itself; edge cells also a wall thickness around.
  QHash<quint64, Iv> envS, envV;
  auto stampAround = [&](QHash<quint64, Iv> &env, int i, int j, int d, const std::pair<int, int> &e,
                         const std::pair<int, int> &levels) {
    forCells(ras, ras.xs[size_t(i)] - d, ras.ys[size_t(j)] - d, ras.xs[size_t(i) + 1] + d, ras.ys[size_t(j) + 1] + d,
             [&](int, int, quint64 k) {
               env[k].push_back(e);
               auto &c = out.cuts[k];
               c.push_back(levels.first);
               c.push_back(levels.second);
             });
  };
  for (auto it = ras.cells.constBegin(); it != ras.cells.constEnd(); ++it) {
    const QPoint p = keyCell(it.key());
    const CellInfo &c = it.value();
    if (c.kind >= 0) {
      const std::pair<int, int> e(c.elevation - cfg.floor, c.elevation + c.height + cfg.ceiling);
      envS[it.key()].push_back(e);
      bool edge = false;
      for (int dy = -1; dy <= 1 && !edge; ++dy)
        for (int dx = -1; dx <= 1 && !edge; ++dx) {
          const auto n = ras.cells.constFind(cellKey(p.x() + dx, p.y() + dy));
          edge = n == ras.cells.constEnd() || n->kind < 0 || n->elevation != c.elevation || n->height != c.height;
        }
      if (edge)
        stampAround(envS, p.x(), p.y(), wall, e, {c.elevation, c.elevation + c.height});
    }
    if (c.vent) {
      const std::pair<int, int> e(c.ventElevation - cfg.ventWall, c.ventElevation + c.ventHeight + cfg.ventWall);
      envV[it.key()].push_back(e);
      bool edge = false;
      for (int dy = -1; dy <= 1 && !edge; ++dy)
        for (int dx = -1; dx <= 1 && !edge; ++dx) {
          const auto n = ras.cells.constFind(cellKey(p.x() + dx, p.y() + dy));
          edge = n == ras.cells.constEnd() || !n->vent || n->ventElevation != c.ventElevation
                 || n->ventHeight != c.ventHeight;
        }
      if (edge)
        stampAround(envV, p.x(), p.y(), ventWall, e, {c.ventElevation, c.ventElevation + c.ventHeight});
    }
  }
  for (auto it = envS.constBegin(); it != envS.constEnd(); ++it) {
    const Iv s = subtract(normalized(it.value()), open.value(it.key()));
    if (!s.empty())
      out.solidS[it.key()] = s;
  }
  for (auto it = envV.constBegin(); it != envV.constEnd(); ++it) {
    Iv s = subtract(normalized(it.value()), open.value(it.key()));
    s = subtract(s, out.solidS.value(it.key()));
    if (!s.empty())
      out.solidV[it.key()] = s;
  }
  return out;
}

// Greedy rectangle cover of grid cells: rows first, then grown downwards.
void mergeInto(const std::vector<QPoint> &cells, const std::pair<int, int> &z, const std::vector<int> &xs,
               const std::vector<int> &ys, bool vent, std::vector<Box> &out) {
  QSet<quint64> left;
  for (const QPoint &p: cells)
    left.insert(cellKey(p.x(), p.y()));
  std::vector<QPoint> order = cells;
  std::sort(order.begin(), order.end(), [](const QPoint &a, const QPoint &b) {
    return a.y() != b.y() ? a.y() < b.y() : a.x() < b.x();
  });
  for (const QPoint &p: order) {
    if (!left.contains(cellKey(p.x(), p.y())))
      continue;
    int x1 = p.x();
    while (left.contains(cellKey(x1 + 1, p.y())))
      ++x1;
    int y1 = p.y();
    for (;;) {
      bool full = true;
      for (int x = p.x(); x <= x1 && full; ++x)
        full = left.contains(cellKey(x, y1 + 1));
      if (!full)
        break;
      ++y1;
    }
    for (int y = p.y(); y <= y1; ++y)
      for (int x = p.x(); x <= x1; ++x)
        left.remove(cellKey(x, y));
    Box b;
    b.mn = {xs[size_t(p.x())], ys[size_t(p.y())], z.first};
    b.mx = {xs[size_t(x1) + 1], ys[size_t(y1) + 1], z.second};
    b.vent = vent;
    out.push_back(b);
  }
}

} // namespace

QRect Raster::cellRect(const quint64 key) const {
  const QPoint p = keyCell(key);
  if (p.x() < 0 || p.y() < 0 || p.x() + 1 >= int(xs.size()) || p.y() + 1 >= int(ys.size()))
    return {};
  const int x0 = xs[size_t(p.x())], y0 = ys[size_t(p.y())];
  return {x0, y0, xs[size_t(p.x()) + 1] - x0, ys[size_t(p.y()) + 1] - y0};
}

int segmentLength(const Path &path, const int seg) {
  if (seg < 0 || seg + 1 >= int(path.nodes.size()) || seg >= int(path.segs.size()))
    return 0;
  const QPoint d = path.nodes[size_t(seg) + 1] - path.nodes[size_t(seg)];
  return int(std::lround(std::hypot(double(d.x()), double(d.y())))) + path.segs[size_t(seg)].width;
}

Raster rasterize(const Doc &doc) {
  Raster ras;
  const Config &cfg = doc.config;
  const int wall = std::max(1, cfg.wall);
  const int ventWall = std::max(1, cfg.ventWall);

  std::vector<Prim> corridors, vents, rooms, sharedRooms;
  for (const Path &path: doc.paths)
    pathPrims(path, path.kind == Vent ? vents : corridors);
  bool anyDoorway = false;
  for (const RoomShape &room: doc.rooms)
    for (const QRect &rc: room.rects) {
      if (rc.width() <= 0 || rc.height() <= 0)
        continue;
      Prim p;
      p.kind = Room;
      p.shape = room.id;
      p.x0 = rc.x();
      p.y0 = rc.y();
      p.x1 = rc.x() + rc.width();
      p.y1 = rc.y() + rc.height();
      p.contact = room.contact;
      p.roomHeight = room.height;
      p.roomElevation = room.elevation;
      if (room.contact == ContactSharedWall) {
        sharedRooms.push_back(p);
      } else {
        anyDoorway = anyDoorway || room.contact == ContactDoorway;
        rooms.push_back(p);
      }
    }
  const size_t sharedCount = sharedRooms.size();
  for (size_t i = 0; i < sharedCount; ++i)
    attachBridges(Prim(sharedRooms[i]), corridors, wall, sharedRooms);

  // Grid lines: every edge and its wall-thickness offsets, so walls land exactly on lines.
  std::vector<int> &xs = ras.xs, &ys = ras.ys;
  auto addEdges = [&](const Prim &p, std::initializer_list<int> offsets) {
    for (const int o: offsets) {
      xs.push_back(p.x0 + o);
      xs.push_back(p.x1 + o);
      ys.push_back(p.y0 + o);
      ys.push_back(p.y1 + o);
    }
  };
  for (const auto *set: {&corridors, &rooms, &sharedRooms})
    for (const Prim &p: *set)
      addEdges(p, {0, wall, -wall, 2 * wall, -2 * wall});
  for (const Prim &p: vents)
    addEdges(p, {0, ventWall, -ventWall});
  if (anyDoorway) {
    const int dlo = cfg.doorWidth / 2, dhi = cfg.doorWidth - cfg.doorWidth / 2;
    for (const Prim &p: corridors) {
      if (p.diagonal)
        continue;
      if (std::fabs(p.dir.y) < 1e-9) {
        ys.push_back(int(std::lround(p.a.y)) - dlo);
        ys.push_back(int(std::lround(p.a.y)) + dhi);
      } else {
        xs.push_back(int(std::lround(p.a.x)) - dlo);
        xs.push_back(int(std::lround(p.a.x)) + dhi);
      }
    }
  }
  for (const QRect &s: doc.sealed) {
    xs.push_back(s.x());
    xs.push_back(s.x() + s.width());
    ys.push_back(s.y());
    ys.push_back(s.y() + s.height());
  }
  for (auto *v: {&xs, &ys}) {
    std::sort(v->begin(), v->end());
    v->erase(std::unique(v->begin(), v->end()), v->end());
  }
  if (xs.size() < 2 || ys.size() < 2)
    return ras;

  auto centre = [&ras](int i, int j) {
    return glm::dvec2((ras.xs[size_t(i)] + ras.xs[size_t(i) + 1]) * 0.5, (ras.ys[size_t(j)] + ras.ys[size_t(j) + 1]) * 0.5);
  };

  QSet<quint64> nearCorridor, nearDoorway;
  for (const Prim &p: corridors)
    forCells(ras, p.x0 - wall, p.y0 - wall, p.x1 + wall, p.y1 + wall,
             [&](int, int, quint64 k) { nearCorridor.insert(k); });
  for (const Prim &p: rooms)
    if (p.contact == ContactDoorway)
      forCells(ras, p.x0 - wall, p.y0 - wall, p.x1 + wall, p.y1 + wall,
               [&](int, int, quint64 k) { nearDoorway.insert(k); });

  for (const Prim &p: vents)
    forCells(ras, p.x0, p.y0, p.x1, p.y1, [&](int, int, quint64 k) {
      CellInfo &c = ras.cells[k];
      if (!c.vent) {
        c.vent = true;
        c.ventElevation = p.s.elevation;
        c.ventHeight = p.s.height;
      } else {
        c.ventHeight = std::max(c.ventHeight, p.s.height);
      }
    });
  auto stampRoom = [](CellInfo &c, const Prim &p) {
    c.kind = Room;
    c.shape = p.shape;
    c.seg = -1;
    c.elevation = p.roomElevation;
    c.height = p.roomHeight;
    c.lateral = 0;
    c.doorZone = false;
  };
  for (const Prim &p: rooms)
    forCells(ras, p.x0, p.y0, p.x1, p.y1, [&](int, int, quint64 k) { stampRoom(ras.cells[k], p); });
  for (const Prim &p: corridors)
    forCells(ras, p.x0, p.y0, p.x1, p.y1, [&](int i, int j, quint64 k) {
      CellInfo &c = ras.cells[k];
      if (c.kind == Room)
        return;
      const glm::dvec2 rel = centre(i, j) - p.a;
      const int lateral = int(std::lround(std::fabs(rel.x * p.dir.y - rel.y * p.dir.x)));
      if (c.kind == Corridor) {
        // Junction: the taller corridor's ceiling wins.
        c.height = std::max(c.height, p.s.height);
        c.lateral = std::min(c.lateral, lateral);
        return;
      }
      c.kind = Corridor;
      c.shape = p.shape;
      c.seg = p.seg;
      c.elevation = p.s.elevation;
      c.height = p.s.height;
      c.lateral = lateral;
      c.doorZone = nearDoorway.contains(k);
    });
  for (const Prim &p: sharedRooms)
    forCells(ras, p.x0, p.y0, p.x1, p.y1, [&](int, int, quint64 k) {
      if (nearCorridor.contains(k))
        return;
      auto it = ras.cells.find(k);
      if (it != ras.cells.end() && it->kind >= 0)
        return;
      stampRoom(ras.cells[k], p);
    });
  for (const QRect &s: doc.sealed)
    forCells(ras, s.x(), s.y(), s.x() + s.width(), s.y() + s.height(), [&](int, int, quint64 k) {
      auto it = ras.cells.find(k);
      if (it != ras.cells.end())
        it->sealed = true;
    });
  return ras;
}

namespace {

// Which space the open intervals of a cell offer across [z0, z1] (a side face) or just above / below a height
// (z0 == z1, dir = +1 / -1).
FaceCategory openCategory(const Columns &cols, quint64 key, int z0, int z1, int dir, FaceCategory space) {
  auto touches = [&](const Iv &iv) {
    for (const auto &[a, b]: iv) {
      if (dir > 0 ? (a <= z0 && b > z0) : dir < 0 ? (a < z0 && b >= z0) : (a < z1 && b > z0))
        return true;
    }
    return false;
  };
  if (const auto s = cols.openS.constFind(key); s != cols.openS.constEnd() && touches(*s))
    return space;
  if (const auto v = cols.openV.constFind(key); v != cols.openV.constEnd() && touches(*v))
    return FaceVent;
  return FaceNone;
}

void classifyFaces(const Columns &cols, Box &b) {
  const auto &xs = cols.xs, &ys = cols.ys;
  auto lineIndex = [](const std::vector<int> &lines, int v) {
    const auto it = std::lower_bound(lines.begin(), lines.end(), v);
    return it != lines.end() && *it == v ? int(it - lines.begin()) : -1;
  };
  const int i0 = lineIndex(xs, b.mn.x), i1 = lineIndex(xs, b.mx.x);
  const int j0 = lineIndex(ys, b.mn.y), j1 = lineIndex(ys, b.mx.y);
  if (i0 < 0 || i1 < 0 || j0 < 0 || j1 < 0)
    return;
  auto pick = [](FaceCategory cur, FaceCategory c) {
    return cur == FaceNone || (cur == FaceVent && c != FaceNone) ? c : cur;
  };
  for (int j = j0; j < j1; ++j) {
    b.faces[0] = pick(b.faces[0], openCategory(cols, cellKey(i1, j), b.mn.z, b.mx.z, 0, FaceWall));
    b.faces[1] = pick(b.faces[1], openCategory(cols, cellKey(i0 - 1, j), b.mn.z, b.mx.z, 0, FaceWall));
  }
  for (int i = i0; i < i1; ++i) {
    b.faces[2] = pick(b.faces[2], openCategory(cols, cellKey(i, j1), b.mn.z, b.mx.z, 0, FaceWall));
    b.faces[3] = pick(b.faces[3], openCategory(cols, cellKey(i, j0 - 1), b.mn.z, b.mx.z, 0, FaceWall));
  }
  for (int j = j0; j < j1; ++j)
    for (int i = i0; i < i1; ++i) {
      b.faces[4] = pick(b.faces[4], openCategory(cols, cellKey(i, j), b.mx.z, b.mx.z, +1, FaceFloor));
      b.faces[5] = pick(b.faces[5], openCategory(cols, cellKey(i, j), b.mn.z, b.mn.z, -1, FaceCeiling));
    }
}

} // namespace

std::vector<Box> generate(const Doc &doc, int *columns) {
  const Columns cols = computeColumns(doc);
  std::vector<Box> out;
  for (int pass = 0; pass < 2; ++pass) {
    const auto &solid = pass == 0 ? cols.solidS : cols.solidV;
    std::map<std::pair<int, int>, std::vector<QPoint>> groups;
    for (auto it = solid.constBegin(); it != solid.constEnd(); ++it) {
      std::vector<int> cuts = cols.cuts.value(it.key());
      std::sort(cuts.begin(), cuts.end());
      cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
      for (const auto &iv: it.value()) {
        int lo = iv.first;
        for (const int z: cuts)
          if (z > lo && z < iv.second) {
            groups[{lo, z}].push_back(keyCell(it.key()));
            lo = z;
          }
        groups[{lo, iv.second}].push_back(keyCell(it.key()));
      }
    }
    for (const auto &[z, cells]: groups)
      mergeInto(cells, z, cols.xs, cols.ys, pass == 1, out);
  }
  for (Box &b: out)
    classifyFaces(cols, b);
  if (columns)
    *columns = int(cols.solidS.size() + cols.solidV.size());
  return out;
}

std::vector<std::pair<int, int>> solidColumn(const Doc &doc, const int x, const int y) {
  const Columns cols = computeColumns(doc);
  const int i = int(std::upper_bound(cols.xs.begin(), cols.xs.end(), x) - cols.xs.begin()) - 1;
  const int j = int(std::upper_bound(cols.ys.begin(), cols.ys.end(), y) - cols.ys.begin()) - 1;
  if (i < 0 || j < 0 || i + 1 >= int(cols.xs.size()) || j + 1 >= int(cols.ys.size()))
    return {};
  Iv s = cols.solidS.value(cellKey(i, j));
  const Iv v = cols.solidV.value(cellKey(i, j));
  s.insert(s.end(), v.begin(), v.end());
  return normalized(s);
}

namespace {
constexpr int kJsonVersion = 2;
QJsonArray rectJson(const QRect &r) { return QJsonArray {r.x(), r.y(), r.width(), r.height()}; }
QRect rectFrom(const QJsonArray &c, int scale) {
  return {c.at(0).toInt() * scale, c.at(1).toInt() * scale, c.at(2).toInt() * scale, c.at(3).toInt() * scale};
}
} // namespace

QJsonObject toJson(const Doc &doc) {
  QJsonObject o;
  o[QStringLiteral("v")] = kJsonVersion;
  o[QStringLiteral("id")] = doc.id;
  o[QStringLiteral("name")] = doc.name;
  o[QStringLiteral("next")] = doc.nextShapeId;
  o[QStringLiteral("origin")] = QJsonArray {doc.origin.x, doc.origin.y, doc.origin.z};
  QJsonArray paths;
  for (const Path &p: doc.paths) {
    QJsonObject po;
    po[QStringLiteral("id")] = p.id;
    po[QStringLiteral("kind")] = int(p.kind);
    QJsonArray nodes;
    for (const QPoint &n: p.nodes)
      nodes.append(QJsonArray {n.x(), n.y()});
    po[QStringLiteral("nodes")] = nodes;
    QJsonArray segs;
    for (const Segment &s: p.segs)
      segs.append(QJsonArray {s.width, s.height, s.elevation});
    po[QStringLiteral("segs")] = segs;
    paths.append(po);
  }
  o[QStringLiteral("paths")] = paths;
  QJsonArray rooms;
  for (const RoomShape &r: doc.rooms) {
    QJsonObject ro;
    ro[QStringLiteral("id")] = r.id;
    ro[QStringLiteral("height")] = r.height;
    ro[QStringLiteral("elevation")] = r.elevation;
    ro[QStringLiteral("contact")] = r.contact;
    QJsonArray rects;
    for (const QRect &rc: r.rects)
      rects.append(rectJson(rc));
    ro[QStringLiteral("rects")] = rects;
    rooms.append(ro);
  }
  o[QStringLiteral("rooms")] = rooms;
  QJsonArray sealed;
  for (const QRect &s: doc.sealed)
    sealed.append(rectJson(s));
  o[QStringLiteral("sealed")] = sealed;
  const Config &c = doc.config;
  QJsonObject co;
  co[QStringLiteral("wall")] = c.wall;
  co[QStringLiteral("floor")] = c.floor;
  co[QStringLiteral("ceiling")] = c.ceiling;
  co[QStringLiteral("ventWall")] = c.ventWall;
  co[QStringLiteral("doorWidth")] = c.doorWidth;
  co[QStringLiteral("doorHeight")] = c.doorHeight;
  co[QStringLiteral("corridorWidth")] = c.corridorWidth;
  co[QStringLiteral("corridorHeight")] = c.corridorHeight;
  co[QStringLiteral("roomHeight")] = c.roomHeight;
  co[QStringLiteral("roomContact")] = c.roomContact;
  co[QStringLiteral("ventWidth")] = c.ventWidth;
  co[QStringLiteral("ventHeight")] = c.ventHeight;
  co[QStringLiteral("ventElevation")] = c.ventElevation;
  co[QStringLiteral("matFloor")] = c.matFloor;
  co[QStringLiteral("matWall")] = c.matWall;
  co[QStringLiteral("matCeiling")] = c.matCeiling;
  co[QStringLiteral("matVent")] = c.matVent;
  co[QStringLiteral("target")] = c.target;
  co[QStringLiteral("groupName")] = c.groupName;
  co[QStringLiteral("groupId")] = c.groupId;
  o[QStringLiteral("config")] = co;
  return o;
}

Doc fromJson(const QJsonObject &o) {
  Doc d;
  const QJsonObject co = o.value(QStringLiteral("config")).toObject();
  // Version 1 stored shapes in grid cells of config.cell Source units.
  const bool legacy = o.value(QStringLiteral("v")).toInt(1) < 2;
  const int scale = legacy ? std::max(1, co.value(QStringLiteral("cell")).toInt(16)) : 1;
  const int legacyContact = legacy ? std::clamp(co.value(QStringLiteral("doorMode")).toInt(0), 0, 1) : ContactOpen;

  d.id = o.value(QStringLiteral("id")).toInt();
  d.name = o.value(QStringLiteral("name")).toString(QStringLiteral("Layout"));
  d.nextShapeId = std::max(1, o.value(QStringLiteral("next")).toInt(1));
  const QJsonArray org = o.value(QStringLiteral("origin")).toArray();
  if (org.size() == 3)
    d.origin = {float(org[0].toDouble()), float(org[1].toDouble()), float(org[2].toDouble())};
  for (const auto &pv: o.value(QStringLiteral("paths")).toArray()) {
    const QJsonObject po = pv.toObject();
    Path p;
    p.id = po.value(QStringLiteral("id")).toInt();
    p.kind = Kind(std::clamp(po.value(QStringLiteral("kind")).toInt(), 0, 2));
    for (const auto &nv: po.value(QStringLiteral("nodes")).toArray()) {
      const QJsonArray n = nv.toArray();
      p.nodes.emplace_back(n.at(0).toInt() * scale, n.at(1).toInt() * scale);
    }
    for (const auto &sv: po.value(QStringLiteral("segs")).toArray()) {
      const QJsonArray s = sv.toArray();
      p.segs.push_back({s.at(0).toInt(128), s.at(1).toInt(128), s.at(2).toInt(0)});
    }
    p.segs.resize(p.nodes.size() > 0 ? p.nodes.size() - 1 : 0);
    d.paths.push_back(std::move(p));
  }
  for (const auto &rv: o.value(QStringLiteral("rooms")).toArray()) {
    const QJsonObject ro = rv.toObject();
    RoomShape r;
    r.id = ro.value(QStringLiteral("id")).toInt();
    r.height = ro.value(QStringLiteral("height")).toInt(legacy ? 192 : 128);
    r.elevation = ro.value(QStringLiteral("elevation")).toInt(0);
    r.contact = std::clamp(ro.value(QStringLiteral("contact")).toInt(legacyContact), 0, 2);
    for (const auto &cv: ro.value(QStringLiteral("rects")).toArray())
      r.rects.push_back(rectFrom(cv.toArray(), scale));
    d.rooms.push_back(std::move(r));
  }
  for (const auto &sv: o.value(QStringLiteral("sealed")).toArray()) {
    const QJsonArray s = sv.toArray();
    if (s.size() >= 4)
      d.sealed.push_back(rectFrom(s, 1));
    else
      d.sealed.emplace_back(s.at(0).toInt() * scale, s.at(1).toInt() * scale, scale, scale);
  }
  Config &c = d.config;
  auto gi = [&co](const char *k, int def) { return co.value(QLatin1String(k)).toInt(def); };
  c.wall = gi("wall", legacy ? 16 : c.wall);
  c.floor = gi("floor", c.floor);
  c.ceiling = gi("ceiling", c.ceiling);
  c.ventWall = gi("ventWall", c.ventWall);
  c.doorWidth = gi("doorWidth", c.doorWidth);
  c.doorHeight = gi("doorHeight", c.doorHeight);
  c.corridorWidth = gi("corridorWidth", c.corridorWidth);
  c.corridorHeight = gi("corridorHeight", c.corridorHeight);
  c.roomHeight = gi("roomHeight", legacy ? 192 : c.roomHeight);
  c.roomContact = std::clamp(gi("roomContact", legacyContact), 0, 2);
  c.ventWidth = gi("ventWidth", c.ventWidth);
  c.ventHeight = gi("ventHeight", c.ventHeight);
  c.ventElevation = gi("ventElevation", legacy ? 16 : c.ventElevation);
  c.matFloor = co.value(QStringLiteral("matFloor")).toString();
  c.matWall = co.value(QStringLiteral("matWall")).toString();
  c.matCeiling = co.value(QStringLiteral("matCeiling")).toString();
  c.matVent = co.value(QStringLiteral("matVent")).toString();
  c.target = gi("target", c.target);
  c.groupName = co.value(QStringLiteral("groupName")).toString(c.groupName);
  c.groupId = gi("groupId", c.groupId);
  return d;
}

} // namespace eng::layout
