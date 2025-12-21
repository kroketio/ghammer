#include "mesh.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <unordered_map>

namespace eng {

namespace {

void orthogonalize(glm::vec3 &v, const glm::vec3 &reference) {
  v -= reference * glm::dot(v, reference);
  const float len = glm::length(v);
  if (len > 1e-8f)
    v /= len;
}

} // namespace

Mesh::Mesh() = default;
Mesh::~Mesh() = default;

void Mesh::bumpGeometry() {
  // Globally unique, not per mesh: two meshes built the same way would share a version, and swapping them on an
  // object (previews, modifier output) would look unchanged to the GPU pools and caches.
  m_geometryVersion = ++s_geometryEpoch;
  m_wireDirty = true;
  // Faces never share vertices.
  const int faces = faceCount();
  for (int f = 0; f < faces; ++f) {
    const uint32_t start = faceIndexStart(f), cnt = faceIndexCount(f);
    for (uint32_t i = start; i < start + cnt && i < m_indices.size(); ++i)
      if (m_indices[i] < m_vertices.size())
        m_vertices[m_indices[i]].faceIndex = float(f);
  }
}

void Mesh::bumpFaces() {
  m_faceVersion = ++s_faceEpoch;
}

void Mesh::ensureFaceArrays(const int faceCount) {
  const int needed = faceCount + 1;
  if (needed <= static_cast<int>(m_faceMaterials.size()))
    return;

  m_faceMaterials.resize(needed);
  m_faceTexBases.resize(needed);
  m_facePickingIDs.resize(needed, 0u);
  m_faceGroups.resize(needed);
}

void Mesh::setFaceTexBasis(const int faceIndex, const FaceTexBasis &basis) {
  if (faceIndex < 0)
    return;
  ensureFaceArrays(faceIndex);
  m_faceTexBases[faceIndex] = basis;
  bumpFaces();
}

const FaceTexBasis &Mesh::faceTexBasis(const int faceIndex) const {
  static const FaceTexBasis kFallback {};
  if (faceIndex >= 0 && faceIndex < static_cast<int>(m_faceTexBases.size()))
    return m_faceTexBases[faceIndex];
  return kFallback;
}

QSharedPointer<Material> Mesh::faceMaterial(const int faceIndex) const {
  if (faceIndex >= 0 && faceIndex < static_cast<int>(m_faceMaterials.size()))
    return m_faceMaterials[faceIndex];
  return nullptr;
}

void Mesh::setFaceMaterial(const int faceIndex, const QSharedPointer<Material> &material) {
  if (faceIndex < 0)
    return;
  ensureFaceArrays(faceIndex);
  m_faceMaterials[faceIndex] = material;
  bumpFaces();
}

void Mesh::setFacePickingID(const int faceIndex, const uint32_t id) {
  if (faceIndex < 0)
    return;
  ensureFaceArrays(faceIndex);
  m_facePickingIDs[faceIndex] = id;
  bumpFaces();
}

uint32_t Mesh::getFacePickingID(const int faceIndex) const {
  if (faceIndex >= 0 && faceIndex < static_cast<int>(m_facePickingIDs.size()))
    return m_facePickingIDs[faceIndex];
  return 0;
}

void Mesh::setFaceGroups(const int faceIndex, const FaceGroupSet &groups) {
  if (faceIndex < 0)
    return;
  ensureFaceArrays(faceIndex);
  if (m_faceGroups[faceIndex] == groups)
    return;
  m_faceGroups[faceIndex] = groups;
  ++m_groupVersion;
}

const FaceGroupSet &Mesh::faceGroups(const int faceIndex) const {
  static const FaceGroupSet kEmpty;
  if (faceIndex >= 0 && faceIndex < static_cast<int>(m_faceGroups.size()))
    return m_faceGroups[faceIndex];
  return kEmpty;
}

void Mesh::addFaceGroup(const int faceIndex, const quint32 id) {
  if (faceIndex < 0 || id == 0)
    return;
  FaceGroupSet set = faceGroups(faceIndex);
  facegroup::insert(set, id);
  setFaceGroups(faceIndex, set);
}

void Mesh::removeFaceGroup(const int faceIndex, const quint32 id) {
  if (faceIndex < 0 || faceIndex >= static_cast<int>(m_faceGroups.size()))
    return;
  FaceGroupSet set = m_faceGroups[faceIndex];
  facegroup::erase(set, id);
  setFaceGroups(faceIndex, set);
}

void Mesh::addVertices(const std::vector<Vertex> &vertices) {
  // VMF UVs are world-derived; never clamp them or tiling and offsets break.
  m_vertices = vertices;
  bumpGeometry();
}

void Mesh::addIndices(const std::vector<uint32_t> &indices) {
  m_indices = indices;
  ensureFaceArrays(faceCount() - 1);
  bumpGeometry();
}

// Tangent space from the UV mapping, so swaps, flips and negative scales in the VMF axes end up in the TBN.
void Mesh::rebuildTangents() {
  for (auto &v: m_vertices) {
    v.tangent = glm::vec3(0.0f);
    v.bitangent = glm::vec3(0.0f);
  }

  const size_t triCount = m_indices.size() / 3;
  for (size_t t = 0; t < triCount; ++t) {
    Vertex &v0 = m_vertices[m_indices[t * 3 + 0]];
    Vertex &v1 = m_vertices[m_indices[t * 3 + 1]];
    Vertex &v2 = m_vertices[m_indices[t * 3 + 2]];

    const glm::vec3 e1 = v1.position - v0.position;
    const glm::vec3 e2 = v2.position - v0.position;
    const glm::vec2 d1 = v1.texCoord - v0.texCoord;
    const glm::vec2 d2 = v2.texCoord - v0.texCoord;

    const float denom = d1.x * d2.y - d2.x * d1.y;
    if (std::fabs(denom) < 1e-9f)
      continue;

    const float f = 1.0f / denom;
    const glm::vec3 tangent = f * (d2.y * e1 - d1.y * e2);
    const glm::vec3 bitangent = f * (d1.x * e2 - d2.x * e1);
    if (!std::isfinite(tangent.x) || !std::isfinite(bitangent.x))
      continue;

    for (Vertex *v: {&v0, &v1, &v2}) {
      v->tangent += tangent;
      v->bitangent += bitangent;
    }
  }

  for (auto &v: m_vertices) {
    if (glm::dot(v.tangent, v.tangent) < 1e-12f) {
      // Degenerate UVs: pick any orthonormal pair so the TBN stays invertible.
      const glm::vec3 helper = std::fabs(v.normal.z) < 0.9f ? glm::vec3(0.0f, 0.0f, 1.0f)
                                                          : glm::vec3(1.0f, 0.0f, 0.0f);
      v.tangent = glm::normalize(glm::cross(helper, v.normal));
      v.bitangent = glm::normalize(glm::cross(v.normal, v.tangent));
      continue;
    }
    orthogonalize(v.tangent, v.normal);
    if (glm::dot(v.tangent, v.tangent) < 1e-12f)
      v.tangent = glm::vec3(1.0f, 0.0f, 0.0f);
    orthogonalize(v.bitangent, v.normal);
    orthogonalize(v.bitangent, v.tangent);
    if (glm::dot(v.bitangent, v.bitangent) < 1e-12f)
      v.bitangent = glm::normalize(glm::cross(v.normal, v.tangent));
  }
}

std::shared_ptr<Mesh> Mesh::createFromCubeSides(const std::array<Side, 6> &sides) {
  auto mesh = std::make_shared<Mesh>();
  std::vector<Vertex> vertices;
  std::vector<uint32_t> indices;
  vertices.reserve(24);
  indices.reserve(36);

  for (int face = 0; face < 6; ++face) {
    const auto &s = sides[face];

    const glm::vec3 uVec(s.uaxis[0], s.uaxis[1], s.uaxis[2]);
    const glm::vec3 vVec(s.vaxis[0], s.vaxis[1], s.vaxis[2]);

    const glm::vec3 p0(s.vertices_plus[0].x(), s.vertices_plus[0].y(), s.vertices_plus[0].z());
    const glm::vec3 p1(s.vertices_plus[1].x(), s.vertices_plus[1].y(), s.vertices_plus[1].z());
    const glm::vec3 p2(s.vertices_plus[2].x(), s.vertices_plus[2].y(), s.vertices_plus[2].z());

    glm::vec3 normal = glm::cross(p1 - p0, p2 - p0);
    const float normalLen = glm::length(normal);
    normal = normalLen > 1e-8f ? normal / normalLen : glm::vec3(0.0f, 1.0f, 0.0f);

    const uint32_t base = static_cast<uint32_t>(vertices.size());
    for (int i = 0; i < 4; ++i) {
      const glm::vec3 pos(s.vertices_plus[i].x(), s.vertices_plus[i].y(), s.vertices_plus[i].z());
      // The attribute is the raw projection onto the texture axes (the shader derives UVs from the basis), so the
      // fitted tangent / bitangent align with uAxis / vAxis and normal maps follow the texture, not the winding.
      const float u = glm::dot(pos, uVec);
      const float v = glm::dot(pos, vVec);
      vertices.push_back({pos, normal, glm::vec2(u, v), glm::vec3(0), glm::vec3(0)});
    }

    indices.insert(indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
  }

  mesh->m_vertices = std::move(vertices);
  mesh->m_indices = std::move(indices);
  mesh->ensureFaceArrays(5);
  mesh->rebuildTangents();
  mesh->bumpGeometry();
  return mesh;
}

// Corners in perimeter order from edge adjacency: of the two triangles' six edges, the one seen twice is the
// diagonal, the four seen once the perimeter. Index order is NOT perimeter order (it gives a bowtie).
bool Mesh::facePerimeter(const int faceIndex, std::array<uint32_t, 4> &outVerts) const {
  if (faceIndex < 0)
    return false;

  if (faceIndex >= faceCount() || faceIndexCount(faceIndex) != 6)
    return false;
  const size_t start = faceIndexStart(faceIndex);
  if (start + 6 > m_indices.size())
    return false;

  struct UndirectedEdge {
    uint32_t a;
    uint32_t b;
    int count;
  };

  std::vector<UndirectedEdge> edges;
  for (size_t t = 0; t < 2; ++t) {
    const uint32_t tri[3] = {m_indices[start + t * 3],
                            m_indices[start + t * 3 + 1],
                            m_indices[start + t * 3 + 2]};
    for (int e = 0; e < 3; ++e) {
      const uint32_t lo = std::min(tri[e], tri[(e + 1) % 3]);
      const uint32_t hi = std::max(tri[e], tri[(e + 1) % 3]);
      bool seen = false;
      for (auto &edge: edges) {
        if (edge.a == lo && edge.b == hi) {
          ++edge.count;
          seen = true;
          break;
        }
      }
      if (!seen)
        edges.push_back({lo, hi, 1});
    }
  }

  std::vector<std::pair<uint32_t, uint32_t>> boundary;
  for (const auto &edge: edges) {
    if (edge.count == 1)
      boundary.emplace_back(edge.a, edge.b);
  }
  if (boundary.size() != 4)
    return false;

  std::vector<uint32_t> loop;
  loop.push_back(boundary[0].first);
  loop.push_back(boundary[0].second);
  while (loop.size() < 4) {
    const uint32_t cur = loop.back();
    const uint32_t prev = loop[loop.size() - 2];
    bool advanced = false;
    for (const auto &e: boundary) {
      uint32_t next = 0;
      if (e.first == cur)
        next = e.second;
      else if (e.second == cur)
        next = e.first;
      else
        continue;
      if (next == prev)
        continue;
      loop.push_back(next);
      advanced = true;
      break;
    }
    if (!advanced)
      return false;
  }

  for (int i = 0; i < 4; ++i) {
    for (int j = i + 1; j < 4; ++j) {
      if (loop[i] == loop[j])
        return false;
    }
  }

  const uint32_t closeLo = std::min(loop[3], loop[0]);
  const uint32_t closeHi = std::max(loop[3], loop[0]);
  bool closes = false;
  for (const auto &e: boundary) {
    if (e.first == closeLo && e.second == closeHi) {
      closes = true;
      break;
    }
  }
  if (!closes)
    return false;

  for (int k = 0; k < 4; ++k) {
    if (loop[k] >= m_vertices.size())
      return false;
  }

  outVerts = {loop[0], loop[1], loop[2], loop[3]};
  return true;
}

void Mesh::setFaceRanges(std::vector<uint32_t> first, std::vector<uint32_t> count) {
  if (first.size() != count.size())
    return;
  bool legacy = !first.empty() && first.size() * 6 == m_indices.size();
  for (size_t i = 0; legacy && i < first.size(); ++i)
    legacy = first[i] == i * 6 && count[i] == 6;
  if (legacy) {
    m_faceFirst.clear();
    m_faceCount.clear();
  } else {
    m_faceFirst = std::move(first);
    m_faceCount = std::move(count);
  }
  ensureFaceArrays(faceCount() - 1);
  bumpGeometry();
}

bool Mesh::getFacePolygon(const int faceIndex, std::vector<glm::vec3> &out) const {
  out.clear();
  if (faceIndex < 0 || faceIndex >= faceCount())
    return false;
  const uint32_t count = faceIndexCount(faceIndex);
  if (count == 6) {
    std::array<uint32_t, 4> q {};
    if (facePerimeter(faceIndex, q)) {
      for (const uint32_t v: q)
        out.push_back(m_vertices[v].position);
      return true;
    }
  }
  // Fan: (p0 p1 p2) (p0 p2 p3) ... -> perimeter p0 p1 p2 ... pn.
  const uint32_t start = faceIndexStart(faceIndex);
  if (count < 3 || start + count > m_indices.size())
    return false;
  out.push_back(m_vertices[m_indices[start]].position);
  out.push_back(m_vertices[m_indices[start + 1]].position);
  for (uint32_t t = 0; t < count; t += 3)
    out.push_back(m_vertices[m_indices[start + t + 2]].position);
  return out.size() >= 3;
}

bool Mesh::getFacePlane(const int faceIndex, glm::vec3 &normal, float &distance) const {
  std::vector<glm::vec3> poly;
  if (!getFacePolygon(faceIndex, poly))
    return false;
  // Newell's method: robust for any planar polygon, degenerate corners included.
  glm::dvec3 n(0.0);
  glm::dvec3 c(0.0);
  for (size_t i = 0; i < poly.size(); ++i) {
    const glm::dvec3 a(poly[i]);
    const glm::dvec3 b(poly[(i + 1) % poly.size()]);
    n.x += (a.y - b.y) * (a.z + b.z);
    n.y += (a.z - b.z) * (a.x + b.x);
    n.z += (a.x - b.x) * (a.y + b.y);
    c += a;
  }
  const double len = glm::length(n);
  if (len < 1e-12)
    return false;
  n /= len;
  c /= double(poly.size());
  // Brushes are wound inward (Source), other meshes may not be; brushes are convex, so out is away from the centre.
  glm::dvec3 centre(0.0);
  for (const Vertex &v: m_vertices)
    centre += glm::dvec3(v.position);
  if (!m_vertices.empty() && glm::dot(n, c - centre / double(m_vertices.size())) < 0.0)
    n = -n;
  normal = glm::vec3(n);
  distance = float(glm::dot(n, c));
  return true;
}

std::shared_ptr<Mesh> Mesh::createFromPolygons(const std::vector<std::vector<glm::vec3>> &polygons) {
  auto mesh = std::make_shared<Mesh>();
  std::vector<Vertex> vertices;
  std::vector<uint32_t> indices;
  std::vector<uint32_t> first, count;
  for (const auto &poly: polygons) {
    if (poly.size() < 3)
      continue;
    glm::dvec3 n(0.0);
    for (size_t i = 0; i < poly.size(); ++i) {
      const glm::dvec3 a(poly[i]);
      const glm::dvec3 b(poly[(i + 1) % poly.size()]);
      n.x += (a.y - b.y) * (a.z + b.z);
      n.y += (a.z - b.z) * (a.x + b.x);
      n.z += (a.x - b.x) * (a.y + b.y);
    }
    const double len = glm::length(n);
    const glm::vec3 normal = len > 1e-12 ? glm::vec3(n / len) : glm::vec3(0, 1, 0);
    const uint32_t base = uint32_t(vertices.size());
    for (const auto &p: poly)
      vertices.push_back({p, normal, glm::vec2(0.0f), glm::vec3(0.0f), glm::vec3(0.0f)});
    first.push_back(uint32_t(indices.size()));
    for (uint32_t i = 1; i + 1 < poly.size(); ++i)
      indices.insert(indices.end(), {base, base + i, base + i + 1});
    count.push_back(uint32_t(indices.size()) - first.back());
  }
  mesh->m_vertices = std::move(vertices);
  mesh->m_indices = std::move(indices);
  mesh->m_faceFirst = std::move(first);
  mesh->m_faceCount = std::move(count);
  // Quad-only meshes go back to the legacy layout for the quad-specific paths (auto-fill, the box cut tool).
  bool allQuads = !mesh->m_faceCount.empty();
  for (const uint32_t c: mesh->m_faceCount)
    allQuads = allQuads && c == 6;
  if (allQuads) {
    mesh->m_faceFirst.clear();
    mesh->m_faceCount.clear();
  }
  mesh->ensureFaceArrays(mesh->faceCount() - 1);
  mesh->rebuildTangents();
  mesh->bumpGeometry();
  return mesh;
}

void Mesh::refreshTangentFrame() {
  for (int f = 0; f < faceCount(); ++f) {
    const FaceTexBasis &b = faceTexBasis(f);
    const uint32_t start = faceIndexStart(f);
    const uint32_t cnt = faceIndexCount(f);
    for (uint32_t i = start; i < start + cnt && i < m_indices.size(); ++i) {
      Vertex &v = m_vertices[m_indices[i]];
      v.texCoord = glm::vec2(glm::dot(v.position, b.uAxis), glm::dot(v.position, b.vAxis));
    }
  }
  rebuildTangents();
  bumpGeometry();
}

bool Mesh::getFaceQuad(const int faceIndex, std::array<glm::vec3, 4> &out) const {
  std::array<uint32_t, 4> verts {};
  if (!facePerimeter(faceIndex, verts))
    return false;

  for (int k = 0; k < 4; ++k)
    out[k] = m_vertices[verts[k]].position;
  return true;
}

const std::vector<WireVertex> &Mesh::wireVertices() const {
  if (!m_wireDirty)
    return m_wireVertices;
  m_wireDirty = false;
  rebuildWireframe();
  return m_wireVertices;
}

const std::vector<uint32_t> &Mesh::wireIndices() const {
  wireVertices();
  return m_wireIndices;
}

// Interior quad diagonals (shared by two coplanar triangles) are hidden; non-manifold edges and creases drawn.
void Mesh::rebuildWireframe() const {
  m_wireVertices.clear();
  m_wireIndices.clear();

  const size_t triCount = m_indices.size() / 3;
  if (triCount == 0 || m_vertices.empty())
    return;

  const auto triNormal = [this](const size_t t) -> glm::vec3 {
    const glm::vec3 &p0 = m_vertices[m_indices[t * 3 + 0]].position;
    const glm::vec3 &p1 = m_vertices[m_indices[t * 3 + 1]].position;
    const glm::vec3 &p2 = m_vertices[m_indices[t * 3 + 2]].position;
    const glm::vec3 n = glm::cross(p1 - p0, p2 - p0);
    const float len = glm::length(n);
    return len > 1e-8f ? n / len : glm::vec3(0.0f);
  };

  std::vector<glm::vec3> normals(triCount);
  for (size_t t = 0; t < triCount; ++t)
    normals[t] = triNormal(t);

  using EdgeKey = std::pair<uint32_t, uint32_t>;
  std::map<EdgeKey, std::vector<size_t>> edgeTris;
  for (size_t t = 0; t < triCount; ++t) {
    for (int e = 0; e < 3; ++e) {
      const uint32_t a = m_indices[t * 3 + static_cast<size_t>((e + 1) % 3)];
      const uint32_t b = m_indices[t * 3 + static_cast<size_t>((e + 2) % 3)];
      if (a == b)
        continue;
      edgeTris[EdgeKey(std::min(a, b), std::max(a, b))].push_back(t);
    }
  }

  std::map<EdgeKey, bool> suppressEdge;
  for (const auto &entry: edgeTris) {
    const std::vector<size_t> &tris = entry.second;
    bool suppress = false;
    if (tris.size() == 2)
      suppress = std::fabs(glm::dot(normals[tris[0]], normals[tris[1]])) > COPLANAR_DOT_EPSILON;
    suppressEdge[entry.first] = suppress;
  }

  m_wireVertices.reserve(m_indices.size());
  m_wireIndices.reserve(m_indices.size());

  for (size_t t = 0; t < triCount; ++t) {
    std::array<glm::vec3, 3> bary = {
        glm::vec3(1.0f, 0.0f, 0.0f),
        glm::vec3(0.0f, 1.0f, 0.0f),
        glm::vec3(0.0f, 0.0f, 1.0f)
    };

    for (int e = 0; e < 3; ++e) {
      const uint32_t a = m_indices[t * 3 + static_cast<size_t>((e + 1) % 3)];
      const uint32_t b = m_indices[t * 3 + static_cast<size_t>((e + 2) % 3)];
      if (a == b)
        continue;

      const auto it = suppressEdge.find(EdgeKey(std::min(a, b), std::max(a, b)));
      if (it == suppressEdge.end() || !it->second)
        continue;

      // Pinned to 1 across the triangle, so the edge opposite corner e never rasterises as a line.
      bary[0][e] = 1.0f;
      bary[1][e] = 1.0f;
      bary[2][e] = 1.0f;
    }

    for (int corner = 0; corner < 3; ++corner) {
      const uint32_t srcIdx = m_indices[t * 3 + static_cast<size_t>(corner)];
      if (srcIdx >= m_vertices.size())
        return;
      m_wireVertices.push_back({m_vertices[srcIdx].position, bary[corner], m_vertices[srcIdx].normal});
      m_wireIndices.push_back(static_cast<uint32_t>(m_wireVertices.size() - 1));
    }
  }
}

std::pair<glm::vec3, glm::vec3> Mesh::getAABB() const {
  if (m_vertices.empty())
    return {glm::vec3(0), glm::vec3(0)};
  glm::vec3 minV = m_vertices[0].position;
  glm::vec3 maxV = m_vertices[0].position;
  for (const auto &v: m_vertices) {
    minV = glm::min(minV, v.position);
    maxV = glm::max(maxV, v.position);
  }
  return {minV, maxV};
}

} // namespace eng

namespace eng {

std::vector<std::vector<glm::vec3>> smoothPolygonNormals(const std::vector<std::vector<glm::dvec3>> &polys,
                                                         const std::vector<glm::dvec3> &normals,
                                                         const double angleDeg) {
  const double cosLimit = std::cos(angleDeg * 3.14159265358979323846 / 180.0);
  // Corners by position (to a thousandth of a unit).
  auto key = [](const glm::dvec3 &p) {
    const auto q = [](double c) { return quint64(qint64(std::llround(c * 1000.0)) & 0x1fffff); };
    return q(p.x) | (q(p.y) << 21) | (q(p.z) << 42);
  };
  std::unordered_map<quint64, std::vector<int>> at;
  for (size_t f = 0; f < polys.size(); ++f)
    for (const glm::dvec3 &p: polys[f]) {
      auto &list = at[key(p)];
      if (list.empty() || list.back() != int(f))
        list.push_back(int(f));
    }
  // Area-weighted: a big flat face next to a round stays flat and the round blends into it.
  std::vector<double> area(polys.size(), 0.0);
  for (size_t f = 0; f < polys.size(); ++f)
    for (size_t k = 1; k + 1 < polys[f].size(); ++k)
      area[f] += glm::length(glm::cross(polys[f][k] - polys[f][0], polys[f][k + 1] - polys[f][0])) * 0.5;
  std::vector<std::vector<glm::vec3>> out(polys.size());
  for (size_t f = 0; f < polys.size(); ++f) {
    const glm::dvec3 nf = f < normals.size() ? normals[f] : glm::dvec3(0.0, 1.0, 0.0);
    for (const glm::dvec3 &p: polys[f]) {
      glm::dvec3 sum(0.0);
      for (const int g: at[key(p)])
        if (size_t(g) < normals.size() && glm::dot(normals[size_t(g)], nf) >= cosLimit)
          sum += normals[size_t(g)] * std::max(area[size_t(g)], 1e-9);
      out[f].push_back(glm::vec3(glm::length(sum) > 1e-12 ? glm::normalize(sum) : nf));
    }
  }
  return out;
}

void Mesh::setSmoothShading(const bool smooth) {
  const int faces = faceCount();
  std::vector<std::vector<glm::dvec3>> polys(size_t(std::max(0, faces)));
  std::vector<glm::dvec3> normals(polys.size(), glm::dvec3(0.0, 1.0, 0.0));
  std::vector<glm::vec3> poly;
  for (int f = 0; f < faces; ++f) {
    glm::vec3 n;
    float d = 0.0f;
    if (getFacePolygon(f, poly))
      for (const glm::vec3 &p: poly)
        polys[size_t(f)].push_back(glm::dvec3(p));
    if (getFacePlane(f, n, d))
      normals[size_t(f)] = glm::dvec3(n);
  }
  std::vector<std::vector<glm::vec3>> corners;
  if (smooth) {
    corners = smoothPolygonNormals(polys, normals);
  } else {
    corners.resize(polys.size());
    for (size_t f = 0; f < polys.size(); ++f)
      corners[f].assign(polys[f].size(), glm::vec3(normals[f]));
  }
  setCornerNormals(corners);
}

void Mesh::setCornerNormals(const std::vector<std::vector<glm::vec3>> &faceCorners) {
  std::vector<glm::vec3> poly;
  for (int f = 0; f < faceCount() && size_t(f) < faceCorners.size(); ++f) {
    const auto &cn = faceCorners[size_t(f)];
    if (cn.empty() || !getFacePolygon(f, poly) || poly.size() != cn.size())
      continue;
    const uint32_t start = faceIndexStart(f), count = faceIndexCount(f);
    for (uint32_t i = start; i < start + count && i < m_indices.size(); ++i) {
      Vertex &v = m_vertices[m_indices[i]];
      double best = 1e30;
      for (size_t k = 0; k < poly.size(); ++k) {
        const double d = glm::length(glm::dvec3(poly[k]) - glm::dvec3(v.position));
        if (d < best) {
          best = d;
          v.normal = cn[k];
        }
      }
    }
  }
  rebuildTangents();
  bumpGeometry();
}

} // namespace eng
