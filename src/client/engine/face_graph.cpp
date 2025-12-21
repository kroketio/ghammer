#include "face_graph.h"

#include <unordered_set>

#include <algorithm>
#include <cmath>

#include "brush.h"
#include "gameobject.h"
#include "mesh.h"
#include "scene.h"

namespace eng {

namespace {

glm::dvec3 newell(const std::vector<glm::dvec3> &poly) {
  glm::dvec3 n(0.0);
  for (size_t i = 0; i < poly.size(); ++i) {
    const glm::dvec3 &a = poly[i];
    const glm::dvec3 &b = poly[(i + 1) % poly.size()];
    n.x += (a.y - b.y) * (a.z + b.z);
    n.y += (a.z - b.z) * (a.x + b.x);
    n.z += (a.x - b.x) * (a.y + b.y);
  }
  return n;
}

bool toolMaterial(const QString &name) {
  static const char *kTools[] = {"skybox", "trigger", "toolsclip", "playerclip", "npcclip", "toolshint",
                                 "toolsskip", "areaportal", "occluder", "toolsfog", "toolsinvisible", "collision"};
  const QString n = name.toLower();
  for (const char *t: kTools)
    if (n.contains(QLatin1String(t)))
      return true;
  return false;
}

} // namespace

quint64 FaceGraph::cellKey(int x, int y, int z) const {
  return (quint64(quint32(x) & 0x1fffffu) << 42) | (quint64(quint32(y) & 0x1fffffu) << 21) | quint64(quint32(z) & 0x1fffffu);
}

std::vector<SolidInput> FaceGraph::solidsFromBrushes(const std::vector<std::pair<quint32, ConvexBrush>> &brushes) {
  std::vector<SolidInput> out;
  for (const auto &[id, b]: brushes) {
    SolidInput s;
    s.objId = id;
    for (size_t i = 0; i < b.polygons.size() && i < b.faces.size(); ++i) {
      SolidInput::Face f;
      f.face = int(i);
      f.pickId = makePickingId(id, uint32_t(i));
      f.poly = b.polygons[i];
      f.groups = b.faces[i].groups;
      if (b.faces[i].material)
        f.material = b.faces[i].material->name;
      s.faces.push_back(std::move(f));
    }
    out.push_back(std::move(s));
  }
  return out;
}

FaceGraph FaceGraph::fromScene(const Scene &scene) {
  std::vector<SolidInput> solids;
  solids.reserve(scene.gameObjects.size());
  std::vector<glm::vec3> local;
  for (const auto &obj: scene.gameObjects) {
    const auto mesh = obj ? obj->getMesh() : nullptr;
    if (!mesh || obj->hidden() || mesh->faceCount() == 0)
      continue;
    const glm::dmat4 model(obj->getModelMatrix());
    SolidInput s;
    s.objId = obj->stableId();
    int tools = 0;
    for (int f = 0; f < mesh->faceCount(); ++f) {
      if (!mesh->getFacePolygon(f, local) || local.size() < 3)
        continue;
      SolidInput::Face face;
      face.face = f;
      face.pickId = mesh->getFacePickingID(f);
      face.groups = mesh->faceGroups(f);
      face.poly.reserve(local.size());
      for (const auto &p: local)
        face.poly.push_back(glm::dvec3(model * glm::dvec4(glm::dvec3(p), 1.0)));
      if (const auto m = mesh->faceMaterialRef(f)) {
        face.material = m->name;
        if (toolMaterial(m->name))
          ++tools;
      }
      s.faces.push_back(std::move(face));
    }
    s.occluder = tools < int(s.faces.size()) / 2 + 1;
    if (!s.faces.empty())
      solids.push_back(std::move(s));
  }
  return build(solids);
}

FaceGraph FaceGraph::build(const std::vector<SolidInput> &solids, const bool withLinks) {
  FaceGraph g;
  g.mn = glm::dvec3(1e300);
  g.mx = glm::dvec3(-1e300);
  for (const SolidInput &in: solids) {
    const int solidIndex = int(g.m_solids.size());
    Solid solid;
    solid.objId = in.objId;
    solid.occluder = in.occluder;
    solid.mn = glm::dvec3(1e300);
    solid.mx = glm::dvec3(-1e300);
    glm::dvec3 centre(0.0);
    size_t count = 0;
    for (const auto &f: in.faces)
      for (const auto &p: f.poly) {
        centre += p;
        ++count;
        solid.mn = glm::min(solid.mn, p);
        solid.mx = glm::max(solid.mx, p);
        solid.verts.push_back(p);
      }
    if (count == 0)
      continue;
    centre /= double(count);
    // Winding from the sign of the solid's volume, the same for every face. "Away from the centre" only holds for
    // convex solids: bent modifier pieces are not, and an inside-out face makes its solid reach past it (the face next
    // to it then reads as covered). No volume (a single plane): fall back to the centre.
    double volume = 0.0;
    for (const auto &f: in.faces)
      if (f.poly.size() >= 3)
        volume += glm::dot(f.poly.front(), newell(f.poly)) / 6.0;
    const glm::dvec3 ext = solid.mx - solid.mn;
    const int winding =
        std::fabs(volume) > 1e-6 * std::max(ext.x * ext.y * ext.z, 1e-12) ? (volume > 0.0 ? 1 : -1) : 0;
    const size_t firstNode = g.nodes.size();
    for (const auto &f: in.faces) {
      glm::dvec3 n = newell(f.poly);
      const double len = glm::length(n);
      if (len < 1e-9)
        continue;
      FaceNode node;
      node.objId = in.objId;
      node.pickId = f.pickId;
      node.face = f.face;
      node.solid = solidIndex;
      node.poly = f.poly;
      node.groups = f.groups;
      node.material = f.material;
      node.area = len * 0.5;
      n /= len;
      glm::dvec3 c(0.0);
      node.mn = glm::dvec3(1e300);
      node.mx = glm::dvec3(-1e300);
      for (const auto &p: f.poly) {
        c += p;
        node.mn = glm::min(node.mn, p);
        node.mx = glm::max(node.mx, p);
      }
      c /= double(f.poly.size());
      if (winding != 0 ? winding < 0 : glm::dot(n, c - centre) < 0.0)
        n = -n;
      node.normal = n;
      node.centroid = c;
      node.dist = glm::dot(n, c);
      node.selectable = in.occluder;
      solid.normals.push_back(n);
      solid.dists.push_back(node.dist);
      g.nodes.push_back(std::move(node));
    }
    if (g.nodes.size() == firstNode)
      continue;
    g.mn = glm::min(g.mn, solid.mn);
    g.mx = glm::max(g.mx, solid.mx);
    g.m_solids.push_back(std::move(solid));
  }
  if (g.nodes.empty()) {
    g.mn = g.mx = glm::dvec3(0.0);
    return g;
  }
  const glm::dvec3 ext = g.mx - g.mn;
  g.m_cell = std::max(4.0, std::cbrt(std::max(1.0, ext.x * ext.y * ext.z) / std::max<size_t>(1, g.m_solids.size())));
  g.m_dims = glm::ivec3(glm::floor(ext / g.m_cell)) + 1;
  g.m_rayStamp.assign(g.m_solids.size(), 0);
  for (int s = 0; s < int(g.m_solids.size()); ++s) {
    const Solid &solid = g.m_solids[size_t(s)];
    const glm::ivec3 a(glm::floor((solid.mn - g.mn) / g.m_cell)), b(glm::floor((solid.mx - g.mn) / g.m_cell));
    for (int z = a.z; z <= b.z; ++z)
      for (int y = a.y; y <= b.y; ++y)
        for (int x = a.x; x <= b.x; ++x)
          g.m_solidGrid[g.cellKey(x, y, z)].push_back(s);
  }
  for (int i = 0; i < int(g.nodes.size()); ++i)
    g.m_byPick[g.nodes[size_t(i)].pickId] = i;
  g.computeHidden();
  if (withLinks)
    g.computeLinks();
  return g;
}

int FaceGraph::indexOfPick(const quint32 pickId) const {
  const auto it = m_byPick.find(pickId);
  return it == m_byPick.end() ? -1 : it->second;
}

int FaceGraph::solidContaining(const glm::dvec3 &p, const int exceptSolid) const {
  const glm::ivec3 c(glm::floor((p - mn) / m_cell));
  const auto it = m_solidGrid.find(cellKey(c.x, c.y, c.z));
  if (it == m_solidGrid.end())
    return -1;
  for (const int s: it->second) {
    if (s == exceptSolid)
      continue;
    const Solid &solid = m_solids[size_t(s)];
    if (!solid.occluder)
      continue;
    if (glm::any(glm::lessThan(p, solid.mn - kEps)) || glm::any(glm::greaterThan(p, solid.mx + kEps)))
      continue;
    bool inside = true;
    for (size_t k = 0; k < solid.normals.size() && inside; ++k)
      inside = glm::dot(solid.normals[k], p) - solid.dists[k] < -kEps * 0.5;
    if (inside)
      return s;
  }
  return -1;
}

double FaceGraph::solidDepth(const int solid, const glm::dvec3 &dir) const {
  if (solid < 0 || solid >= int(m_solids.size()))
    return 0.0;
  double lo = 1e300, hi = -1e300;
  for (const auto &v: m_solids[size_t(solid)].verts) {
    const double d = glm::dot(dir, v);
    lo = std::min(lo, d);
    hi = std::max(hi, d);
  }
  return hi > lo ? hi - lo : 0.0;
}

bool FaceGraph::solidsTouch(const int a, const int b) const {
  if (a == b || a < 0 || b < 0 || a >= int(m_solids.size()) || b >= int(m_solids.size()))
    return false;
  const Solid &A = m_solids[size_t(a)];
  const Solid &B = m_solids[size_t(b)];
  if (glm::any(glm::greaterThan(A.mn, B.mx + kEps)) || glm::any(glm::greaterThan(B.mn, A.mx + kEps)))
    return false;
  // A face plane of one solid with every vertex of the other beyond it separates them.
  auto separatedBy = [](const Solid &planes, const Solid &other) {
    for (size_t k = 0; k < planes.normals.size(); ++k) {
      bool allOut = true;
      for (const auto &v: other.verts)
        if (glm::dot(planes.normals[k], v) - planes.dists[k] <= kEps) {
          allOut = false;
          break;
        }
      if (allOut)
        return true;
    }
    return false;
  };
  return !separatedBy(A, B) && !separatedBy(B, A);
}

std::vector<int> FaceGraph::touchingSolids(const int s) const {
  std::vector<int> out;
  if (s < 0 || s >= int(m_solids.size()))
    return out;
  const Solid &S = m_solids[size_t(s)];
  const glm::ivec3 a = glm::clamp(glm::ivec3(glm::floor((S.mn - kEps - mn) / m_cell)), glm::ivec3(0), m_dims - 1);
  const glm::ivec3 b = glm::clamp(glm::ivec3(glm::floor((S.mx + kEps - mn) / m_cell)), glm::ivec3(0), m_dims - 1);
  std::vector<char> seen(m_solids.size(), 0);
  seen[size_t(s)] = 1;
  for (int z = a.z; z <= b.z; ++z)
    for (int y = a.y; y <= b.y; ++y)
      for (int x = a.x; x <= b.x; ++x) {
        const auto it = m_solidGrid.find(cellKey(x, y, z));
        if (it == m_solidGrid.end())
          continue;
        for (const int o: it->second) {
          if (seen[size_t(o)])
            continue;
          seen[size_t(o)] = 1;
          if (solidsTouch(s, o))
            out.push_back(o);
        }
      }
  std::sort(out.begin(), out.end());
  return out;
}

bool FaceGraph::hitSolid(const Solid &s, const glm::dvec3 &origin, const glm::dvec3 &dir, double &best) const {
  if (!s.occluder)
    return false;
  double enter = 0.0, leave = best;
  for (size_t k = 0; k < s.normals.size() && enter <= leave; ++k) {
    const double denom = glm::dot(s.normals[k], dir);
    const double distOut = glm::dot(s.normals[k], origin) - s.dists[k];
    if (std::fabs(denom) < 1e-12) {
      if (distOut > 0.0)
        return false;
      continue;
    }
    const double t = -distOut / denom;
    if (denom < 0.0)
      enter = std::max(enter, t);
    else
      leave = std::min(leave, t);
  }
  if (enter <= leave && leave > kEps && enter < best) {
    best = std::max(0.0, enter);
    return true;
  }
  return false;
}

// Walks the solid grid along the ray; stops at the first cell whose exit lies beyond the nearest hit so far.
bool FaceGraph::raycast(const glm::dvec3 &origin, const glm::dvec3 &dir, const double maxT, double *tOut) const {
  if (m_solids.empty())
    return false;
  const glm::dvec3 lo = mn - kEps, hi = mn + glm::dvec3(m_dims) * m_cell + kEps;
  double t0 = 0.0, t1 = maxT;
  for (int a = 0; a < 3; ++a) {
    if (std::fabs(dir[a]) < 1e-12) {
      if (origin[a] < lo[a] || origin[a] > hi[a])
        return false;
      continue;
    }
    double ta = (lo[a] - origin[a]) / dir[a], tb = (hi[a] - origin[a]) / dir[a];
    if (ta > tb)
      std::swap(ta, tb);
    t0 = std::max(t0, ta);
    t1 = std::min(t1, tb);
    if (t0 > t1)
      return false;
  }
  if (++m_rayEpoch == 0) {
    std::fill(m_rayStamp.begin(), m_rayStamp.end(), 0u);
    m_rayEpoch = 1;
  }
  const glm::dvec3 p = origin + dir * t0;
  glm::ivec3 c = glm::clamp(glm::ivec3(glm::floor((p - mn) / m_cell)), glm::ivec3(0), m_dims - 1);
  glm::ivec3 step;
  glm::dvec3 tMax, tDelta;
  for (int a = 0; a < 3; ++a) {
    if (std::fabs(dir[a]) < 1e-12) {
      step[a] = 0;
      tMax[a] = 1e300;
      tDelta[a] = 1e300;
      continue;
    }
    step[a] = dir[a] > 0.0 ? 1 : -1;
    const double boundary = mn[a] + double(c[a] + (step[a] > 0 ? 1 : 0)) * m_cell;
    tMax[a] = (boundary - origin[a]) / dir[a];
    tDelta[a] = m_cell / std::fabs(dir[a]);
  }
  double best = maxT;
  bool hit = false;
  for (;;) {
    if (const auto it = m_solidGrid.find(cellKey(c.x, c.y, c.z)); it != m_solidGrid.end())
      for (const int s: it->second) {
        if (m_rayStamp[size_t(s)] == m_rayEpoch)
          continue;
        m_rayStamp[size_t(s)] = m_rayEpoch;
        hit |= hitSolid(m_solids[size_t(s)], origin, dir, best);
      }
    int a = 0;
    if (tMax[1] < tMax[a])
      a = 1;
    if (tMax[2] < tMax[a])
      a = 2;
    const double exit = tMax[a];
    if ((hit && best <= exit) || exit > t1 || step[a] == 0)
      break;
    c[a] += step[a];
    if (c[a] < 0 || c[a] >= m_dims[a])
      break;
    tMax[a] += tDelta[a];
  }
  if (hit && tOut)
    *tOut = best;
  return hit;
}

void FaceGraph::computeHidden() {
  for (FaceNode &n: nodes)
    n.hidden = faceCovered(n);
}

bool FaceGraph::faceCovered(const FaceNode &n) const {
  if (n.poly.size() < 3)
    return false;
  // Just in front of the face.
  const glm::dvec3 off = n.normal * (kEps * 5.0);
  // A point in the open means the face shows.
  if (!insideOtherSolid(n.centroid + off, n.solid))
    return false;
  // Candidates: occluding solids whose box meets the face's.
  const glm::dvec3 lo = n.mn + off - kEps, hi = n.mx + off + kEps;
  std::vector<int> candidates;
  {
    const glm::ivec3 a(glm::floor((lo - mn) / m_cell)), b(glm::floor((hi - mn) / m_cell));
    std::unordered_set<int> seen;
    for (int z = a.z; z <= b.z; ++z)
      for (int y = a.y; y <= b.y; ++y)
        for (int x = a.x; x <= b.x; ++x) {
          const auto it = m_solidGrid.find(cellKey(x, y, z));
          if (it == m_solidGrid.end())
            continue;
          for (const int s: it->second) {
            if (s == n.solid || !seen.insert(s).second)
              continue;
            const Solid &solid = m_solids[size_t(s)];
            if (!solid.occluder || glm::any(glm::lessThan(hi, solid.mn - kEps))
                || glm::any(glm::greaterThan(lo, solid.mx + kEps)))
              continue;
            candidates.push_back(s);
          }
        }
  }
  // Cut each candidate's part out of the remaining convex pieces: outside any one plane stays, inside all goes.
  // Inside = behind every plane, or up to tol in front of one (edges on a neighbour's sides count as covered).
  const double tol = kEps * 0.5;
  auto area = [](const std::vector<glm::dvec3> &p) { return glm::length(newell(p)) * 0.5; };
  const double minArea = std::max(1e-6, area(n.poly) * 1e-6);
  std::vector<std::vector<glm::dvec3>> left;
  {
    std::vector<glm::dvec3> p;
    for (const glm::dvec3 &v: n.poly)
      p.push_back(v + off);
    left.push_back(std::move(p));
  }
  for (const int s: candidates) {
    const Solid &solid = m_solids[size_t(s)];
    std::vector<std::vector<glm::dvec3>> next;
    for (std::vector<glm::dvec3> cur: left) {
      for (size_t k = 0; k < solid.normals.size() && !cur.empty(); ++k) {
        // value < 0: inside this plane's half-space (or within tol).
        std::vector<double> d(cur.size());
        bool anyOut = false, anyIn = false;
        for (size_t i = 0; i < cur.size(); ++i) {
          d[i] = glm::dot(solid.normals[k], cur[i]) - solid.dists[k] - tol;
          anyOut = anyOut || d[i] >= 0.0;
          anyIn = anyIn || d[i] < 0.0;
        }
        if (!anyIn) {
          next.push_back(std::move(cur)); // all outside this plane: the solid misses it
          cur.clear();
          break;
        }
        if (!anyOut)
          continue; // all inside this plane: on to the next
        std::vector<glm::dvec3> out, in;
        for (size_t i = 0; i < cur.size(); ++i) {
          const size_t j = (i + 1) % cur.size();
          (d[i] >= 0.0 ? out : in).push_back(cur[i]);
          if ((d[i] >= 0.0) != (d[j] >= 0.0)) {
            const glm::dvec3 x = cur[i] + (cur[j] - cur[i]) * (d[i] / (d[i] - d[j]));
            out.push_back(x);
            in.push_back(x);
          }
        }
        if (out.size() >= 3 && area(out) > minArea)
          next.push_back(std::move(out));
        cur = std::move(in);
        if (cur.size() < 3 || area(cur) <= minArea)
          cur.clear();
      }
      // What is still in `cur` lies inside the solid: covered.
    }
    left = std::move(next);
    if (left.empty())
      return true;
  }
  return false;
}

namespace {

// Concave when each face has a part in front of the other's plane (inside corner), else convex.
LinkKind linkKind(const FaceNode &A, const FaceNode &B) {
  if (glm::dot(A.normal, B.normal) > 0.999)
    return LinkKind::Flat;
  auto inFront = [](const FaceNode &of, const FaceNode &plane) {
    for (const auto &v: of.poly)
      if (glm::dot(plane.normal, v) - plane.dist > FaceGraph::kEps * 5.0)
        return true;
    return false;
  };
  return inFront(B, A) && inFront(A, B) ? LinkKind::Concave : LinkKind::Convex;
}

// The part of segment a-b inside convex polygon `poly` (in its plane), as segment parameters.
bool clipToPolygon(const FaceNode &n, const glm::dvec3 &a, const glm::dvec3 &b, double &t0, double &t1) {
  t0 = 0.0;
  t1 = 1.0;
  const glm::dvec3 d = b - a;
  const auto &poly = n.poly;
  for (size_t k = 0; k < poly.size(); ++k) {
    const glm::dvec3 &p = poly[k];
    const glm::dvec3 &q = poly[(k + 1) % poly.size()];
    glm::dvec3 m = glm::cross(n.normal, q - p);
    const double len = glm::length(m);
    if (len < 1e-12)
      continue;
    m /= len;
    if (glm::dot(m, n.centroid - p) < 0.0)
      m = -m;
    const double fa = glm::dot(m, a - p) + FaceGraph::kEps;
    const double fd = glm::dot(m, d);
    if (std::fabs(fd) < 1e-12) {
      if (fa < 0.0)
        return false;
      continue;
    }
    const double t = -fa / fd;
    if (fd > 0.0)
      t0 = std::max(t0, t);
    else
      t1 = std::min(t1, t);
    if (t0 > t1)
      return false;
  }
  return true;
}

} // namespace

void FaceGraph::computeLinks() {
  links.assign(nodes.size(), {});
  const double cell = std::max(4.0, m_cell);
  std::unordered_map<quint64, std::vector<int>> grid;
  auto cellRange = [&](const glm::dvec3 &lo, const glm::dvec3 &hi, glm::ivec3 &a, glm::ivec3 &b) {
    a = glm::ivec3(glm::floor((lo - kEps - mn) / cell));
    b = glm::ivec3(glm::floor((hi + kEps - mn) / cell));
  };
  for (int i = 0; i < int(nodes.size()); ++i) {
    glm::ivec3 a, b;
    cellRange(nodes[size_t(i)].mn, nodes[size_t(i)].mx, a, b);
    for (int z = a.z; z <= b.z; ++z)
      for (int y = a.y; y <= b.y; ++y)
        for (int x = a.x; x <= b.x; ++x)
          grid[cellKey(x, y, z)].push_back(i);
  }
  std::unordered_map<quint64, char> done;
  std::vector<int> candidates;
  for (int i = 0; i < int(nodes.size()); ++i) {
    const FaceNode &A = nodes[size_t(i)];
    for (size_t k = 0; k < A.poly.size(); ++k) {
      const glm::dvec3 ea = A.poly[k], eb = A.poly[(k + 1) % A.poly.size()];
      const double len = glm::length(eb - ea);
      if (len < 0.1)
        continue;
      glm::ivec3 ca, cb;
      cellRange(glm::min(ea, eb), glm::max(ea, eb), ca, cb);
      candidates.clear();
      for (int z = ca.z; z <= cb.z; ++z)
        for (int y = ca.y; y <= cb.y; ++y)
          for (int x = ca.x; x <= cb.x; ++x)
            if (const auto it = grid.find(cellKey(x, y, z)); it != grid.end())
              candidates.insert(candidates.end(), it->second.begin(), it->second.end());
      std::sort(candidates.begin(), candidates.end());
      candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
      for (const int j: candidates) {
        if (j == i)
          continue;
        const FaceNode &B = nodes[size_t(j)];
        if (glm::dot(A.normal, B.normal) < -0.999)
          continue;
        if (std::fabs(glm::dot(B.normal, ea) - B.dist) > kEps || std::fabs(glm::dot(B.normal, eb) - B.dist) > kEps)
          continue;
        const quint64 pairKey = (quint64(quint32(std::min(i, j))) << 32) | quint32(std::max(i, j));
        if (done.count(pairKey))
          continue;
        double t0, t1;
        if (!clipToPolygon(B, ea, eb, t0, t1) || (t1 - t0) * len < 0.1)
          continue;
        done[pairKey] = 1;
        const glm::dvec3 pa = ea + (eb - ea) * t0, pb = ea + (eb - ea) * t1;
        const LinkKind kind = linkKind(A, B);
        links[size_t(i)].push_back({j, kind, pa, pb});
        links[size_t(j)].push_back({i, kind, pa, pb});
      }
    }
  }
}

} // namespace eng
