#include "face_fill.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include <QDebug>
#include <QHash>

#include "gameobject.h"
#include "mesh.h"
#include "scene.h"

namespace eng {

namespace {

constexpr double kBig = std::numeric_limits<double>::max();

// Traces of the storey solve (FILL_DEBUG).
bool fillVerbose() {
  static const bool v = !qgetenv("FILL_DEBUG").isEmpty();
  return v;
}

int dominantAxis(const glm::vec3 &n) {
  const float ax = std::fabs(n.x);
  const float ay = std::fabs(n.y);
  const float az = std::fabs(n.z);
  if (ax >= ay && ax >= az)
    return 0;
  return ay >= az ? 1 : 2;
}

// In-plane frame: a vertical wall's v is world up (heights read straight off the rect); u is the remaining
// horizontal axis, the run's direction.
void basisFor(const int axis, glm::vec3 &u, glm::vec3 &v, bool &vertical) {
  if (axis == 1) {
    u = {1.0f, 0.0f, 0.0f};
    v = {0.0f, 0.0f, 1.0f};
    vertical = false;
    return;
  }
  u = axis == 0 ? glm::vec3(0.0f, 0.0f, 1.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
  v = {0.0f, 1.0f, 0.0f};
  vertical = true;
}

double overlapLen(const double a0, const double a1, const double b0, const double b1) {
  return std::min(a1, b1) - std::max(a0, b0);
}

bool rectsTouch(const FaceInfo &a, const FaceInfo &b, const double gap) {
  const double ou = overlapLen(a.u0, a.u1, b.u0, b.u1);
  const double ov = overlapLen(a.v0, a.v1, b.v0, b.v1);
  if (ou < -gap || ov < -gap)
    return false;
  // Rects meeting only at a corner share nothing: one axis must overlap by a real amount.
  const double reach = 0.05
      * std::max(std::max(a.spanU(), a.spanV()), std::max(b.spanU(), b.spanV()));
  return (ou > reach && ov > -gap) || (ov > reach && ou > -gap);
}

bool cornerTouch(const FaceInfo &a, const FaceInfo &b, const double gap) {
  if (a.axis == b.axis || !a.vertical || !b.vertical)
    return false;

  const glm::dvec3 am = a.xzMin();
  const glm::dvec3 ax = a.xzMax();
  const glm::dvec3 bm = b.xzMin();
  const glm::dvec3 bx = b.xzMax();

  const double dx = std::max({am.x - bx.x, bm.x - ax.x, 0.0});
  const double dz = std::max({am.z - bx.z, bm.z - ax.z, 0.0});
  if (dx > gap || dz > gap)
    return false;

  const double ov = overlapLen(a.v0, a.v1, b.v0, b.v1);
  return ov > 0.05 * std::max(a.spanV(), b.spanV());
}

// Nothing within `clear` along the face's normal. Faces are axis aligned, so only parallel faces can stand in
// front: a plane solve plus rect containment. (Testing non-parallel ones solves t on the wrong axis.)
bool frontClear(const FaceInfo &f, const std::vector<FaceInfo> &all, const double clear) {
  const double nd = f.normal[f.axis];
  if (std::fabs(nd) < 1e-6)
    return true;

  for (const auto &h: all) {
    if (h.pickId == f.pickId || h.axis != f.axis)
      continue;
    const double t = (h.plane - f.center[f.axis]) / nd;
    if (t <= 0.01 || t > clear)
      continue;
    const glm::vec3 hit = f.center + glm::vec3(f.normal) * float(t);
    const double hu = glm::dot(hit, h.uDir);
    const double hv = glm::dot(hit, h.vDir);
    if (hu >= h.u0 - 1e-3 && hu <= h.u1 + 1e-3 && hv >= h.v0 - 1e-3 && hv <= h.v1 + 1e-3)
      return false;
  }
  return true;
}

// Points away from the floors' footprint: the outside-only cull.
bool facesOutside(const FaceInfo &f, const glm::dvec2 &envCenter) {
  const glm::dvec2 c(f.center.x, f.center.z);
  const glm::dvec2 n(f.normal.x, f.normal.z);
  return glm::dot(n, c - envCenter) > 1e-3;
}

void clusterYs(std::vector<double> &ys, const double tol) {
  std::sort(ys.begin(), ys.end());
  std::vector<double> out;
  for (const double y: ys) {
    if (out.empty() || std::fabs(out.back() - y) > tol)
      out.push_back(y);
  }
  ys = out;
}

} // namespace

bool isTrimFace(const FaceInfo &f, const FillParams &p) {
  if (f.area < p.minArea)
    return true;
  return f.vertical && f.spanV() < p.minSpanV;
}

glm::dvec3 FaceInfo::xzMin() const {
  glm::dvec3 m(kBig, kBig, kBig);
  for (const auto &p: quad) {
    m.x = std::min(m.x, double(p.x));
    m.y = std::min(m.y, double(p.y));
    m.z = std::min(m.z, double(p.z));
  }
  return m;
}

glm::dvec3 FaceInfo::xzMax() const {
  glm::dvec3 m(-kBig, -kBig, -kBig);
  for (const auto &p: quad) {
    m.x = std::max(m.x, double(p.x));
    m.y = std::max(m.y, double(p.y));
    m.z = std::max(m.z, double(p.z));
  }
  return m;
}

std::vector<FaceInfo> collectFaces(const Scene &scene) {
  std::vector<FaceInfo> out;

  for (const auto &obj: scene.gameObjects) {
    if (!obj)
      continue;
    const auto mesh = obj->getMesh();
    if (!mesh)
      continue;

    const glm::mat4 model = obj->getModelMatrix();
    const int faceCount = mesh->faceCount();
    out.reserve(out.size() + size_t(faceCount));

    for (int face = 0; face < faceCount; ++face) {
      const quint32 pickId = mesh->getFacePickingID(face);
      if (pickId == 0)
        continue;

      std::array<glm::vec3, 4> local {};
      if (!mesh->getFaceQuad(face, local))
        continue;

      FaceInfo fi;
      fi.pickId = pickId;
      fi.objId = obj->stableId();
      fi.face = face;
      for (int k = 0; k < 4; ++k)
        fi.quad[k] = glm::vec3(model * glm::vec4(local[k], 1.0f));

      // Diagonal cross product: area-weighted normal of a planar quad, zero (dropped) when degenerate.
      const glm::vec3 n = glm::cross(fi.quad[2] - fi.quad[0], fi.quad[3] - fi.quad[1]);
      fi.area = 0.5 * double(glm::length(n));
      if (fi.area < 1e-6)
        continue;
      // VMF winds faces with the plane pointing INTO the solid; every test here uses the outward normal, so flip once.
      fi.normal = -glm::normalize(n);

      fi.center = (fi.quad[0] + fi.quad[1] + fi.quad[2] + fi.quad[3]) * 0.25f;
      fi.axis = dominantAxis(fi.normal);
      fi.plane = double(fi.center[fi.axis]);
      basisFor(fi.axis, fi.uDir, fi.vDir, fi.vertical);

      double u0 = kBig;
      double u1 = -kBig;
      double v0 = kBig;
      double v1 = -kBig;
      for (const auto &p: fi.quad) {
        const double u = glm::dot(glm::vec3(p), fi.uDir);
        const double v = glm::dot(glm::vec3(p), fi.vDir);
        u0 = std::min(u0, u);
        u1 = std::max(u1, u);
        v0 = std::min(v0, v);
        v1 = std::max(v1, v);
      }
      fi.u0 = u0;
      fi.u1 = u1;
      fi.v0 = v0;
      fi.v1 = v1;
      out.push_back(fi);
    }
  }
  return out;
}


namespace {

// Vertical faces bucketed by axis and plane, sorted by plane, for range scans.
using PlaneBuckets = std::vector<std::pair<double, std::vector<int>>>;

int axisSlot(const int axis) { return axis == 0 ? 0 : 1; }

struct PlaneIndex {
  PlaneBuckets slot[2];
};

PlaneIndex buildPlaneIndex(const std::vector<FaceInfo> &faces) {
  PlaneIndex idx;
  QHash<quint64, std::vector<int>> tmp;
  tmp.reserve(faces.size() * 2);
  for (size_t i = 0; i < faces.size(); ++i) {
    const FaceInfo &f = faces[i];
    if (!f.vertical)
      continue;
    const quint64 key = (quint64(f.axis) << 56)
        ^ quint64(llround(f.plane * 512.0)) & 0x00ffffffffffffffull;
    tmp[key].push_back(int(i));
  }
  for (auto it = tmp.constBegin(); it != tmp.constEnd(); ++it) {
    const int axis = int((it.key() >> 56) & 0xff);
    // Not `it.value().front() ? ...`: that tests the face index, filing index 0's bucket at plane 0.0.
    const double rep = faces[size_t(it.value().front())].plane;
    idx.slot[axisSlot(axis)].push_back({rep, it.value()});
  }
  for (int k = 0; k < 2; ++k) {
    std::sort(idx.slot[k].begin(), idx.slot[k].end(),
              [](const std::pair<double, std::vector<int>> &a,
                 const std::pair<double, std::vector<int>> &b) { return a.first < b.first; });
  }
  return idx;
}

void collectInPlaneRange(const PlaneBuckets &b, const double lo, const double hi,
                        std::vector<int> &out) {
  auto it = std::lower_bound(b.begin(), b.end(), lo,
                            [](const std::pair<double, std::vector<int>> &a, const double v) {
                              return a.first < v;
                            });
  for (; it != b.end() && it->first <= hi; ++it)
    out.insert(out.end(), it->second.begin(), it->second.end());
}

// The storey band a face sits in; false outside the layer bounds.
bool bandFor(const std::vector<double> &bounds, const double y, double &b0, double &b1) {
  for (size_t i = 0; i + 1 < bounds.size(); ++i) {
    if (y >= bounds[i] - 1e-6 && y <= bounds[i + 1] + 1e-6) {
      b0 = bounds[i];
      b1 = bounds[i + 1];
      return true;
    }
  }
  return false;
}

// Each floor slab's [bottom, top]: a band matching one is the slab (between one floor's ceiling and the next
// floor), not a storey.
std::vector<std::pair<double, double>> slabIntervals(const std::vector<FaceInfo> &faces,
                                                   const FillParams &p) {
  std::vector<std::pair<double, double>> out;
  QHash<quint32, std::pair<double, double>> byObj;
  for (const auto &h: faces) {
    if (h.axis != 1 || h.area < p.floorMinArea)
      continue;
    // Only real slabs: a wall brush's top is thin in one axis, and pairing those makes the ground floor a "space".
    if (std::min(h.spanU(), h.spanV()) < p.floorMinDepth)
      continue;
    const auto it = byObj.find(h.objId);
    if (it == byObj.end())
      byObj.insert(h.objId, {h.plane, h.plane});
    else {
      it->first = std::min(it->first, h.plane);
      it->second = std::max(it->second, h.plane);
    }
  }
  for (const auto &e: byObj)
    if (e.second - e.first > 1e-6)
      out.push_back(e);
  return out;
}

} // namespace

bool spansStorey(const FaceInfo &f, const std::vector<double> &bounds, const double frac) {
  if (frac <= 0.0)
    return true;
  double b0 = 0.0, b1 = 0.0;
  if (!bandFor(bounds, (f.v0 + f.v1) * 0.5, b0, b1))
    return true;
  const double h = b1 - b0;
  if (h <= 1e-6)
    return true;
  return (f.v1 - f.v0) >= frac * h - 1e-6;
}

namespace {

// Openings: parts of the run's rectangle no accepted face covers, sealed by wall on every side (windows,
// doorways; air past a wall's end drains to the border).
std::vector<GapRect> detectGaps(const std::vector<FaceInfo> &faces,
                               const std::vector<int> &accepted,
                               const FillParams &p) {
  std::vector<GapRect> gaps;
  if (accepted.empty())
    return gaps;

  QHash<quint64, std::vector<int>> groups;
  groups.reserve(accepted.size() * 2);
  for (const int fi: accepted) {
    const FaceInfo &f = faces[size_t(fi)];
    const quint64 key = (quint64(f.axis) << 56)
        ^ quint64(llround(f.plane * 512.0)) & 0x00ffffffffffffffull;
    groups[key].push_back(fi);
  }

  for (auto it = groups.constBegin(); it != groups.constEnd(); ++it) {
    double uMin = kBig, uMax = -kBig, vMin = kBig, vMax = -kBig;
    for (const int fi: it.value()) {
      const FaceInfo &f = faces[size_t(fi)];
      uMin = std::min(uMin, f.u0);
      uMax = std::max(uMax, f.u1);
      vMin = std::min(vMin, f.v0);
      vMax = std::max(vMax, f.v1);
    }
    const double spanU = uMax - uMin;
    const double spanV = vMax - vMin;
    if (spanU <= 1e-6 || spanV <= 1e-6)
      continue;

    const double cell = std::max(p.gap, std::max(spanU, spanV) / 160.0);
    int nx = std::max(1, int(std::ceil(spanU / cell)));
    int ny = std::max(1, int(std::ceil(spanV / cell)));
    if (nx > 192)
      nx = 192;
    if (ny > 192)
      ny = 192;
    const double cu = spanU / nx;
    const double cv = spanV / ny;

    std::vector<char> covered(size_t(nx) * size_t(ny), 0);
    for (const int fi: it.value()) {
      const FaceInfo &f = faces[size_t(fi)];
      const int i0 = std::clamp(int((f.u0 - uMin) / cu), 0, nx - 1);
      const int i1 = std::clamp(int((f.u1 - uMin) / cu), 0, nx - 1);
      const int j0 = std::clamp(int((f.v0 - vMin) / cv), 0, ny - 1);
      const int j1 = std::clamp(int((f.v1 - vMin) / cv), 0, ny - 1);
      for (int j = j0; j <= j1; ++j)
        for (int i = i0; i <= i1; ++i)
          covered[size_t(j) * size_t(nx) + size_t(i)] = 1;
    }

    // Drain the empty cells touching the border; empty cells left are enclosed openings.
    std::vector<char> outside(size_t(nx) * size_t(ny), 0);
    std::vector<int> stack;
    auto pushIfEmpty = [&](const int x, const int y) {
      if (x < 0 || y < 0 || x >= nx || y >= ny)
        return;
      const size_t k = size_t(y) * size_t(nx) + size_t(x);
      if (outside[k] || covered[k])
        return;
      outside[k] = 1;
      stack.push_back(int(k));
    };
    for (int x = 0; x < nx; ++x) {
      pushIfEmpty(x, 0);
      pushIfEmpty(x, ny - 1);
    }
    for (int y = 0; y < ny; ++y) {
      pushIfEmpty(0, y);
      pushIfEmpty(nx - 1, y);
    }
    while (!stack.empty()) {
      const size_t k = size_t(stack.back());
      stack.pop_back();
      const int x = int(k % size_t(nx));
      const int y = int(k / size_t(nx));
      pushIfEmpty(x + 1, y);
      pushIfEmpty(x - 1, y);
      pushIfEmpty(x, y + 1);
      pushIfEmpty(x, y - 1);
    }

    // Each enclosed blob becomes one rect.
    std::vector<char> done(size_t(nx) * size_t(ny), 0);
    for (int y = 0; y < ny; ++y) {
      for (int x = 0; x < nx; ++x) {
        const size_t k = size_t(y) * size_t(nx) + size_t(x);
        if (covered[k] || outside[k] || done[k])
          continue;
        int i0 = x, i1 = x, j0 = y, j1 = y;
        std::vector<int> blob {int(k)};
        done[k] = 1;
        while (!blob.empty()) {
          const size_t c = size_t(blob.back());
          blob.pop_back();
          const int bx = int(c % size_t(nx));
          const int by = int(c / size_t(nx));
          i0 = std::min(i0, bx);
          i1 = std::max(i1, bx);
          j0 = std::min(j0, by);
          j1 = std::max(j1, by);
          auto grow = [&](const int gx, const int gy) {
            if (gx < 0 || gy < 0 || gx >= nx || gy >= ny)
              return;
            const size_t gk = size_t(gy) * size_t(nx) + size_t(gx);
            if (covered[gk] || outside[gk] || done[gk])
              return;
            done[gk] = 1;
            blob.push_back(int(gk));
          };
          grow(bx + 1, by);
          grow(bx - 1, by);
          grow(bx, by + 1);
          grow(bx, by - 1);
        }
        GapRect r;
        const FaceInfo &src = faces[size_t(it.value().front())];
        r.axis = src.axis;
        r.plane = src.plane;
        r.u0 = uMin + double(i0) * cu;
        r.u1 = uMin + double(i1 + 1) * cu;
        r.v0 = vMin + double(j0) * cv;
        r.v1 = vMin + double(j1 + 1) * cv;
        gaps.push_back(r);
      }
    }
  }
  return gaps;
}

int findFace(const std::vector<FaceInfo> &faces, const quint32 seed) {
  for (size_t i = 0; i < faces.size(); ++i)
    if (faces[i].pickId == seed)
      return int(i);
  return -1;
}

// Splits the run into facades; the seed's comes first (growRun seeds its accepted list with it).
std::vector<Facade> facadesForRun(const std::vector<FaceInfo> &faces, const std::vector<int> &run,
                                 const int seedIdx) {
  std::vector<Facade> out;
  for (const int fi: run) {
    const FaceInfo &f = faces[size_t(fi)];
    if (!f.vertical)
      continue;

    Facade *fac = nullptr;
    for (auto &c: out)
      if (c.axis == f.axis && std::fabs(c.plane - f.plane) <= 1e-3)
        fac = &c;
    if (!fac) {
      out.emplace_back();
      fac = &out.back();
      fac->axis = f.axis;
      fac->plane = f.plane;
      fac->normal = f.normal;
      fac->uDir = f.uDir;
      fac->u0 = f.u0;
      fac->u1 = f.u1;
      fac->v0 = f.v0;
      fac->v1 = f.v1;
    } else {
      fac->u0 = std::min(fac->u0, f.u0);
      fac->u1 = std::max(fac->u1, f.u1);
      fac->v0 = std::min(fac->v0, f.v0);
      fac->v1 = std::max(fac->v1, f.v1);
    }
    if (fi == seedIdx)
      fac->seed = true;
    fac->faces.push_back({f.pickId, f.u0, f.u1, f.v0, f.v1});
    if (std::find(fac->objects.begin(), fac->objects.end(), f.objId) == fac->objects.end())
      fac->objects.push_back(f.objId);
  }
  return out;
}

// A floor must sit behind the facade at a height it covers: a plinth's exposed side has only its own brush
// behind it.
bool facadeHasFloorBehind(const Facade &fac, const std::vector<FaceInfo> &faces, const FillParams &p) {
  const double sign = fac.normal[fac.axis] > 0.0 ? 1.0 : -1.0;
  for (const auto &h: faces) {
    if (h.axis != 1 || h.area < p.floorMinArea) {
      continue;
    }
    if (std::min(h.spanU(), h.spanV()) < p.floorMinDepth) {
      continue;
    }
    if (std::find(fac.objects.begin(), fac.objects.end(), h.objId) != fac.objects.end()) {
      continue;
    }
    // Behind the facade plane, on the side its normal points away from.
    if (sign * (fac.plane - double(h.center[fac.axis])) <= 0.0) {
      continue;
    }
    // Under the facade's own band, and running along it.
    if (h.plane < fac.v0 - 1e-3 || h.plane > fac.v1 + 1e-3) {
      continue;
    }
    const double s0 = fac.axis == 0 ? h.v0 : h.u0;
    const double s1 = fac.axis == 0 ? h.v1 : h.u1;
    if (s1 <= fac.u0 + 1e-3 || s0 >= fac.u1 - 1e-3) {
      continue;
    }
    return true;
  }
  return false;
}

} // namespace

namespace {

// The seed's run: coplanar and edge-connected, plus corners when wrap is on. Floors are not consulted: the
// seed may be nowhere near one.
std::vector<int> growRun(const std::vector<FaceInfo> &faces, const PlaneIndex &idx,
                        const int seedIdx, const FillParams &p) {
  // seen = already dequeued; accepted = the answer. A face is seen before it is judged; conflating the two
  // swallows everything coplanar in the map.
  std::vector<char> seen(faces.size(), 0);
  std::vector<int> accepted;
  std::vector<int> queue {seedIdx};
  seen[size_t(seedIdx)] = 1;
  accepted.push_back(seedIdx);

  std::vector<int> cand;
  while (!queue.empty()) {
    const int cur = queue.back();
    queue.pop_back();
    const FaceInfo &f = faces[size_t(cur)];

    cand.clear();
    collectInPlaneRange(idx.slot[axisSlot(f.axis)], f.plane - p.planeTol, f.plane + p.planeTol, cand);
    if (p.wrap) {
      // u runs along the horizontal axis in the face, so a corner wall's plane sits on one of the u ends.
      const int perpSlot = f.axis == 0 ? 1 : 0;
      collectInPlaneRange(idx.slot[perpSlot], f.u0 - p.gap, f.u0 + p.gap, cand);
      collectInPlaneRange(idx.slot[perpSlot], f.u1 - p.gap, f.u1 + p.gap, cand);
    }

    if (fillVerbose())
      qDebug().noquote() << QStringLiteral(
          "[grow] pop obj=%1 face=%2 axis=%3 plane=%4 v=[%5, %6] coplanarCand=%7")
          .arg(f.objId).arg(f.face).arg(f.axis).arg(f.plane * 10.0, 0, 'f', 2)
          .arg(f.v0 * 10.0, 0, 'f', 1).arg(f.v1 * 10.0, 0, 'f', 1).arg(cand.size());

    for (const int other: cand) {
      if (seen[size_t(other)])
        continue;
      const FaceInfo &g = faces[size_t(other)];
      if (!g.vertical || isTrimFace(g, p))
        continue;

      const bool sameWall = g.axis == f.axis && std::fabs(g.plane - f.plane) <= p.planeTol
          && glm::dot(g.normal, f.normal) > 0.0 && rectsTouch(f, g, p.gap);
      const bool aroundCorner = p.wrap && g.axis != f.axis && cornerTouch(f, g, p.gap);
      if (!sameWall && !aroundCorner) {
        // Only log near misses: same axis, close plane.
        if (fillVerbose() && g.axis == f.axis && std::fabs(g.plane - f.plane) <= 20.0) {
          const double ou = overlapLen(f.u0, f.u1, g.u0, g.u1);
          const double ov = overlapLen(f.v0, f.v1, g.v0, g.v1);
          qDebug().noquote() << QStringLiteral(
              "[grow]   miss obj=%1 face=%2 plane=%3 v=[%4, %5] dPlane=%6 ou=%7 ov=%8 gap=%9 dot=%10")
              .arg(g.objId).arg(g.face).arg(g.plane * 10.0, 0, 'f', 2)
              .arg(g.v0 * 10.0, 0, 'f', 1).arg(g.v1 * 10.0, 0, 'f', 1)
              .arg(std::fabs(g.plane - f.plane) * 10.0, 0, 'f', 3)
              .arg(ou * 10.0, 0, 'f', 2).arg(ov * 10.0, 0, 'f', 3)
              .arg(p.gap * 10.0, 0, 'f', 2)
              .arg(glm::dot(g.normal, f.normal), 0, 'f', 2);
        }
        continue;
      }

      seen[other] = 1;
      accepted.push_back(other);
      // Without this the flood stops after one hop.
      queue.push_back(other);
    }
  }
  return accepted;
}

// Floor-sized horizontal faces, gathered once for the storey search.
std::vector<int> slabsOf(const std::vector<FaceInfo> &faces, const FillParams &p) {
  std::vector<int> out;
  for (size_t i = 0; i < faces.size(); ++i) {
    const FaceInfo &h = faces[i];
    if (h.axis == 1 && h.area >= p.floorMinArea)
      out.push_back(int(i));
  }
  return out;
}

// Storey planes behind the run, against the run's whole footprint: a wall face's thin XZ rect can miss a slab
// that stops slightly short of the wall.
std::vector<double> floorsForRun(const std::vector<FaceInfo> &faces, const std::vector<int> &run,
                                const std::vector<int> &slabs, const FillParams &p,
                                const glm::dvec3 &runMin, const glm::dvec3 &runMax) {
  std::vector<double> out;
  for (const int fi: run) {
    const FaceInfo &f = faces[size_t(fi)];
    for (const int si: slabs) {
      const FaceInfo &h = faces[size_t(si)];
      auto reject = [&](const char *why) {
        if (fillVerbose())
          qDebug().noquote() << QStringLiteral("[floor]   x obj=%1 y=%2 (%3)")
                                   .arg(h.objId).arg(h.plane * 10.0, 0, 'f', 1)
                                   .arg(QLatin1String(why));
      };
      if (glm::dot(f.center - h.center, f.normal) <= 0.0) {
        reject("not behind the face");
        continue;
      }
      if (h.plane < f.v0 - p.floorProbe || h.plane > f.v1 + p.floorProbe) {
        reject("outside probe range");
        continue;
      }
      // Depth along the wall's normal: a floor runs back into the building, a wall's top face does not.
      const double depth = f.axis == 0 ? h.spanU() : h.spanV();
      if (depth < p.floorMinDepth) {
        reject("too shallow");
        continue;
      }
      // The slab abuts the wall's inside, not the picked outside face: grow the footprint along the normal by more
      // than the wall thickness. The behind test keeps the wrong side out.
      const double grow = p.floorMinDepth + 1.0;
      const double xLo = runMin.x - (f.axis == 0 ? grow : 0.0);
      const double xHi = runMax.x + (f.axis == 0 ? grow : 0.0);
      const double zLo = runMin.z - (f.axis == 2 ? grow : 0.0);
      const double zHi = runMax.z + (f.axis == 2 ? grow : 0.0);
      if (h.u1 < xLo - p.gap || h.u0 > xHi + p.gap) {
        reject("no X overlap with run");
        continue;
      }
      if (h.v1 < zLo - p.gap || h.v0 > zHi + p.gap) {
        reject("no Z overlap with run");
        continue;
      }
      out.push_back(h.plane);
      if (fillVerbose())
        qDebug().noquote() << QStringLiteral("[floor]   + obj=%1 y=%2 depth=%3")
                               .arg(h.objId).arg(h.plane * 10.0, 0, 'f', 1)
                               .arg(depth * 10.0, 0, 'f', 1);
    }
  }
  return out;
}

} // namespace

FillResult solveWallFill(const std::vector<FaceInfo> &faces, const quint32 seed, const FillParams &p) {
  FillResult res;

  const int seedIdx = findFace(faces, seed);
  if (seedIdx < 0) {
    res.note = QStringLiteral("seed face not found");
    return res;
  }
  if (!faces[size_t(seedIdx)].vertical) {
    res.note = QStringLiteral("seed face is not a wall face");
    return res;
  }

  const PlaneIndex idx = buildPlaneIndex(faces);
  const std::vector<int> accepted = growRun(faces, idx, seedIdx, p);

  res.faces.reserve(accepted.size());
  for (const int fi: accepted)
    res.faces.push_back(faces[size_t(fi)].pickId);
  res.facades = facadesForRun(faces, accepted, seedIdx);

  res.gaps = detectGaps(faces, accepted, p);
  res.ok = !res.faces.empty();
  if (!res.ok)
    res.note = QStringLiteral("run produced no faces");
  return res;
}

FillResult solveFacadeFill(const std::vector<FaceInfo> &faces, const quint32 seed, const FillParams &p) {
  FillResult res;

  const int seedIdx = findFace(faces, seed);
  if (seedIdx < 0) {
    res.note = QStringLiteral("seed face not found");
    return res;
  }
  if (!faces[size_t(seedIdx)].vertical) {
    res.note = QStringLiteral("seed face is not a wall face");
    return res;
  }

  const PlaneIndex idx = buildPlaneIndex(faces);
  std::vector<int> run = growRun(faces, idx, seedIdx, p);
  if (run.empty()) {
    res.note = QStringLiteral("run produced no faces");
    return res;
  }
  const int grownSize = int(run.size());

  double x0 = kBig, x1 = -kBig, z0 = kBig, z1 = -kBig, lo = kBig, hi = -kBig;
  for (const int fi: run) {
    const FaceInfo &f = faces[size_t(fi)];
    const glm::dvec3 mn = f.xzMin();
    const glm::dvec3 mx = f.xzMax();
    x0 = std::min(x0, mn.x);
    x1 = std::max(x1, mx.x);
    z0 = std::min(z0, mn.z);
    z1 = std::max(z1, mx.z);
    lo = std::min(lo, f.v0);
    hi = std::max(hi, f.v1);
  }
  const glm::dvec3 runMin(x0, lo, z0);
  const glm::dvec3 runMax(x1, hi, z1);

  // Storeys come from the whole run, so a seed between two floors still finds them.
  const std::vector<int> slabs = p.detectFloors ? slabsOf(faces, p) : std::vector<int> {};
  std::vector<double> floors = p.detectFloors
                                  ? floorsForRun(faces, run, slabs, p, runMin, runMax)
                                  : std::vector<double> {};
  clusterYs(floors, std::max(0.25, p.gap * 2.0));
  for (const double y: floors)
    if (y >= lo - 1e-3 && y <= hi + 1e-3)
      res.levelYs.push_back(y);
  res.floorsFromDetection = res.levelYs.size() >= 2;

  // The floors' footprint tells outside from inside.
  glm::dvec2 envCenter {};
  int envCount = 0;
  for (const double y: res.levelYs) {
    for (const int si: slabs) {
      const FaceInfo &h = faces[size_t(si)];
      if (std::fabs(h.plane - y) > 1e-3)
        continue;
      const glm::dvec3 mn = h.xzMin();
      const glm::dvec3 mx = h.xzMax();
      envCenter += glm::dvec2((mn.x + mx.x) * 0.5, (mn.z + mx.z) * 0.5);
      ++envCount;
    }
  }
  if (envCount > 0) {
    envCenter /= double(envCount);
  }

  // Interior cull, only with a floor envelope and only if it leaves more than the seed (else the envelope is wrong).
  int droppedNotClear = 0;
  int droppedFacingIn = 0;
  if (envCount > 0 && run.size() > 1) {
    std::vector<int> outside;
    outside.reserve(run.size());
    for (const int fi: run) {
      if (fi == seedIdx)
        continue;
      const FaceInfo &g = faces[size_t(fi)];
      if (!frontClear(g, faces, p.outsideClear)) {
        ++droppedNotClear;
        if (fillVerbose())
          qDebug().noquote() << QStringLiteral("[facade]   - obj=%1 face=%2 blocked in front at %3u")
                                 .arg(g.objId).arg(g.face).arg(p.outsideClear * 10.0, 0, 'f', 1);
        continue;
      }
      if (!facesOutside(g, envCenter)) {
        ++droppedFacingIn;
        if (fillVerbose())
          qDebug().noquote() << QStringLiteral("[facade]   - obj=%1 face=%2 faces inward (interior)")
                                 .arg(g.objId).arg(g.face);
        continue;
      }
      outside.push_back(fi);
    }
    if (!outside.empty()) {
      outside.push_back(seedIdx);
      run = outside;
    } else if (fillVerbose()) {
      qDebug() << "[facade] cull left nothing, envelope rejected, keeping the whole run";
    }
  }

  // Layers are the bands the slabs cut the run into; anything above the top slab or below the bottom one is its
  // own band (parapet, plinth). Without floors the bands come from the wall's own horizontal seams.
  res.bounds.clear();
  if (res.floorsFromDetection) {
    if (res.levelYs.front() > lo + 1e-3)
      res.bounds.push_back(lo);
    for (const double y: res.levelYs)
      res.bounds.push_back(y);
    if (res.levelYs.back() < hi - 1e-3)
      res.bounds.push_back(hi);
  } else {
    std::vector<double> seams;
    seams.reserve(run.size() * 2);
    for (const int fi: run) {
      seams.push_back(faces[size_t(fi)].v0);
      seams.push_back(faces[size_t(fi)].v1);
    }
    clusterYs(seams, std::max(0.25, p.gap * 2.0));
    for (const double y: seams)
      if (y >= lo - 1e-3 && y <= hi + 1e-3)
        res.bounds.push_back(y);
  }
  if (res.bounds.size() < 2) {
    res.bounds.clear();
    res.bounds.push_back(lo);
    res.bounds.push_back(hi);
  }

  // Storey bands vs the void between one floor's ceiling and the next floor.
  const auto intervals = slabIntervals(faces, p);
  double tallest = 0.0;
  for (size_t i = 0; i + 1 < res.bounds.size(); ++i)
    tallest = std::max(tallest, res.bounds[i + 1] - res.bounds[i]);

  bool anyIntervalMatched = false;
  for (size_t i = 0; i + 1 < res.bounds.size(); ++i) {
    for (const auto &iv: intervals) {
      if (std::fabs(iv.first - res.bounds[i]) < 0.05 && std::fabs(iv.second - res.bounds[i + 1]) < 0.05) {
        anyIntervalMatched = true;
        break;
      }
    }
  }

  res.layerKinds.clear();
  for (size_t i = 0; i + 1 < res.bounds.size(); ++i) {
    const double h = res.bounds[i + 1] - res.bounds[i];
    LayerKind kind = LayerKind::Wall;
    if (anyIntervalMatched) {
      for (const auto &iv: intervals) {
        if (std::fabs(iv.first - res.bounds[i]) < 0.05 && std::fabs(iv.second - res.bounds[i + 1]) < 0.05) {
          kind = LayerKind::Space;
          break;
        }
      }
    } else if (tallest > 1e-6 && h < 0.45 * tallest) {
      // No slabs paired up: a band far shorter than the tallest is not a storey.
      kind = LayerKind::Space;
    }
    res.layerKinds.push_back(kind);
  }

  // Dotted where a face spans across the seam: the layer wants a cut the mesh lacks.
  res.boundCutsMesh.assign(res.bounds.size(), 0);
  for (size_t i = 0; i < res.bounds.size(); ++i) {
    const double b = res.bounds[i];
    for (const int fi: run) {
      const FaceInfo &f = faces[size_t(fi)];
      if (f.v0 < b - 0.01 && f.v1 > b + 0.01) {
        res.boundCutsMesh[i] = 1;
        break;
      }
    }
  }

  // Second pass with the storeys known: drop faces that do not span their storey (frames, trim).
  const size_t before = run.size();
  std::vector<int> spanning;
  spanning.reserve(before);
  for (const int fi: run) {
    if (fi == seedIdx || spansStorey(faces[size_t(fi)], res.bounds, p.storeySpanFrac)) {
      spanning.push_back(fi);
    } else {
      const FaceInfo &g = faces[size_t(fi)];
      if (fillVerbose())
        qDebug().noquote() << QStringLiteral(
          "[facade]   - obj=%1 face=%2 spans only %3%4 of its storey (trim/frame)")
          .arg(g.objId).arg(g.face)
          .arg(100.0 * g.spanV() / std::max(1e-6, res.bounds.back() - res.bounds.front()), 0, 'f', 1)
          .arg(QStringLiteral("%"));
    }
  }
  if (spanning.size() > 1)
    run = spanning;

  // The ground floor: the lowest slab top at or below the seed. Its brush is a floor, never a facade, even when
  // its sides line up flush with the wall above.
  quint32 groundObj = 0;
  double groundY = 0.0;
  bool groundValid = false;
  const FaceInfo &seedFace = faces[size_t(seedIdx)];
  {
    QHash<quint32, double> topByObj;
    for (const int si: slabs) {
      const FaceInfo &h = faces[size_t(si)];
      if (h.plane > seedFace.v0 + 1e-3)
        continue;
      if (std::min(h.spanU(), h.spanV()) < p.floorMinDepth)
        continue;
      const auto it = topByObj.find(h.objId);
      if (it == topByObj.end() || h.plane > it.value())
        topByObj.insert(h.objId, h.plane);
    }
    for (auto it = topByObj.constBegin(); it != topByObj.constEnd(); ++it) {
      if (!groundValid || it.value() < groundY) {
        groundValid = true;
        groundY = it.value();
        groundObj = it.key();
      }
    }
  }
  res.groundY = groundY;
  res.groundObj = groundObj;
  res.groundFromSeed = groundValid;

  std::vector<int> facadeRun;
  facadeRun.reserve(run.size());
  int droppedGround = 0;
  for (const int fi: run) {
    const FaceInfo &f = faces[size_t(fi)];
    if (fi != seedIdx && groundValid && (f.objId == groundObj || f.v1 <= groundY + 1e-3)) {
      ++droppedGround;
      if (fillVerbose())
        qDebug().noquote() << QStringLiteral(
            "[ground]   - obj=%1 face=%2 is the ground floor slab (top y=%3)")
                                 .arg(f.objId).arg(f.face).arg(groundY * 10.0, 0, 'f', 1);
      continue;
    }
    facadeRun.push_back(fi);
  }
  if (facadeRun.empty())
    facadeRun = run;

  res.gaps = detectGaps(faces, facadeRun, p);

  if (fillVerbose()) {
    const FaceInfo &sf = faces[size_t(seedIdx)];
    qDebug().noquote() << QStringLiteral(
        "[facade] seed obj=%1 face=%2 axis=%3 plane=%4 v=[%5, %6] area=%7")
        .arg(sf.objId).arg(sf.face).arg(sf.axis)
        .arg(sf.plane * 10.0, 0, 'f', 1).arg(sf.v0 * 10.0, 0, 'f', 1)
        .arg(sf.v1 * 10.0, 0, 'f', 1).arg(sf.area * 100.0, 0, 'f', 1);
    qDebug().noquote() << QStringLiteral("[facade] grown run = %1 face(s)").arg(grownSize);

    QStringList fy;
    for (const double y: res.levelYs)
      fy << QString::number(y * 10.0, 'f', 1);
    qDebug().noquote() << QStringLiteral("[facade] floors = %1  y=[%2]  (from %3 slab(s) over the run)")
                             .arg(res.levelYs.size()).arg(fy.join(QStringLiteral(", "))).arg(slabs.size());

    QStringList bands;
    for (size_t i = 0; i < res.layerKinds.size(); ++i) {
      const double h = (res.bounds[i + 1] - res.bounds[i]) * 10.0;
      bands << QStringLiteral("%1:%2(%3)")
                   .arg(i)
                   .arg(res.layerKinds[i] == LayerKind::Space ? QStringLiteral("space")
                                                             : QStringLiteral("wall"))
                   .arg(h, 0, 'f', 0);
    }
    QStringList cutSeams;
    for (size_t i = 0; i < res.boundCutsMesh.size(); ++i)
      if (res.boundCutsMesh[i])
        cutSeams << QString::number(res.bounds[i] * 10.0, 'f', 0);
    qDebug().noquote() << QStringLiteral("[facade] layers from %1  bands=[%2]  seams cutting mesh=[%3]")
                             .arg(res.floorsFromDetection ? QStringLiteral("floors")
                                                          : QStringLiteral("wall geometry"))
                             .arg(bands.join(QStringLiteral(", ")))
                             .arg(cutSeams.join(QStringLiteral(", ")));
    qDebug().noquote() << QStringLiteral(
        "[facade] envelope centre = (%1, %2)  culled: %3 blocked in front, %4 facing inward")
        .arg(envCenter.x * 10.0, 0, 'f', 1).arg(envCenter.y * 10.0, 0, 'f', 1)
        .arg(droppedNotClear).arg(droppedFacingIn);
    qDebug().noquote() << QStringLiteral("[facade] storey-span dropped %1 of %2 face(s)")
                             .arg(before - run.size()).arg(before);
    qDebug().noquote() << QStringLiteral("[ground] seed y=%1 -> ground y=%2 obj=%3, %4 face(s) dropped")
                             .arg(seedFace.v0 * 10.0, 0, 'f', 1)
                             .arg(groundValid ? QString::number(groundY * 10.0, 'f', 1) : QStringLiteral("?"))
                             .arg(groundObj)
                             .arg(droppedGround);
    for (const int fi: run) {
      const FaceInfo &f = faces[size_t(fi)];
      qDebug().noquote() << QStringLiteral(
          "[facade]   + obj=%1 face=%2 axis=%3 plane=%4 u=[%5, %6] v=[%7, %8] area=%9")
          .arg(f.objId).arg(f.face).arg(f.axis).arg(f.plane * 10.0, 0, 'f', 1)
          .arg(f.u0 * 10.0, 0, 'f', 1).arg(f.u1 * 10.0, 0, 'f', 1)
          .arg(f.v0 * 10.0, 0, 'f', 1).arg(f.v1 * 10.0, 0, 'f', 1)
          .arg(f.area * 100.0, 0, 'f', 1);
    }
  }

  res.faces.reserve(facadeRun.size());
  for (const int fi: facadeRun)
    res.faces.push_back(faces[size_t(fi)].pickId);
  res.facades = facadesForRun(faces, facadeRun, seedIdx);

  // Drop plane groups that are not walls: only with detected storeys (the floor-behind test needs slabs),
  // never the seed's facade.
  if (res.floorsFromDetection && res.facades.size() > 1) {
    std::vector<Facade> walls;
    QStringList dropped;
    for (const auto &fac: res.facades) {
      if (fac.seed || facadeHasFloorBehind(fac, faces, p)) {
        walls.push_back(fac);
      } else {
        dropped << QStringLiteral("axis%1@%2 u=[%3, %4] v=[%5, %6] brushes=%7")
                       .arg(fac.axis)
                       .arg(fac.plane * 10.0, 0, 'f', 1)
                       .arg(fac.u0 * 10.0, 0, 'f', 1)
                       .arg(fac.u1 * 10.0, 0, 'f', 1)
                       .arg(fac.v0 * 10.0, 0, 'f', 1)
                       .arg(fac.v1 * 10.0, 0, 'f', 1)
                       .arg(fac.objects.size());
      }
    }
    if (!walls.empty()) {
      if (fillVerbose() && !dropped.isEmpty())
        qDebug().noquote() << QStringLiteral("[facade] not walls (no floor behind): %1")
                                 .arg(dropped.join(QStringLiteral("; ")));
      res.facades = walls;
    }
  }

  if (fillVerbose()) {
    QStringList facs;
    for (const auto &fc: res.facades)
      facs << QStringLiteral("axis%1@%2 u=[%3, %4] n=%5,%6 faces=%7%8")
                  .arg(fc.axis)
                  .arg(fc.plane * 10.0, 0, 'f', 1)
                  .arg(fc.u0 * 10.0, 0, 'f', 1)
                  .arg(fc.u1 * 10.0, 0, 'f', 1)
                  .arg(fc.normal.x, 0, 'f', 0)
                  .arg(fc.normal.z, 0, 'f', 0)
                  .arg(fc.faces.size())
                  .arg(fc.seed ? QStringLiteral(" seed") : QString());
    qDebug().noquote() << QStringLiteral("[facade] facades = %1: %2")
                             .arg(res.facades.size()).arg(facs.join(QStringLiteral("; ")));
  }

  res.ok = !res.faces.empty();
  if (!res.ok)
    res.note = QStringLiteral("no facade faces qualified");
  return res;
}

} // namespace eng
