#include "vk_gpu_scene.h"

#include <algorithm>

#include <rhi/qrhi.h>

#include "engine/scene.h"
#include "vk_shared.h"

namespace vk {

quint32 RangeAllocator::allocate(const quint32 count) {
  if (count == 0)
    return m_bump;

  // First fit by address; the free list is short in practice.
  for (size_t i = 0; i < m_free.size(); ++i) {
    if (m_free[i].count < count)
      continue;
    const quint32 start = m_free[i].start;
    if (m_free[i].count == count)
      m_free.erase(m_free.begin() + qsizetype(i));
    else {
      m_free[i].start += count;
      m_free[i].count -= count;
    }
    return start;
  }

  const quint32 start = m_bump;
  m_bump += count;
  return start;
}

void RangeAllocator::release(const quint32 start, const quint32 count) {
  if (count == 0)
    return;

  size_t i = 0;
  while (i < m_free.size() && m_free[i].start < start)
    ++i;

  const bool joinsPrev = i > 0 && m_free[i - 1].start + m_free[i - 1].count == start;
  const bool joinsNext = i < m_free.size() && start + count == m_free[i].start;

  if (joinsPrev && joinsNext) {
    m_free[i - 1].count += count + m_free[i].count;
    m_free.erase(m_free.begin() + qsizetype(i));
  } else if (joinsPrev) {
    m_free[i - 1].count += count;
  } else if (joinsNext) {
    m_free[i].start = start;
    m_free[i].count += count;
  } else {
    m_free.insert(m_free.begin() + qsizetype(i), Free {start, count});
  }
}

void RangeAllocator::clear() {
  m_free.clear();
  m_bump = 0;
}

void GpuScene::invalidate() {
  delete m_vertexBuffer;
  delete m_indexBuffer;
  delete m_wireBuffer;
  delete m_wireIndexBuffer;
  m_vertexBuffer = m_indexBuffer = m_wireBuffer = m_wireIndexBuffer = nullptr;
  m_vertexCapacity = m_indexCapacity = m_wireCapacity = m_wireIndexCapacity = 0;
  reset();
}

void GpuScene::reset() {
  m_vertices.clear();
  m_indices.clear();
  m_faces.clear();
  m_wireVertices.clear();
  m_wireIndices.clear();
  m_liveFaces = 0;
  ++m_faceEpoch;
  m_records.clear();
  m_wireMeshes.clear();
  m_syncedScene = nullptr;
  m_syncedStructureVersion = ~0ull;
  m_syncedGeometryEpoch = ~0ull;
}

QRhiBuffer *GpuScene::growPool(QRhiBuffer *buffer, quint32 &capacity, const quint32 needed,
                               const int usage) {
  if (buffer && capacity >= needed)
    return buffer;

  const quint32 grown = std::max(needed, std::max(capacity * 2, 1u << 16));
  // Dynamic: refilled in place with updateDynamicBuffer, which a Static buffer would silently ignore.
  auto *buf = Shared::instance().rhi()->newBuffer(
      QRhiBuffer::Dynamic, static_cast<QRhiBuffer::UsageFlags>(usage), int(grown));
  if (!buf->create()) {
    qWarning("GpuScene: failed to allocate %u byte pool", grown);
    delete buf;
    return nullptr;
  }

  if (buffer)
    Shared::instance().retire(buffer);

  capacity = grown;
  return buf;
}

void GpuScene::appendObject(const quint32 stableId, const std::shared_ptr<eng::Mesh> &mesh,
                           QRhiResourceUpdateBatch *rub) {
  if (!mesh)
    return;
  const auto &verts = mesh->vertexData();
  const auto &idx = mesh->indexData();
  if (verts.empty() || idx.empty())
    return;

  const quint32 vertBytes = quint32(verts.size() * sizeof(eng::Vertex));
  const quint32 idxBytes = quint32(idx.size() * sizeof(uint32_t));

  auto *vbuf = growPool(m_vertexBuffer, m_vertexCapacity,
                       (m_vertices.highWater() + quint32(verts.size())) * quint32(sizeof(eng::Vertex)),
                       QRhiBuffer::VertexBuffer);
  if (!vbuf)
    return;
  auto *ibuf = growPool(m_indexBuffer, m_indexCapacity,
                        (m_indices.highWater() + quint32(idx.size())) * quint32(sizeof(uint32_t)),
                        QRhiBuffer::IndexBuffer);
  if (!ibuf)
    return;
  m_vertexBuffer = vbuf;
  m_indexBuffer = ibuf;

  MeshGpuRecord rec;
  rec.vertexOffset = m_vertices.allocate(quint32(verts.size()));
  rec.vertexCount = quint32(verts.size());
  rec.indexOffset = m_indices.allocate(quint32(idx.size()));
  rec.indexCount = static_cast<quint32>(idx.size());
  rec.faceOffset = m_faces.allocate(quint32(mesh->faceCount()));
  rec.faceCount = static_cast<quint32>(mesh->faceCount());
  rec.geometryVersion = mesh->geometryVersion();

  m_liveFaces += rec.faceCount;

  rub->updateDynamicBuffer(m_vertexBuffer, rec.vertexOffset * sizeof(eng::Vertex), int(vertBytes),
                          verts.data());
  rub->updateDynamicBuffer(m_indexBuffer, rec.indexOffset * sizeof(uint32_t), int(idxBytes),
                          idx.data());

  m_records.insert(stableId, rec);
}

void GpuScene::releaseRecord(const MeshGpuRecord &rec, const bool releaseWire) {
  m_vertices.release(rec.vertexOffset, rec.vertexCount);
  m_indices.release(rec.indexOffset, rec.indexCount);
  m_faces.release(rec.faceOffset, rec.faceCount);
  m_liveFaces -= rec.faceCount;
  if (releaseWire) {
    m_wireVertices.release(rec.wireVertexOffset, rec.wireVertexCount);
    m_wireIndices.release(rec.wireIndexOffset, rec.wireIndexCount);
  }
}

// A re-created pool starts empty: every record uploaded into the previous generation goes back down.
void GpuScene::reuploadSolid(const eng::Scene *scene, QRhiResourceUpdateBatch *rub) {
  if (!m_vertexBuffer || !m_indexBuffer)
    return;
  for (const auto &obj: scene->gameObjects) {
    const auto it = m_records.constFind(obj->stableId());
    if (it == m_records.constEnd())
      continue;
    const MeshGpuRecord &rec = it.value();
    const auto mesh = obj->getMesh();
    if (!mesh || rec.vertexCount == 0 || rec.indexCount == 0)
      continue;
    rub->updateDynamicBuffer(m_vertexBuffer, rec.vertexOffset * sizeof(eng::Vertex),
                            int(size_t(rec.vertexCount) * sizeof(eng::Vertex)),
                            mesh->vertexData().data());
    rub->updateDynamicBuffer(m_indexBuffer, rec.indexOffset * sizeof(uint32_t),
                            int(size_t(rec.indexCount) * sizeof(uint32_t)), mesh->indexData().data());
  }
}

void GpuScene::reuploadWire(QRhiResourceUpdateBatch *rub) {
  if (!m_wireBuffer || !m_wireIndexBuffer)
    return;
  for (auto it = m_wireMeshes.constBegin(); it != m_wireMeshes.constEnd(); ++it) {
    const auto &mesh = it.value();
    const auto rec = m_records.constFind(it.key());
    if (rec == m_records.constEnd())
      continue;
    const MeshGpuRecord &r = rec.value();
    if (r.wireVertexCount == 0 || r.wireIndexCount == 0 || !mesh)
      continue;
    rub->updateDynamicBuffer(m_wireBuffer, r.wireVertexOffset * sizeof(eng::WireVertex),
                            int(size_t(r.wireVertexCount) * sizeof(eng::WireVertex)),
                            mesh->wireVertices().data());
    rub->updateDynamicBuffer(m_wireIndexBuffer, r.wireIndexOffset * sizeof(uint32_t),
                            int(size_t(r.wireIndexCount) * sizeof(uint32_t)),
                            mesh->wireIndices().data());
  }
}

void GpuScene::sync(const eng::Scene *scene, QRhiResourceUpdateBatch *rub) {
  if (!scene || !rub || !Shared::instance().isValid())
    return;

  const bool structureChanged = scene != m_syncedScene
      || scene->structureVersion() != m_syncedStructureVersion;
  // Nothing changed shape while the geometry epoch stood still (moving is not a geometry change).
  if (!structureChanged && eng::Mesh::globalGeometryEpoch() == m_syncedGeometryEpoch)
    return;

  if (scene != m_syncedScene)
    reset();

  if (structureChanged) {
    // Drop records of objects that are gone; their pool space goes back to the allocator.
    QHash<quint32, bool> present;
    present.reserve(qsizetype(scene->gameObjects.size()) * 2);
    for (const auto &obj: scene->gameObjects)
      present.insert(obj->stableId(), true);

    for (auto it = m_records.begin(); it != m_records.end();) {
      if (present.contains(it.key())) {
        ++it;
        continue;
      }
      releaseRecord(it.value(), true);
      m_wireMeshes.remove(it.key());
      it = m_records.erase(it);
    }
  }

  QRhiBuffer *solidVtxBefore = m_vertexBuffer;
  QRhiBuffer *solidIdxBefore = m_indexBuffer;

  for (const auto &obj: scene->gameObjects) {
    const auto mesh = obj->getMesh();
    if (!mesh)
      continue;

    const auto it = m_records.find(obj->stableId());
    if (it == m_records.end()) {
      appendObject(obj->stableId(), mesh, rub);
      continue;
    }

    MeshGpuRecord &rec = it.value();
    if (rec.geometryVersion == mesh->geometryVersion())
      continue;

    const auto &verts = mesh->vertexData();
    const auto &idx = mesh->indexData();
    const quint32 faceCount = quint32(mesh->faceCount());

    // Same footprint: bytes swap in place. Otherwise only this object relocates.
    if (rec.fits(quint32(verts.size()), quint32(idx.size()), faceCount) && rec.indexCount > 0) {
      rub->updateDynamicBuffer(m_vertexBuffer, rec.vertexOffset * sizeof(eng::Vertex),
                              int(verts.size() * sizeof(eng::Vertex)), verts.data());
      rec.geometryVersion = mesh->geometryVersion();
      continue;
    }

    // Wire offsets live in the wire pools, untouched by a solid relocate: carry them over.
    const MeshGpuRecord wireCarry = rec;
    releaseRecord(rec, false);
    // Erase now: an emptied mesh gets no new record, and keeping the old one would let its ranges be released twice.
    m_records.erase(it);
    appendObject(obj->stableId(), mesh, rub);
    if (const auto fresh = m_records.find(obj->stableId()); fresh != m_records.end()) {
      MeshGpuRecord &next = fresh.value();
      next.wireVertexOffset = wireCarry.wireVertexOffset;
      next.wireVertexCount = wireCarry.wireVertexCount;
      next.wireIndexOffset = wireCarry.wireIndexOffset;
      next.wireIndexCount = wireCarry.wireIndexCount;
      next.wireVersion = wireCarry.wireVersion;
    } else {
      m_wireVertices.release(wireCarry.wireVertexOffset, wireCarry.wireVertexCount);
      m_wireIndices.release(wireCarry.wireIndexOffset, wireCarry.wireIndexCount);
      m_wireMeshes.remove(obj->stableId());
    }
  }

  if (m_vertexBuffer != solidVtxBefore || m_indexBuffer != solidIdxBefore)
    reuploadSolid(scene, rub);

  m_syncedScene = scene;
  m_syncedStructureVersion = scene->structureVersion();
  m_syncedGeometryEpoch = eng::Mesh::globalGeometryEpoch();
}

void GpuScene::syncWire(const quint32 stableId, const std::shared_ptr<eng::Mesh> &mesh,
                       QRhiResourceUpdateBatch *rub) {
  if (!mesh || !rub || !Shared::instance().isValid())
    return;

  const auto found = m_records.find(stableId);
  if (found == m_records.constEnd())
    return;

  const quint64 wireVersion = mesh->geometryVersion();
  if (found->wireVersion == wireVersion)
    return;

  const auto &wireVerts = mesh->wireVertices();
  const auto &wireIdx = mesh->wireIndices();
  if (wireVerts.empty() || wireIdx.empty())
    return;

  const quint32 wireBytes = quint32(wireVerts.size() * sizeof(eng::WireVertex));
  const quint32 wireIdxBytes = quint32(wireIdx.size() * sizeof(uint32_t));

  QRhiBuffer *wireVtxBefore = m_wireBuffer;
  QRhiBuffer *wireIdxBefore = m_wireIndexBuffer;

  auto *wbuf = growPool(m_wireBuffer, m_wireCapacity,
                        (m_wireVertices.highWater() + quint32(wireVerts.size()))
                            * quint32(sizeof(eng::WireVertex)),
                        QRhiBuffer::VertexBuffer);
  if (!wbuf)
    return;
  auto *widx = growPool(m_wireIndexBuffer, m_wireIndexCapacity,
                        (m_wireIndices.highWater() + quint32(wireIdx.size()))
                            * quint32(sizeof(uint32_t)),
                        QRhiBuffer::IndexBuffer);
  if (!widx)
    return;

  m_wireBuffer = wbuf;
  m_wireIndexBuffer = widx;

  MeshGpuRecord rec = found.value();
  // Release the previous wire range so reselecting does not leak pool space.
  if (rec.wireIndexCount != 0) {
    m_wireVertices.release(rec.wireVertexOffset, rec.wireVertexCount);
    m_wireIndices.release(rec.wireIndexOffset, rec.wireIndexCount);
  }
  rec.wireVertexOffset = m_wireVertices.allocate(quint32(wireVerts.size()));
  rec.wireVertexCount = quint32(wireVerts.size());
  rec.wireIndexOffset = m_wireIndices.allocate(quint32(wireIdx.size()));
  rec.wireIndexCount = static_cast<quint32>(wireIdx.size());
  rec.wireVersion = wireVersion;

  // A fresh wire pool is empty: every other selected mesh's wire goes back down too.
  if (m_wireBuffer != wireVtxBefore || m_wireIndexBuffer != wireIdxBefore)
    reuploadWire(rub);

  rub->updateDynamicBuffer(m_wireBuffer, rec.wireVertexOffset * sizeof(eng::WireVertex), int(wireBytes),
                          wireVerts.data());
  rub->updateDynamicBuffer(m_wireIndexBuffer, rec.wireIndexOffset * sizeof(uint32_t),
                          int(wireIdxBytes), wireIdx.data());

  m_wireMeshes.insert(stableId, mesh);
  m_records.insert(stableId, rec);
}

const MeshGpuRecord *GpuScene::record(const quint32 stableId) const {
  const auto it = m_records.constFind(stableId);
  return it == m_records.constEnd() ? nullptr : &*it;
}

} // namespace vk
