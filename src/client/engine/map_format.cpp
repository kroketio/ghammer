#include "map_format.h"

#include <algorithm>
#include <bit>
#include <cstring>

#include <QFile>
#include <QLoggingCategory>

#include <zlib.h>

namespace fmap {

static QLoggingCategory fmapLog("fmap", QtInfoMsg);

Value Value::makeBool(bool v) {
  Value x;
  x.kind = ValueKind::Bool;
  x.boolValue = v;
  return x;
}

Value Value::makeInt(int32_t v) {
  Value x;
  x.kind = ValueKind::Int32;
  x.intValue = v;
  return x;
}

Value Value::makeFloat(float v) {
  Value x;
  x.kind = ValueKind::Float;
  x.floatValue = v;
  return x;
}

Value Value::makeVec2(const glm::vec2 &v) {
  Value x;
  x.kind = ValueKind::Vec2;
  x.vec = glm::vec4(v, 0.0f, 1.0f);
  return x;
}

Value Value::makeVec3(const glm::vec3 &v) {
  Value x;
  x.kind = ValueKind::Vec3;
  x.vec = glm::vec4(v, 1.0f);
  return x;
}

Value Value::makeVec4(const glm::vec4 &v) {
  Value x;
  x.kind = ValueKind::Vec4;
  x.vec = v;
  return x;
}

Value Value::makeColor(const QColor &c) {
  Value x;
  x.kind = ValueKind::Color;
  x.vec = glm::vec4(c.redF(), c.greenF(), c.blueF(), c.alphaF());
  return x;
}

Value Value::makeString(const QString &v) {
  Value x;
  x.kind = ValueKind::String;
  x.string = v;
  return x;
}

double Value::asDouble(double fallback) const {
  switch (kind) {
    case ValueKind::Bool:
      return boolValue ? 1.0 : 0.0;
    case ValueKind::Int32:
      return double(intValue);
    case ValueKind::Float:
      return double(floatValue);
    case ValueKind::Vec2:
    case ValueKind::Vec3:
    case ValueKind::Vec4:
    case ValueKind::Color:
      return double(vec.x);
    default:
      return fallback;
  }
}

bool Value::asBool(bool fallback) const {
  switch (kind) {
    case ValueKind::Bool:
      return boolValue;
    case ValueKind::Int32:
      return intValue != 0;
    case ValueKind::Float:
      return floatValue != 0.0f;
    default:
      return fallback;
  }
}

glm::vec3 Value::asVec3(const glm::vec3 &fallback) const {
  switch (kind) {
    case ValueKind::Vec2:
      return glm::vec3(vec.x, vec.y, 0.0f);
    case ValueKind::Vec3:
    case ValueKind::Vec4:
    case ValueKind::Color:
      return glm::vec3(vec.x, vec.y, vec.z);
    default:
      return fallback;
  }
}

const Value *EntityRecord::find(const QString &key) const {
  for (const auto &[k, v]: properties) {
    if (k == key)
      return &v;
  }
  return nullptr;
}

void EntityRecord::set(const QString &key, const Value &value) {
  for (auto &[k, v]: properties) {
    if (k == key) {
      v = value;
      return;
    }
  }
  properties.emplace_back(key, value);
}

void Writer::u8(uint8_t v) {
  m_buf.append(char(v));
}

void Writer::u16(uint16_t v) {
  char b[2];
  std::memcpy(b, &v, 2);
  m_buf.append(b, 2);
}

void Writer::u32(uint32_t v) {
  char b[4];
  std::memcpy(b, &v, 4);
  m_buf.append(b, 4);
}

void Writer::u64(uint64_t v) {
  char b[8];
  std::memcpy(b, &v, 8);
  m_buf.append(b, 8);
}

void Writer::f32(float v) {
  char b[4];
  std::memcpy(b, &v, 4);
  m_buf.append(b, 4);
}

void Writer::f64(double v) {
  char b[8];
  std::memcpy(b, &v, 8);
  m_buf.append(b, 8);
}

void Writer::vec2(const glm::vec2 &v) {
  f32(v.x);
  f32(v.y);
}

void Writer::vec3(const glm::vec3 &v) {
  f32(v.x);
  f32(v.y);
  f32(v.z);
}

void Writer::vec4(const glm::vec4 &v) {
  f32(v.x);
  f32(v.y);
  f32(v.z);
  f32(v.w);
}

void Writer::dvec2(const glm::dvec2 &v) {
  f64(v.x);
  f64(v.y);
}

void Writer::dvec3(const glm::dvec3 &v) {
  f64(v.x);
  f64(v.y);
  f64(v.z);
}

void Writer::str(const QString &v) {
  const QByteArray utf8 = v.toUtf8();
  u32(uint32_t(utf8.size()));
  m_buf.append(utf8);
}

void Writer::blob(const QByteArray &v) {
  u32(uint32_t(v.size()));
  m_buf.append(v);
}

Reader::Reader(const QByteArray &buf)
    : m_data(reinterpret_cast<const uint8_t *>(buf.constData())), m_size(size_t(buf.size())) {}

size_t Reader::remaining() const {
  return m_pos < m_size ? m_size - m_pos : 0;
}

void Reader::skip(const size_t n) {
  if (n > remaining()) {
    m_ok = false;
    m_pos = m_size;
    return;
  }
  m_pos += n;
}

uint8_t Reader::u8() {
  if (remaining() < 1) {
    m_ok = false;
    return 0;
  }
  return m_data[m_pos++];
}

uint16_t Reader::u16() {
  if (remaining() < 2) {
    m_ok = false;
    return 0;
  }
  uint16_t v;
  std::memcpy(&v, m_data + m_pos, 2);
  m_pos += 2;
  return v;
}

uint32_t Reader::u32() {
  if (remaining() < 4) {
    m_ok = false;
    return 0;
  }
  uint32_t v;
  std::memcpy(&v, m_data + m_pos, 4);
  m_pos += 4;
  return v;
}

uint64_t Reader::u64() {
  if (remaining() < 8) {
    m_ok = false;
    return 0;
  }
  uint64_t v;
  std::memcpy(&v, m_data + m_pos, 8);
  m_pos += 8;
  return v;
}

float Reader::f32() {
  return std::bit_cast<float>(u32());
}

double Reader::f64() {
  return std::bit_cast<double>(u64());
}

glm::vec2 Reader::vec2() {
  return {f32(), f32()};
}

glm::vec3 Reader::vec3() {
  return {f32(), f32(), f32()};
}

glm::vec4 Reader::vec4() {
  return {f32(), f32(), f32(), f32()};
}

glm::dvec2 Reader::dvec2() {
  return {f64(), f64()};
}

glm::dvec3 Reader::dvec3() {
  return {f64(), f64(), f64()};
}

QString Reader::str() {
  const uint32_t len = u32();
  if (len > remaining()) {
    m_ok = false;
    return {};
  }
  const QString s = QString::fromUtf8(reinterpret_cast<const char *>(m_data + m_pos), int(len));
  m_pos += len;
  return s;
}

QByteArray Reader::blob() {
  const uint32_t len = u32();
  if (len > remaining()) {
    m_ok = false;
    return {};
  }
  QByteArray b(reinterpret_cast<const char *>(m_data + m_pos), int(len));
  m_pos += len;
  return b;
}

void writeValue(Writer &w, const Value &v) {
  w.u8(uint8_t(v.kind));
  switch (v.kind) {
    case ValueKind::Bool:
      w.u8(v.boolValue ? 1 : 0);
      break;
    case ValueKind::Int32:
      w.u32(uint32_t(v.intValue));
      break;
    case ValueKind::Float:
      w.f32(v.floatValue);
      break;
    case ValueKind::Vec2:
      w.vec2(glm::vec2(v.vec));
      break;
    case ValueKind::Vec3:
      w.vec3(glm::vec3(v.vec));
      break;
    case ValueKind::Vec4:
    case ValueKind::Color:
      w.vec4(v.vec);
      break;
    case ValueKind::String:
      w.str(v.string);
      break;
  }
}

Value readValue(Reader &r) {
  const uint8_t kind = r.u8();
  switch (ValueKind(kind)) {
    case ValueKind::Bool:
      return Value::makeBool(r.u8() != 0);
    case ValueKind::Int32:
      return Value::makeInt(int32_t(r.u32()));
    case ValueKind::Float:
      return Value::makeFloat(r.f32());
    case ValueKind::Vec2:
      return Value::makeVec2(r.vec2());
    case ValueKind::Vec3:
      return Value::makeVec3(r.vec3());
    case ValueKind::Vec4:
      return Value::makeVec4(r.vec4());
    case ValueKind::Color:
      return Value::makeColor(QColor::fromRgbF(r.f32(), r.f32(), r.f32(), r.f32()));
    case ValueKind::String:
      return Value::makeString(r.str());
    default:
      r.fail();
      return {};
  }
}

// Believe a count only if the bytes it implies are there; else a corrupt file asks reserve() for gigabytes.
static bool countFits(Reader &r, uint64_t count, size_t minBytesEach) {
  if (count * uint64_t(std::max<size_t>(1, minBytesEach)) > uint64_t(r.remaining())) {
    r.fail();
    return false;
  }
  return true;
}

void writeProperties(Writer &w, const PropertyList &props) {
  w.u32(uint32_t(props.size()));
  for (const auto &[k, v]: props) {
    w.str(k);
    writeValue(w, v);
  }
}

PropertyList readProperties(Reader &r) {
  PropertyList props;
  const uint32_t count = r.u32();
  if (!countFits(r, count, 5))
    return props;
  props.reserve(count);
  for (uint32_t i = 0; i < count && r.ok(); ++i) {
    const QString key = r.str();
    const Value value = readValue(r);
    if (!r.ok())
      break;
    props.emplace_back(key, value);
  }
  return props;
}

void writeMeshRecord(Writer &w, const MeshRecord &m) {
  w.u32(m.id);
  w.str(m.name);

  w.u32(uint32_t(m.vertices.size()));
  for (const auto &v: m.vertices) {
    w.vec3(v.position);
    w.vec3(v.normal);
    w.vec2(v.texCoord);
    w.vec3(v.tangent);
    w.vec3(v.bitangent);
  }

  w.u32(uint32_t(m.indices.size()));
  for (const uint32_t i: m.indices)
    w.u32(i);

  w.u32(uint32_t(m.faces.size()));
  for (const auto &f: m.faces) {
    w.u32(f.indexOffset);
    w.u32(f.indexCount);
    w.str(f.material);
    w.vec3(f.basis.uAxis);
    w.vec3(f.basis.vAxis);
    w.f32(f.basis.tilesPerUnitU);
    w.f32(f.basis.tilesPerUnitV);
    w.f32(f.basis.offsetU);
    w.f32(f.basis.offsetV);
    w.u8(f.basis.valid ? 1 : 0);
    w.u32(f.pickingId);
    const size_t groups = std::min<size_t>(f.groups.size(), 255);
    w.u8(uint8_t(groups));
    for (size_t g = 0; g < groups; ++g)
      w.u32(f.groups[g]);
  }
}

MeshRecord readMeshRecord(Reader &r, const uint32_t version) {
  MeshRecord m;
  m.id = r.u32();
  m.name = r.str();

  const uint32_t vertexCount = r.u32();
  if (!countFits(r, vertexCount, 14 * sizeof(float)))
    return m;
  m.vertices.reserve(vertexCount);
  for (uint32_t i = 0; i < vertexCount && r.ok(); ++i) {
    eng::Vertex v;
    v.position = r.vec3();
    v.normal = r.vec3();
    v.texCoord = r.vec2();
    v.tangent = r.vec3();
    v.bitangent = r.vec3();
    m.vertices.push_back(v);
  }

  const uint32_t indexCount = r.u32();
  if (!countFits(r, indexCount, sizeof(uint32_t)))
    return m;
  m.indices.reserve(indexCount);
  for (uint32_t i = 0; i < indexCount && r.ok(); ++i)
    m.indices.push_back(r.u32());

  const uint32_t faceCount = r.u32();
  if (!countFits(r, faceCount, 45))
    return m;
  m.faces.reserve(faceCount);
  for (uint32_t i = 0; i < faceCount && r.ok(); ++i) {
    FaceRecord f;
    f.indexOffset = r.u32();
    f.indexCount = r.u32();
    f.material = r.str();
    f.basis.uAxis = r.vec3();
    f.basis.vAxis = r.vec3();
    f.basis.tilesPerUnitU = r.f32();
    f.basis.tilesPerUnitV = r.f32();
    f.basis.offsetU = r.f32();
    f.basis.offsetV = r.f32();
    f.basis.valid = r.u8() != 0;
    f.pickingId = r.u32();
    if (version >= 2) {
      const uint8_t groups = r.u8();
      for (uint8_t g = 0; g < groups && r.ok(); ++g)
        eng::facegroup::insert(f.groups, r.u32());
    }
    m.faces.push_back(f);
  }

  // Out-of-range indices would reach picking and the GPU upload: refuse the record.
  if (r.ok()) {
    for (const uint32_t idx: m.indices) {
      if (idx >= m.vertices.size()) {
        r.fail();
        return m;
      }
    }
    for (const auto &f: m.faces) {
      if (uint64_t(f.indexOffset) + f.indexCount > m.indices.size()) {
        r.fail();
        return m;
      }
    }
  }
  return m;
}

void writeNodeRecord(Writer &w, const NodeRecord &n) {
  w.u32(n.id);
  w.u32(n.meshId);
  w.str(n.name);
  w.vec3(n.position);
  w.vec3(n.rotation);
  w.vec3(n.scale);
  w.u32(n.flags);
  w.u32(n.group);
  w.u32(n.brushEntity);
  w.u32(n.procObject);
  w.u32(n.mirrorLink);
  w.u8(n.mirrorSide);
  for (int i = 0; i < 4; ++i)
    w.f32(n.orientation[i]);
  w.u32(n.modHost);
}

NodeRecord readNodeRecord(Reader &r, const uint32_t version) {
  NodeRecord n;
  n.id = r.u32();
  n.meshId = r.u32();
  n.name = r.str();
  n.position = r.vec3();
  n.rotation = r.vec3();
  n.scale = r.vec3();
  if (version >= 2) {
    n.flags = r.u32();
    n.group = r.u32();
  }
  if (version >= 3)
    n.brushEntity = r.u32();
  if (version >= 4)
    n.procObject = r.u32();
  if (version >= 5) {
    n.mirrorLink = r.u32();
    n.mirrorSide = r.u8();
  }
  if (version >= 6)
    for (int i = 0; i < 4; ++i)
      n.orientation[i] = r.f32();
  if (version >= 7)
    n.modHost = r.u32();
  return n;
}

void writeEntityRecord(Writer &w, const EntityRecord &e) {
  w.u32(e.id);
  w.str(e.type);
  w.str(e.name);
  w.vec3(e.position);
  writeProperties(w, e.properties);
}

EntityRecord readEntityRecord(Reader &r) {
  EntityRecord e;
  e.id = r.u32();
  e.type = r.str();
  e.name = r.str();
  e.position = r.vec3();
  e.properties = readProperties(r);
  return e;
}

void writeSceneState(Writer &w, const SceneState &s) {
  w.u8(s.sun.enabled ? 1 : 0);
  w.vec3(s.sun.direction);
  w.vec3(s.sun.color);
  w.f32(s.sun.intensity);
  w.f32(s.sun.azimuth);
  w.f32(s.sun.elevation);

  w.vec3(s.ambient.color);
  w.f32(s.ambient.intensity);

  w.f32(s.fog.density);
  w.vec3(s.fog.color);

  w.u8(s.shadows.enabled ? 1 : 0);
  w.f32(s.shadows.bias);
  w.f32(s.shadows.normalBias);
  w.f32(s.shadows.radius);
  w.f32(s.shadows.strength);
  w.f32(s.shadows.range);
  w.u32(uint32_t(s.shadows.mapSize));

  w.u8(s.ssao.enabled ? 1 : 0);
  w.f32(s.ssao.radius);
  w.f32(s.ssao.intensity);

  w.u32(uint32_t(s.render.renderMode));
  w.u32(uint32_t(s.render.msaaSamples));
  w.f32(s.render.exposure);
  w.f32(s.render.wireframeThickness);
  w.u32(uint32_t(s.render.gridLevel));

  w.str(s.sky);  // v2
  w.str(s.lut);  // v3
}

SceneState readSceneState(Reader &r, const uint32_t version) {
  SceneState s;
  s.sun.enabled = r.u8() != 0;
  s.sun.direction = r.vec3();
  s.sun.color = r.vec3();
  s.sun.intensity = r.f32();
  s.sun.azimuth = r.f32();
  s.sun.elevation = r.f32();

  s.ambient.color = r.vec3();
  s.ambient.intensity = r.f32();

  s.fog.density = r.f32();
  s.fog.color = r.vec3();

  s.shadows.enabled = r.u8() != 0;
  s.shadows.bias = r.f32();
  s.shadows.normalBias = r.f32();
  s.shadows.radius = r.f32();
  s.shadows.strength = r.f32();
  s.shadows.range = r.f32();
  s.shadows.mapSize = int32_t(r.u32());

  s.ssao.enabled = r.u8() != 0;
  s.ssao.radius = r.f32();
  s.ssao.intensity = r.f32();

  s.render.renderMode = int32_t(r.u32());
  s.render.msaaSamples = int32_t(r.u32());
  s.render.exposure = r.f32();
  s.render.wireframeThickness = r.f32();
  s.render.gridLevel = int32_t(r.u32());
  if (version >= 2)
    s.sky = r.str();
  if (version >= 3)
    s.lut = r.str();
  return s;
}

void writeCameraState(Writer &w, const CameraState &c) {
  w.vec3(c.perspective3d.position);
  w.f32(c.perspective3d.yaw);
  w.f32(c.perspective3d.pitch);
  w.f32(c.perspective3d.fov);
  w.f32(c.perspective3d.nearPlane);
  w.f32(c.perspective3d.farPlane);
  w.f32(c.perspective3d.speed);

  w.u32(uint32_t(c.views2d.size()));
  for (const auto &v: c.views2d) {
    w.u32(uint32_t(v.perspective));
    w.f64(v.centerA);
    w.f64(v.centerB);
    w.f64(v.pxPerWorldUnit);
    w.u32(uint32_t(v.gridLevel));
    w.u8(v.showGrid ? 1 : 0);
  }
}

CameraState readCameraState(Reader &r) {
  CameraState c;
  c.perspective3d.position = r.vec3();
  c.perspective3d.yaw = r.f32();
  c.perspective3d.pitch = r.f32();
  c.perspective3d.fov = r.f32();
  c.perspective3d.nearPlane = r.f32();
  c.perspective3d.farPlane = r.f32();
  c.perspective3d.speed = r.f32();

  const uint32_t viewCount = r.u32();
  if (!countFits(r, viewCount, 16))
    return c;
  c.views2d.reserve(viewCount);
  for (uint32_t i = 0; i < viewCount && r.ok(); ++i) {
    OrthoViewState v;
    v.perspective = int32_t(r.u32());
    v.centerA = r.f64();
    v.centerB = r.f64();
    v.pxPerWorldUnit = r.f64();
    v.gridLevel = int32_t(r.u32());
    v.showGrid = r.u8() != 0;
    c.views2d.push_back(v);
  }
  return c;
}

static QByteArray maybeCompress(const QByteArray &payload, bool &compressed) {
  compressed = false;
  if (payload.size() < 256)
    return {};

  uLongf bound = compressBound(uLong(payload.size()));
  QByteArray out(int(bound), '\0');
  if (compress2(reinterpret_cast<Bytef *>(out.data()), &bound,
                reinterpret_cast<const Bytef *>(payload.constData()), uLong(payload.size()), 6)
      != Z_OK)
    return {};

  // Only compress when it actually shrinks the payload.
  if (double(bound) > double(payload.size()) * 0.92)
    return {};

  out.resize(int(bound));
  compressed = true;
  return out;
}

void writeChunk(Writer &out, const uint32_t tag, const uint32_t version, const QByteArray &payload) {
  bool compressed = false;
  const QByteArray packed = maybeCompress(payload, compressed);
  const QByteArray &stored = compressed ? packed : payload;

  out.u32(kChunkHeaderSize);
  out.u32(tag);
  out.u32(version);
  out.u32(uint32_t(stored.size()));
  out.u32(uint32_t(payload.size()));
  out.u32(uint32_t(crc32(0u, reinterpret_cast<const Bytef *>(stored.constData()), uLong(stored.size()))));
  out.u32(
      uint32_t(crc32(0u, reinterpret_cast<const Bytef *>(payload.constData()), uLong(payload.size()))));
  out.u32(compressed ? kChunkCompressed : 0u);
  out.raw(stored);
}

bool readChunkHeader(Reader &r, uint32_t &tag, uint32_t &version, QByteArray &payload,
                    QString *error) {
  const uint32_t headerSize = r.u32();
  tag = r.u32();
  version = r.u32();
  const uint32_t storedSize = r.u32();
  const uint32_t rawSize = r.u32();
  const uint32_t storedCrc = r.u32();
  const uint32_t rawCrc = r.u32();
  const uint32_t flags = r.u32();

  if (!r.ok() || headerSize < kChunkHeaderSize) {
    if (error)
      *error = QStringLiteral("truncated chunk header");
    return false;
  }
  if (storedSize > r.remaining()) {
    if (error)
      *error = QStringLiteral("chunk '%1' claims %2 bytes, %3 left").arg(tagToString(tag))
                   .arg(storedSize).arg(r.remaining());
    return false;
  }

  const QByteArray raw = QByteArray::fromRawData(
      reinterpret_cast<const char *>(r.data() + r.pos()), int(storedSize));
  r.skip(storedSize);
  if (!r.ok()) {
    if (error)
      *error = QStringLiteral("chunk '%1' payload truncated").arg(tagToString(tag));
    return false;
  }

  if (uint32_t(crc32(0u, reinterpret_cast<const Bytef *>(raw.constData()), uLong(raw.size())))
      != storedCrc) {
    if (error)
      *error = QStringLiteral("chunk '%1' failed its stored CRC").arg(tagToString(tag));
    return false;
  }

  if (flags & kChunkCompressed) {
    QByteArray out(int(rawSize), '\0');
    uLongf outLen = uLongf(rawSize);
    if (uncompress(reinterpret_cast<Bytef *>(out.data()), &outLen,
                   reinterpret_cast<const Bytef *>(raw.constData()), uLong(raw.size()))
        != Z_OK || outLen != uLongf(rawSize)) {
      if (error)
        *error = QStringLiteral("chunk '%1' failed to inflate").arg(tagToString(tag));
      return false;
    }
    payload = out;
  } else {
    payload = raw;
  }

  if (uint32_t(crc32(0u, reinterpret_cast<const Bytef *>(payload.constData()), uLong(payload.size())))
      != rawCrc) {
    if (error)
      *error = QStringLiteral("chunk '%1' failed its payload CRC").arg(tagToString(tag));
    return false;
  }
  return true;
}

QString tagToString(const uint32_t tag) {
  char b[5] = {char(tag & 0xff), char((tag >> 8) & 0xff), char((tag >> 16) & 0xff),
               char((tag >> 24) & 0xff), 0};
  return QString::fromLatin1(b);
}

bool saveMap(const QString &path, const Document &doc, QString *error) {
  QByteArray body;

  {
    Writer meta;
    meta.str(doc.meta.mapName);
    meta.str(doc.meta.author);
    meta.str(doc.meta.generator);
    meta.str(doc.meta.sourceFormat);
    meta.u64(doc.meta.createdMs);
    meta.u64(doc.meta.modifiedMs);
    Writer chunks;
    writeChunk(chunks, tag::Header, 1, meta.data());

    Writer meshes;
    meshes.u32(uint32_t(doc.meshes.size()));
    for (const auto &m: doc.meshes)
      writeMeshRecord(meshes, m);
    writeChunk(chunks, tag::Meshes, 2, meshes.data());

    Writer nodes;
    nodes.u32(uint32_t(doc.nodes.size()));
    for (const auto &n: doc.nodes)
      writeNodeRecord(nodes, n);
    writeChunk(chunks, tag::Nodes, 7, nodes.data());

    Writer ents;
    ents.u32(uint32_t(doc.entities.size()));
    for (const auto &e: doc.entities)
      writeEntityRecord(ents, e);
    writeChunk(chunks, tag::Entities, 1, ents.data());

    Writer scene;
    writeSceneState(scene, doc.scene);
    writeChunk(chunks, tag::Scene, 3, scene.data());

    Writer cams;
    writeCameraState(cams, doc.cameras);
    writeChunk(chunks, tag::Cameras, 1, cams.data());

    uint32_t chunkCount = 6u;

    if (!doc.groups.empty()) {
      Writer grps;
      grps.u32(uint32_t(doc.groups.size()));
      grps.u32(doc.activeGroup);
      for (const auto &g: doc.groups) {
        grps.u32(g.id);
        grps.str(g.name);
        grps.u32(uint32_t(g.color.rgba()));
        grps.u32(g.flags);
      }
      writeChunk(chunks, tag::Groups, 1, grps.data());
      ++chunkCount;
    }
    if (!doc.faceGroups.empty()) {
      Writer fg;
      fg.u32(uint32_t(doc.faceGroups.size()));
      for (const auto &g: doc.faceGroups) {
        fg.u32(g.id);
        fg.str(g.name);
        fg.u32(uint32_t(g.color.rgba()));
        fg.u32(g.flags);
      }
      writeChunk(chunks, tag::FaceGroups, 1, fg.data());
      ++chunkCount;
    }
    if (!doc.procObjects.empty()) {
      Writer pr;
      pr.u32(uint32_t(doc.procObjects.size()));
      for (const auto &p: doc.procObjects) {
        pr.u32(p.id);
        pr.str(p.type);
        pr.str(p.name);
        pr.blob(p.params);
      }
      writeChunk(chunks, tag::ProcObjects, 1, pr.data());
      ++chunkCount;
    }
    if (doc.mirror.lineValid || !doc.mirror.links.empty()) {
      Writer mr;
      mr.u8(doc.mirror.lineValid ? 1 : 0);
      mr.u8(doc.mirror.axis);
      mr.f64(doc.mirror.d);
      mr.u32(uint32_t(doc.mirror.links.size()));
      for (const auto &l: doc.mirror.links) {
        mr.u32(l.id);
        mr.u8(l.axis);
        mr.f64(l.d);
        mr.u8(l.flags);
        mr.u32(l.set);
      }
      mr.u32(uint32_t(doc.mirror.sets.size()));
      for (const auto &st: doc.mirror.sets) {
        mr.u32(st.id);
        mr.u8(st.flags);
        mr.str(st.name);
      }
      mr.u32(doc.mirror.activeSet);
      writeChunk(chunks, tag::Mirror, 2, mr.data());
      ++chunkCount;
    }
    if (!doc.connections.empty()) {
      Writer io;
      io.u32(uint32_t(doc.connections.size()));
      for (const auto &e: doc.connections) {
        io.u32(e.entityId);
        io.u32(uint32_t(e.connections.size()));
        for (const auto &c: e.connections) {
          io.str(c.output);
          io.str(c.target);
          io.str(c.input);
          io.str(c.param);
          io.f32(c.delay);
          io.u32(uint32_t(c.times));
          io.u32(c.flags);
        }
      }
      writeChunk(chunks, tag::Connections, 1, io.data());
      ++chunkCount;
    }
    if (!doc.ioGraphLayout.empty()) {
      Writer gl;
      gl.u32(uint32_t(doc.ioGraphLayout.size()));
      for (const auto &g: doc.ioGraphLayout) {
        gl.u32(g.entityId);
        gl.vec2(g.pos);
      }
      writeChunk(chunks, tag::IOGraphLayout, 1, gl.data());
      ++chunkCount;
    }
    if (!doc.layouts.isEmpty()) {
      Writer lay;
      lay.blob(doc.layouts);
      writeChunk(chunks, tag::Layouts, 1, lay.data());
      ++chunkCount;
    }
    if (!doc.modifiers.isEmpty()) {
      Writer mods;
      mods.blob(doc.modifiers);
      writeChunk(chunks, tag::Modifiers, 1, mods.data());
      ++chunkCount;
    }
    if (!doc.paths.isEmpty()) {
      Writer paths;
      paths.blob(doc.paths);
      writeChunk(chunks, tag::Paths, 1, paths.data());
      ++chunkCount;
    }
    if (!doc.levelSettings.isEmpty()) {
      Writer cfg;
      cfg.blob(doc.levelSettings);
      writeChunk(chunks, tag::LevelSettings, 1, cfg.data());
      ++chunkCount;
    }
    if (!doc.materials.isEmpty()) {
      Writer mat;
      mat.blob(doc.materials);
      writeChunk(chunks, tag::Materials, 1, mat.data());
      ++chunkCount;
    }

    Writer file;
    file.u32(kMagic);
    file.u32(kEndianLittle);
    file.u16(kVersionMajor);
    file.u16(kVersionMinor);
    file.u32(kFileHeaderSize);
    file.u32(chunkCount);
    file.u64(uint64_t(chunks.size()));
    file.u32(uint32_t(crc32(0u, reinterpret_cast<const Bytef *>(chunks.data().constData()),
                            uLong(chunks.size()))));
    file.u32(0u);
    body = file.data() + chunks.data();
  }

  QFile f(path);
  if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    if (error)
      *error = QStringLiteral("cannot open '%1' for writing").arg(path);
    return false;
  }
  const qint64 written = f.write(body);
  f.close();

