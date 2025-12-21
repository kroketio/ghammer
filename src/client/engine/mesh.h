#pragma once

#include <atomic>
#include <array>
#include <string>
#include <vector>

#include <QString>
#include <QSharedPointer>
#include <QVector3D>
#include <glm/glm.hpp>

#include "material.h"
#include "face_groups.h"

namespace eng {

// Tangent/bitangent per vertex give normal maps an exact TBN (no screen-space derivatives).
struct Vertex {
  glm::vec3 position;
  glm::vec3 normal;
  glm::vec2 texCoord;
  glm::vec3 tangent;
  glm::vec3 bitangent;
  // Mesh-local face index (filled by Mesh): lets the shader find face data when faces draw in merged runs.
  float faceIndex = 0.0f;
};

struct WireVertex {
  glm::vec3 position;
  glm::vec3 barycentric;
  glm::vec3 normal;
};

struct Side {
  int id = -1;
  int rotation = 0;
  std::array<float, 5> uaxis = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
  std::array<float, 5> vaxis = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
  std::string material;
  std::array<QVector3D, 4> vertices_plus;
  bool hasVerticesPlus = false;
  // Three points, already in editor space.
  std::array<QVector3D, 3> planePoints;
  bool hasPlane = false;
};

struct Solid {
  int id = -1;
  std::vector<Side> sides;
};

// The single source of truth for a face's material mapping (nothing baked into vertices). For a world point p,
// in tile units (1.0 = one full trip across the texture):
//   U = dot(p, uAxis) * tilesPerUnitU + offsetU
//   V = dot(p, vAxis) * tilesPerUnitV + offsetV
// Axes are unit length so tilesPerUnit carries all the scaling.
struct FaceTexBasis {
  glm::vec3 uAxis {1.0f, 0.0f, 0.0f};
  glm::vec3 vAxis {0.0f, 1.0f, 0.0f};
  float tilesPerUnitU = 1.0f;
  float tilesPerUnitV = 1.0f;
  float offsetU = 0.0f; // in tiles
  float offsetV = 0.0f; // in tiles
  bool valid = false;

  [[nodiscard]] glm::vec2 uvAt(const glm::vec3 &p) const {
    return {glm::dot(p, uAxis) * tilesPerUnitU + offsetU,
            glm::dot(p, vAxis) * tilesPerUnitV + offsetV};
  }
};

// CPU-side mesh; the renderer owns all GPU memory and polls versions.
// Smooth shading: faces meeting at a corner within this angle shade as one surface (Blender's "smooth by angle").
inline constexpr double kSmoothAngleDeg = 30.0;

// Per corner of each polygon (`normals` = outward polygon normals): area-weighted average of the sharing
// polygons within `angleDeg`. Used by brush meshes and the Godot export alike, so both shade the same.
std::vector<std::vector<glm::vec3>> smoothPolygonNormals(const std::vector<std::vector<glm::dvec3>> &polys,
                                                         const std::vector<glm::dvec3> &normals,
                                                         double angleDeg = kSmoothAngleDeg);

class Mesh {
public:
  Mesh();
  ~Mesh();

  static std::shared_ptr<Mesh> createFromCubeSides(const std::array<Side, 6> &sides);

  void addVertices(const std::vector<Vertex> &vertices);
  void addIndices(const std::vector<uint32_t> &indices);

  const std::vector<Vertex> &vertexData() const { return m_vertices; }
  const std::vector<uint32_t> &indexData() const { return m_indices; }

  // Faces are convex polygons, each a contiguous run of fan triangles. Meshes without explicit ranges use one
  // quad (6 indices) per face.
  int faceCount() const {
    return m_faceFirst.empty() ? static_cast<int>(m_indices.size() / 6) : static_cast<int>(m_faceFirst.size());
  }
  [[nodiscard]] uint32_t faceIndexStart(int face) const {
    return m_faceFirst.empty() ? uint32_t(face) * 6u : m_faceFirst[size_t(face)];
  }
  [[nodiscard]] uint32_t faceIndexCount(int face) const {
    return m_faceFirst.empty() ? 6u : m_faceCount[size_t(face)];
  }
  [[nodiscard]] bool hasFaceRanges() const { return !m_faceFirst.empty(); }
  // Model faces carry real UVs in the vertices instead of a world projection.
  void setFaceUsesVertexUV(int face, bool on) {
    if (face < 0)
      return;
    if (m_faceVertexUV.size() <= size_t(face))
      m_faceVertexUV.resize(size_t(face) + 1, 0);
    m_faceVertexUV[size_t(face)] = on ? 1 : 0;
    bumpFaces();
  }
  [[nodiscard]] bool faceUsesVertexUV(int face) const {
    return face >= 0 && size_t(face) < m_faceVertexUV.size() && m_faceVertexUV[size_t(face)] != 0;
  }
  [[nodiscard]] const std::vector<uint32_t> &faceFirstArray() const { return m_faceFirst; }
  [[nodiscard]] const std::vector<uint32_t> &faceCountArray() const { return m_faceCount; }
  void setFaceRanges(std::vector<uint32_t> first, std::vector<uint32_t> count);
  // Perimeter in winding order, mesh-local.
  bool getFacePolygon(int faceIndex, std::vector<glm::vec3> &out) const;
  // Normal points out of the solid, mesh-local.
  bool getFacePlane(int faceIndex, glm::vec3 &normal, float &distance) const;

