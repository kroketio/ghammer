#pragma once

#include <memory>
#include <vector>

#include <QHash>
#include <QSharedPointer>

#include <rhi/qrhi.h>

#include "engine/mesh.h"

class QRhiBuffer;
class QRhiResourceUpdateBatch;

namespace eng {
class Scene;
}

namespace vk {

// First-fit range allocator with coalescing on release, so deletes leave reusable holes, not a relayout.
class RangeAllocator {
public:
  [[nodiscard]] quint32 allocate(quint32 count);
  void release(quint32 start, quint32 count);
  // Pool size needed to hold everything handed out so far.
  [[nodiscard]] quint32 highWater() const { return m_bump; }
  void clear();

private:
  struct Free {
    quint32 start;
    quint32 count;
  };
  // Sorted by start with no adjacent entries, so a release coalesces with both neighbours.
  std::vector<Free> m_free;
  quint32 m_bump = 0;
};

struct MeshGpuRecord {
  quint32 vertexOffset = 0; // in vertices
  quint32 vertexCount = 0;
  quint32 indexOffset = 0;  // in indices
  quint32 indexCount = 0;
  quint32 faceOffset = 0;   // base slot of this mesh's per-face records
  quint32 faceCount = 0;
  quint64 geometryVersion = ~0ull;

  quint32 wireVertexOffset = 0;
  quint32 wireVertexCount = 0;
  quint32 wireIndexOffset = 0;
  quint32 wireIndexCount = 0;
  quint64 wireVersion = ~0ull;

  // True when bytes for these counts can be swapped in place.
  [[nodiscard]] bool fits(const quint32 verts, const quint32 indices, const quint32 faces) const {
    return vertexCount == verts && indexCount == indices && faceCount == faces;
  }
};

// Scene geometry in shared buffers; records keyed by the object's stable id, never by index or Mesh pointer.
// Sync: build or patch what needs it, free records whose id is gone.
class GpuScene {
public:
  void invalidate();

  void sync(const eng::Scene *scene, QRhiResourceUpdateBatch *rub);
  void syncWire(quint32 stableId, const std::shared_ptr<eng::Mesh> &mesh, QRhiResourceUpdateBatch *rub);

  [[nodiscard]] const MeshGpuRecord *record(quint32 stableId) const;
  [[nodiscard]] QRhiBuffer *vertexBuffer() const { return m_vertexBuffer; }
  [[nodiscard]] QRhiBuffer *indexBuffer() const { return m_indexBuffer; }
  [[nodiscard]] QRhiBuffer *wireVertexBuffer() const { return m_wireBuffer; }
  [[nodiscard]] QRhiBuffer *wireIndexBuffer() const { return m_wireIndexBuffer; }
  [[nodiscard]] quint32 faceCount() const { return m_liveFaces; }
  // Freed runs leave holes, so a faceOffset can lie past faceCount(): per-face buffers must be this big.
  [[nodiscard]] quint32 faceSlots() const { return m_faces.highWater(); }
  // Bumped whenever face slots move: cached per-face uploads are stale.
  [[nodiscard]] quint32 faceEpoch() const { return m_faceEpoch; }

private:
  void reset();
  void appendObject(quint32 stableId, const std::shared_ptr<eng::Mesh> &mesh,
                   QRhiResourceUpdateBatch *rub);
  // Returns the ranges to the allocator; the buffer is not touched.
  void releaseRecord(const MeshGpuRecord &rec, bool releaseWire);
  // A re-created pool starts empty: every live record is pushed back down.
  void reuploadSolid(const eng::Scene *scene, QRhiResourceUpdateBatch *rub);
  void reuploadWire(QRhiResourceUpdateBatch *rub);
  // A new buffer starts empty, so every live record goes back down.
  QRhiBuffer *growPool(QRhiBuffer *buffer, quint32 &capacity, quint32 neededBytes, int usage);

  QRhiBuffer *m_vertexBuffer = nullptr;
  QRhiBuffer *m_indexBuffer = nullptr;
  QRhiBuffer *m_wireBuffer = nullptr;
  QRhiBuffer *m_wireIndexBuffer = nullptr;

  quint32 m_vertexCapacity = 0;
  quint32 m_indexCapacity = 0;
  quint32 m_wireCapacity = 0;
  quint32 m_wireIndexCapacity = 0;

  RangeAllocator m_vertices;
  RangeAllocator m_indices;
  RangeAllocator m_faces;
  RangeAllocator m_wireVertices;
  RangeAllocator m_wireIndices;

  quint32 m_liveFaces = 0;
  quint32 m_faceEpoch = 0;

  QHash<quint32, MeshGpuRecord> m_records;
  // Meshes with a live wire, re-uploaded when the wire pool grows.
  QHash<quint32, std::shared_ptr<eng::Mesh>> m_wireMeshes;
  const eng::Scene *m_syncedScene = nullptr;
  quint64 m_syncedStructureVersion = ~0ull;
  quint64 m_syncedGeometryEpoch = ~0ull;
};

} // namespace vk