  if (written != body.size()) {
    if (error)
      *error = QStringLiteral("wrote %1 of %2 bytes to '%3'").arg(written).arg(body.size()).arg(path);
    return false;
  }

  qCDebug(fmapLog).noquote() << QStringLiteral("[fmap] saved %1  meshes %2  nodes %3  entities %4  %5 KB")
                                   .arg(path).arg(doc.meshes.size()).arg(doc.nodes.size())
                                   .arg(doc.entities.size()).arg(body.size() / 1024);
  return true;
}

bool loadMap(const QString &path, Document &doc, QString *error) {
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly)) {
    if (error)
      *error = QStringLiteral("cannot open '%1'").arg(path);
    return false;
  }
  const QByteArray raw = f.readAll();
  f.close();

  Reader top(raw);
  const uint32_t magic = top.u32();
  if (magic != kMagic) {
    if (error)
      *error = QStringLiteral("'%1' is not a gHammer map (magic %2)").arg(path).arg(magic, 0, 16);
    return false;
  }
  const uint32_t endian = top.u32();
  if (endian == kEndianSwapped) {
    if (error)
      *error = QStringLiteral("'%1' is big-endian; this build reads little-endian only").arg(path);
    return false;
  }
  if (endian != kEndianLittle) {
    if (error)
      *error = QStringLiteral("'%1' has an unknown endian tag").arg(path);
    return false;
  }
  const uint16_t major = top.u16();
  const uint16_t minor = top.u16();
  const uint32_t headerSize = top.u32();
  const uint32_t chunkCount = top.u32();
  const uint64_t contentSize = top.u64();
  const uint32_t contentCrc = top.u32();
  top.u32();

  if (!top.ok() || headerSize < kFileHeaderSize) {
    if (error)
      *error = QStringLiteral("'%1' has a malformed file header").arg(path);
    return false;
  }
  if (major > kVersionMajor) {
    if (error)
      *error = QStringLiteral("'%1' is format %2.%3, this build understands up to %4.%5")
                   .arg(path).arg(major).arg(minor).arg(kVersionMajor).arg(kVersionMinor);
    return false;
  }

  if (headerSize > kFileHeaderSize)
    top.skip(headerSize - kFileHeaderSize);
  const QByteArray content = raw.mid(int(top.pos()),
                                    int(std::min<uint64_t>(contentSize, top.remaining())));
  if (uint32_t(crc32(0u, reinterpret_cast<const Bytef *>(content.constData()),
                     uLong(content.size()))) != contentCrc) {
    if (error)
      *error = QStringLiteral("'%1' content CRC mismatch (truncated or corrupted file)").arg(path);
    return false;
  }

  Reader r(content);
  int seen = 0;
  while (r.remaining() >= 32 && uint32_t(seen) < chunkCount) {
    uint32_t t = 0;
    uint32_t v = 0;
    QByteArray payload;
    if (!readChunkHeader(r, t, v, payload, error))
      return false;

    Reader p(payload);
    switch (t) {
      case tag::Header:
        doc.meta.mapName = p.str();
        doc.meta.author = p.str();
        doc.meta.generator = p.str();
        doc.meta.sourceFormat = p.str();
        doc.meta.createdMs = p.u64();
        doc.meta.modifiedMs = p.u64();
        break;
      case tag::Meshes: {
        const uint32_t count = p.u32();
        if (!countFits(p, count, 12)) {
          if (error)
            *error = QStringLiteral("'%1' has a corrupt chunk count").arg(path);
          return false;
        }
        doc.meshes.reserve(count);
        for (uint32_t i = 0; i < count && p.ok(); ++i)
          doc.meshes.push_back(readMeshRecord(p, v));
        break;
      }
      case tag::Nodes: {
        const uint32_t count = p.u32();
        if (!countFits(p, count, 8)) {
          if (error)
            *error = QStringLiteral("'%1' has a corrupt chunk count").arg(path);
          return false;
        }
        doc.nodes.reserve(count);
        for (uint32_t i = 0; i < count && p.ok(); ++i)
          doc.nodes.push_back(readNodeRecord(p, v));
        break;
      }
      case tag::Entities: {
        const uint32_t count = p.u32();
        if (!countFits(p, count, 8)) {
          if (error)
            *error = QStringLiteral("'%1' has a corrupt chunk count").arg(path);
          return false;
        }
        doc.entities.reserve(count);
        for (uint32_t i = 0; i < count && p.ok(); ++i)
          doc.entities.push_back(readEntityRecord(p));
        break;
      }
      case tag::Scene:
        doc.scene = readSceneState(p, v);
        break;
      case tag::Cameras:
        doc.cameras = readCameraState(p);
        break;
      case tag::Groups: {
        const uint32_t count = p.u32();
        doc.activeGroup = p.u32();
        if (!countFits(p, count, 12)) {
          if (error)
            *error = QStringLiteral("'%1' has a corrupt group count").arg(path);
          return false;
        }
        for (uint32_t i = 0; i < count && p.ok(); ++i) {
          GroupRecord g;
          g.id = p.u32();
          g.name = p.str();
          g.color = QColor::fromRgba(p.u32());
          g.flags = p.u32();
          doc.groups.push_back(std::move(g));
        }
        break;
      }
      case tag::Layouts:
        doc.layouts = p.blob();
        break;
      case tag::Modifiers:
        doc.modifiers = p.blob();
        break;
      case tag::Paths:
        doc.paths = p.blob();
        break;
      case tag::LevelSettings:
        doc.levelSettings = p.blob();
        break;
      case tag::Materials:
        doc.materials = p.blob();
        break;
      case tag::Mirror: {
        doc.mirror.lineValid = p.u8() != 0;
        doc.mirror.axis = uint8_t(std::min<int>(2, p.u8()));
        doc.mirror.d = p.f64();
        const uint32_t count = p.u32();
        if (!countFits(p, count, 18))
          break;
        for (uint32_t i = 0; i < count && p.ok(); ++i) {
          MirrorLinkRecord rec;
          rec.id = p.u32();
          rec.axis = uint8_t(std::min<int>(2, p.u8()));
          rec.d = p.f64();
          rec.flags = p.u8();
          rec.set = p.u32();
          if (p.ok())
            doc.mirror.links.push_back(rec);
        }
        const uint32_t sets = p.u32();
        if (!countFits(p, sets, 9))
          break;
        for (uint32_t i = 0; i < sets && p.ok(); ++i) {
          MirrorSetRecord st;
          st.id = p.u32();
          st.flags = p.u8();
          st.name = p.str();
          if (p.ok())
            doc.mirror.sets.push_back(st);
        }
        doc.mirror.activeSet = p.u32();
        if (!p.ok())
          doc.mirror = {};
        break;
      }
      case tag::ProcObjects: {
        const uint32_t count = p.u32();
        if (!countFits(p, count, 16))
          break;
        for (uint32_t i = 0; i < count && p.ok(); ++i) {
          ProcObjectRecord rec;
          rec.id = p.u32();
          rec.type = p.str();
          rec.name = p.str();
          rec.params = p.blob();
          if (p.ok())
            doc.procObjects.push_back(std::move(rec));
        }
        break;
      }
      case tag::Connections: {
        const uint32_t count = p.u32();
        if (!countFits(p, count, 8))
          break;
        for (uint32_t i = 0; i < count && p.ok(); ++i) {
          EntityConnectionsRecord rec;
          rec.entityId = p.u32();
          const uint32_t n = p.u32();
          if (!countFits(p, n, 28))
            break;
          for (uint32_t k = 0; k < n && p.ok(); ++k) {
            ConnectionRecord c;
            c.output = p.str();
            c.target = p.str();
            c.input = p.str();
            c.param = p.str();
            c.delay = p.f32();
            c.times = int32_t(p.u32());
            c.flags = p.u32();
            if (p.ok())
              rec.connections.push_back(std::move(c));
          }
          if (p.ok())
            doc.connections.push_back(std::move(rec));
        }
        break;
      }
      case tag::IOGraphLayout: {
        const uint32_t count = p.u32();
        if (!countFits(p, count, 12))
          break;
        for (uint32_t i = 0; i < count && p.ok(); ++i) {
          IOGraphPosRecord g;
          g.entityId = p.u32();
          g.pos = p.vec2();
          if (p.ok())
            doc.ioGraphLayout.push_back(g);
        }
        break;
      }
      case tag::FaceGroups: {
        const uint32_t count = p.u32();
        if (!countFits(p, count, 16))
          break;
        for (uint32_t i = 0; i < count && p.ok(); ++i) {
          FaceGroupRecord g;
          g.id = p.u32();
          g.name = p.str();
          g.color = QColor::fromRgba(p.u32());
          g.flags = p.u32();
          if (p.ok())
            doc.faceGroups.push_back(std::move(g));
        }
        break;
      }
      default:
        qCDebug(fmapLog).noquote()
            << QStringLiteral("[fmap] skipping unknown chunk '%1' (%2 bytes)").arg(tagToString(t))
                   .arg(payload.size());
        break;
    }
    // Scene / camera chunks are read tolerantly (older files are shorter); geometry chunks strictly (a partial
    // mesh is worse than none).
    const bool strict = t == tag::Meshes || t == tag::Nodes || t == tag::Entities;
    if (strict && !p.ok()) {
      if (error)
        *error = QStringLiteral("'%1': chunk '%2' is corrupt or truncated").arg(path, tagToString(t));
      return false;
    }
    ++seen;
  }

  if (!r.ok()) {
    if (error)
      *error = QStringLiteral("'%1' ended mid-chunk").arg(path);
    return false;
  }

  qCDebug(fmapLog).noquote()
      << QStringLiteral("[fmap] loaded %1  format %2.%3  meshes %4  nodes %5  entities %6")
             .arg(path).arg(major).arg(minor).arg(doc.meshes.size()).arg(doc.nodes.size())
             .arg(doc.entities.size());
  return true;
}

} // namespace fmap