  // Vertices counter-clockwise seen from outside; materials / bases are set after.
  static std::shared_ptr<Mesh> createFromPolygons(const std::vector<std::vector<glm::vec3>> &polygons);
  // Re-derives texCoords from the face bases and rebuilds tangents.
  void refreshTangentFrame();
  // Smoothed across faces under kSmoothAngleDeg, or flat. Positions do not change.
  void setSmoothShading(bool smooth);
  // Per face, one normal per corner of getFacePolygon(f) (mesh space); vertices take their nearest corner's.
  void setCornerNormals(const std::vector<std::vector<glm::vec3>> &faceCorners);

  QSharedPointer<Material> faceMaterial(int faceIndex) const;
  // No refcount traffic (for the visibility sweep).
  [[nodiscard]] const QSharedPointer<Material> &faceMaterialRef(int faceIndex) const {
    static const QSharedPointer<Material> kEmpty;
    if (faceIndex >= 0 && faceIndex < static_cast<int>(m_faceMaterials.size()))
      return m_faceMaterials[faceIndex];
    return kEmpty;
  }
  void setFaceMaterial(int faceIndex, const QSharedPointer<Material> &material);

  void setFaceTexBasis(int faceIndex, const FaceTexBasis &basis);
  [[nodiscard]] const FaceTexBasis &faceTexBasis(int faceIndex) const;

  void setFacePickingID(int faceIndex, uint32_t id);
  [[nodiscard]] uint32_t getFacePickingID(int faceIndex) const;

  // Ids into Scene::faceGroups; for selecting faces by category, ignored by the renderer.
  void setFaceGroups(int faceIndex, const FaceGroupSet &groups);
  [[nodiscard]] const FaceGroupSet &faceGroups(int faceIndex) const;
  void addFaceGroup(int faceIndex, quint32 id);
  void removeFaceGroup(int faceIndex, quint32 id);
  [[nodiscard]] quint64 groupVersion() const { return m_groupVersion; }

  bool getFaceQuad(int faceIndex, std::array<glm::vec3, 4> &out) const;

  [[nodiscard]] std::pair<glm::vec3, glm::vec3> getAABB() const;

  // Vertex / index edits invalidate the buffers, face edits only the per-face records.
  [[nodiscard]] quint64 geometryVersion() const { return m_geometryVersion; }
  [[nodiscard]] quint64 faceVersion() const { return m_faceVersion; }

  // Process-wide: poll these instead of every mesh's version, sweep only when one moved.
  [[nodiscard]] static quint64 globalGeometryEpoch() { return s_geometryEpoch.load(); }
  [[nodiscard]] static quint64 globalFaceEpoch() { return s_faceEpoch.load(); }

  // Thick-wireframe geometry: barycentric weights with coplanar interior diagonals pinned away from zero.
  const std::vector<WireVertex> &wireVertices() const;
  const std::vector<uint32_t> &wireIndices() const;

  static constexpr float COPLANAR_DOT_EPSILON = 0.9999f;

private:
  void bumpGeometry();
  void bumpFaces();
  [[nodiscard]] bool facePerimeter(int faceIndex, std::array<uint32_t, 4> &outVerts) const;
  void ensureFaceArrays(int faceCount);
  void rebuildTangents();
  void rebuildWireframe() const;

  std::vector<Vertex> m_vertices;
  std::vector<uint32_t> m_indices;
  std::vector<uint8_t> m_faceVertexUV;
  std::vector<uint32_t> m_faceFirst;
  std::vector<uint32_t> m_faceCount;

  std::vector<QSharedPointer<Material>> m_faceMaterials;
  std::vector<FaceTexBasis> m_faceTexBases;
  std::vector<uint32_t> m_facePickingIDs;
  std::vector<FaceGroupSet> m_faceGroups;
  quint64 m_groupVersion = 0;

  quint64 m_geometryVersion = 0;
  quint64 m_faceVersion = 0;

  inline static std::atomic<quint64> s_geometryEpoch {0};
  inline static std::atomic<quint64> s_faceEpoch {0};

  mutable bool m_wireDirty = true;
  mutable std::vector<WireVertex> m_wireVertices;
  mutable std::vector<uint32_t> m_wireIndices;
};

} // namespace eng
