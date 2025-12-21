#pragma once

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

#include <QByteArray>
#include <QColor>
#include <QString>
#include <glm/glm.hpp>

#include "engine/mesh.h"

namespace fmap {

// The only map extension opened or written.
inline constexpr const char *kFileSuffix = "ghmap";
inline constexpr const char *kFileFilter = "gHammer map (*.ghmap)";
inline bool isMapPath(const QString &path) {
  return path.endsWith(QLatin1String(".ghmap"), Qt::CaseInsensitive);
}
inline bool isWritablePath(const QString &path) {
  return path.endsWith(QLatin1String(".ghmap"), Qt::CaseInsensitive);
}
inline QString withFileSuffix(QString path) {
  const int slash = int(std::max(path.lastIndexOf(QLatin1Char('/')), path.lastIndexOf(QLatin1Char('\\'))));
  if (path.lastIndexOf(QLatin1Char('.')) <= slash)
    path += QLatin1String(".ghmap");
  return path;
}

inline constexpr uint32_t fourcc(char a, char b, char c, char d) {
  return uint32_t(uint8_t(a)) | (uint32_t(uint8_t(b)) << 8) | (uint32_t(uint8_t(c)) << 16)
      | (uint32_t(uint8_t(d)) << 24);
}

// Little-endian on disk. The endian tag follows the magic, so a byte-swapped file is rejected outright.
inline constexpr uint32_t kEndianLittle = 0x01020304u;
inline constexpr uint32_t kEndianSwapped = 0x04030201u;

inline constexpr uint32_t kMagic = fourcc('F', 'M', 'A', 'P');
inline constexpr uint16_t kVersionMajor = 1;
inline constexpr uint16_t kVersionMinor = 0;

inline constexpr uint32_t kChunkCompressed = 1u << 0;
inline constexpr uint32_t kChunkHeaderSize = 32;
inline constexpr uint32_t kFileHeaderSize = 32;

namespace tag {
inline constexpr uint32_t Header = fourcc('H', 'D', 'R', 'R');
inline constexpr uint32_t Meshes = fourcc('M', 'E', 'S', 'H');
inline constexpr uint32_t Nodes = fourcc('N', 'O', 'D', 'E');
inline constexpr uint32_t Entities = fourcc('E', 'N', 'T', 'Y');
inline constexpr uint32_t Scene = fourcc('S', 'C', 'N', 'E');
inline constexpr uint32_t Cameras = fourcc('C', 'A', 'M', 'S');
inline constexpr uint32_t Groups = fourcc('G', 'R', 'P', 'S');
inline constexpr uint32_t Layouts = fourcc('L', 'A', 'Y', 'O');
inline constexpr uint32_t FaceGroups = fourcc('F', 'G', 'R', 'P');
inline constexpr uint32_t ProcObjects = fourcc('P', 'R', 'O', 'C');
inline constexpr uint32_t Connections = fourcc('I', 'O', 'C', 'N');
inline constexpr uint32_t IOGraphLayout = fourcc('I', 'O', 'G', 'L');
inline constexpr uint32_t Materials = fourcc('M', 'A', 'T', 'L');
inline constexpr uint32_t Mirror = fourcc('M', 'I', 'R', 'R');
inline constexpr uint32_t Modifiers = fourcc('M', 'O', 'D', 'S');
inline constexpr uint32_t Paths = fourcc('P', 'A', 'T', 'H');
inline constexpr uint32_t LevelSettings = fourcc('L', 'C', 'F', 'G');
} // namespace tag

// Mirrors eng::FaceTexBasis plus the material name and picking id, for a lossless round trip.
struct FaceRecord {
  uint32_t indexOffset = 0;
  uint32_t indexCount = 0;
  QString material;
  eng::FaceTexBasis basis {};
  uint32_t pickingId = 0;
  eng::FaceGroupSet groups; // MESH v2
};

// Any vertex / index count; faces have their own index range (triangles, quads, n-gon fans).
struct MeshRecord {
  uint32_t id = 0;
  QString name;
  std::vector<eng::Vertex> vertices;
  std::vector<uint32_t> indices;
  std::vector<FaceRecord> faces;
};

struct NodeRecord {
  uint32_t id = 0;
  uint32_t meshId = 0;
  QString name;
  glm::vec3 position {0.0f};
  glm::vec3 rotation {0.0f};
  glm::vec3 scale {1.0f};
  // Chunk version 2+: editor state.
  uint32_t flags = 0;   // 1 = hidden, 2 = locked, 4 = smooth shading
  uint32_t group = 0;   // 0 = not grouped
  // Chunk version 3+: the brush entity this brush belongs to (0 = world).
  uint32_t brushEntity = 0;
  // Chunk version 4+: the generated object (PROC chunk) it belongs to.
  uint32_t procObject = 0;
  // Chunk version 5+: the mirror link (MIRR chunk) and its side (0 / 1).
  uint32_t mirrorLink = 0;
  uint8_t mirrorSide = 0;
  // Chunk version 6+: a brush's rotation (GameObject::orientation), x y z w.
  glm::vec4 orientation {0.0f, 0.0f, 0.0f, 1.0f};
  // Chunk version 7+: the modified object (MODS) this brush is a source of.
  uint32_t modHost = 0;
};

// Entity properties differ per type: a tagged-value list, so new keys need no format change.
enum class ValueKind : uint8_t {
  Bool = 1,
  Int32 = 2,
  Float = 3,
  Vec2 = 4,
  Vec3 = 5,
  Vec4 = 6,
  Color = 7,
  String = 8,
};

struct Value {
  ValueKind kind {ValueKind::Float};
  bool boolValue = false;
  int32_t intValue = 0;
  float floatValue = 0.0f;
  glm::vec4 vec {0.0f};
  QString string;

  static Value makeBool(bool v);
  static Value makeInt(int32_t v);
  static Value makeFloat(float v);
  static Value makeVec2(const glm::vec2 &v);
  static Value makeVec3(const glm::vec3 &v);
  static Value makeVec4(const glm::vec4 &v);
  static Value makeColor(const QColor &c);
  static Value makeString(const QString &v);

  [[nodiscard]] double asDouble(double fallback = 0.0) const;
  [[nodiscard]] bool asBool(bool fallback = false) const;
  [[nodiscard]] glm::vec3 asVec3(const glm::vec3 &fallback = glm::vec3(0.0f)) const;
};

using PropertyList = std::vector<std::pair<QString, Value>>;

struct EntityRecord {
  uint32_t id = 0;
  QString type;
  QString name;
  glm::vec3 position {0.0f};
  PropertyList properties;

  [[nodiscard]] const Value *find(const QString &key) const;
  void set(const QString &key, const Value &value);
};

struct SunState {
  bool enabled = true;
  glm::vec3 direction {-0.288457f, -0.537300f, 0.792528f};
  glm::vec3 color {1.0f, 0.969f, 0.902f};
  float intensity = 5.0f;
  float azimuth = 56.0f;
  float elevation = 32.5f;
};

struct AmbientState {
  glm::vec3 color {37.0f / 255.0f, 44.0f / 255.0f, 57.0f / 255.0f};  // #252c39
  float intensity = 0.7f;
};

struct FogState {
  float density = 0.0006f;
  glm::vec3 color {0.13f, 0.16f, 0.24f};
};

struct ShadowState {
  bool enabled = true;
  float bias = 0.0016f;
  float normalBias = 1.14f;
  float radius = 0.9f;
  float strength = 1.0f; // < 1 lets a share of the light through every shadow
  float range = 440.0f;
  int32_t mapSize = 4096;
};

struct SsaoState {
  bool enabled = false;
  float radius = 2.0f;
  float intensity = 1.0f;
};

struct RenderState {
  int32_t renderMode = 1;
  int32_t msaaSamples = 2;
  float exposure = 1.0f;  // as Godot's Environment
  float wireframeThickness = 2.5f;
  int32_t gridLevel = 6;
};

// Every non-geometric setting the renderer carries.
struct SceneState {
  SunState sun;
  AmbientState ambient;
  FogState fog;
  ShadowState shadows;
  SsaoState ssao;
  RenderState render;
  QString sky;  // skybox ref ("pack/Name", a Panorama map), empty = none (SCNE v2)
  QString lut;  // colour LUT ref ("pack/Name" in a "lut" pack of the asset server), empty = none (SCNE v3)
};

struct PerspectiveCameraState {
  glm::vec3 position {0.0f, 3.0f, 0.0f};
  float yaw = 0.0f;
  float pitch = 0.0f;
  float fov = 45.0f;
  float nearPlane = 0.1f;
  float farPlane = 15000.0f;
  float speed = 55.0f;
};

struct OrthoViewState {
  int32_t perspective = 0;
  double centerA = 0.0;
  double centerB = 0.0;
  double pxPerWorldUnit = 0.05;
  int32_t gridLevel = 6;
  bool showGrid = true;
};

struct CameraState {
  PerspectiveCameraState perspective3d;
  std::vector<OrthoViewState> views2d;
};

struct Meta {
  QString mapName = QStringLiteral("Untitled");
  QString author;
  QString generator = QStringLiteral("level-editor");
  QString sourceFormat;
  uint64_t createdMs = 0;
  uint64_t modifiedMs = 0;
};

// A mesh group (GRPS chunk). Id 0 is worldspawn.
struct GroupRecord {
  uint32_t id = 0;
  QString name;
  QColor color {120, 160, 220};
  uint32_t flags = 0; // 1 = hidden, 2 = locked
};

// Brushes rebuilt by the generator named `type`; nodes point at it by id; params is opaque JSON.
struct ProcObjectRecord {
  uint32_t id = 0;
  QString type;
  QString name;
  QByteArray params;
};

// Nodes point at a link by id; plane p[axis] = d. Each link belongs to a mirror set; the sets table and the
// active set follow the links.
struct MirrorLinkRecord {
  uint32_t id = 0;
  uint8_t axis = 0;
  double d = 0.0;
  uint8_t flags = 0; // 1 = mirror textures
  uint32_t set = 0;
};
struct MirrorSetRecord {
  uint32_t id = 0;
  uint8_t flags = 0; // 1 = hide the mirrored copies
  QString name;
};
struct MirrorRecord {
  bool lineValid = false;
  uint8_t axis = 0;
  double d = 0.0;
  std::vector<MirrorLinkRecord> links;
  std::vector<MirrorSetRecord> sets;
  uint32_t activeSet = 0;
};

// A face group name (FGRP chunk); faces reference it by id.
struct FaceGroupRecord {
  uint32_t id = 0;
  QString name;
  QColor color;
  uint32_t flags = 0;
};

// Kept out of ENTY so builds without I/O still read the entities (skipping the unknown chunk).
inline constexpr uint32_t kConnectionDisabled = 1u << 0;
struct ConnectionRecord {
  QString output;
  QString target;
  QString input;
  QString param;
  float delay = 0.0f;
  int32_t times = -1; // -1 = unlimited
  uint32_t flags = 0; // kConnectionDisabled
};
struct EntityConnectionsRecord {
  uint32_t entityId = 0;
  std::vector<ConnectionRecord> connections;
};

// View state, not undoable, like the cameras.
struct IOGraphPosRecord {
  uint32_t entityId = 0;
  glm::vec2 pos {0.0f};
};

struct Document {
  Meta meta;
  // MATL: material overrides (map layer, variants, project pack, texture ids, resolved snapshot), compact JSON of
  // eng::MaterialOverrides::toJson. Never images, only refs.
  QByteArray materials;
  // IOGL: I/O Editor node positions.
  std::vector<IOGraphPosRecord> ioGraphLayout;
  // IOCN: entity outputs, by entity id.
  std::vector<EntityConnectionsRecord> connections;
  // FGRP: user face group names (the built-in categories are implicit).
  std::vector<FaceGroupRecord> faceGroups;
  // PROC: generated objects.
  std::vector<ProcObjectRecord> procObjects;
  // MIRR: the mirror line and links.
  MirrorRecord mirror;
  // GRPS: mesh groups and the active one. Empty = file from before groups.
  std::vector<GroupRecord> groups;
  uint32_t activeGroup = 0;
  // LAYO: Layout Designer documents, serialized by eng::layout (opaque here).
  QByteArray layouts;
  // MODS: hosts and stacks, compact JSON (eng::mods::toJson per host). Output is never stored, evaluated on load.
  QByteArray modifiers;
  // PATH: paths (eng::path::toJson per path), compact JSON.
  QByteArray paths;
  // LCFG: level-scope settings, compact JSON of the keys off their defaults.
  QByteArray levelSettings;
  std::vector<MeshRecord> meshes;
  std::vector<NodeRecord> nodes;
  std::vector<EntityRecord> entities;
  SceneState scene;
  CameraState cameras;
};

// A short read sets ok() false and returns zeros: a truncated file becomes an empty document, not a crash.
class Writer {
public:
  void u8(uint8_t v);
  void u16(uint16_t v);
  void u32(uint32_t v);
  void u64(uint64_t v);
  void f32(float v);
  void f64(double v);
  void vec2(const glm::vec2 &v);
  void vec3(const glm::vec3 &v);
  void vec4(const glm::vec4 &v);
  void dvec2(const glm::dvec2 &v);
  void dvec3(const glm::dvec3 &v);
  void str(const QString &v);
  void blob(const QByteArray &v);
  void raw(const QByteArray &v) { m_buf.append(v); }

  [[nodiscard]] const QByteArray &data() const { return m_buf; }
  [[nodiscard]] size_t size() const { return size_t(m_buf.size()); }

private:
  QByteArray m_buf;
};

class Reader {
public:
  explicit Reader(const QByteArray &buf);

  [[nodiscard]] bool ok() const { return m_ok; }
  void fail() { m_ok = false; }
  [[nodiscard]] size_t pos() const { return m_pos; }
  [[nodiscard]] const uint8_t *data() const { return m_data; }
  [[nodiscard]] size_t remaining() const;
  void skip(size_t n);

  uint8_t u8();
  uint16_t u16();
  uint32_t u32();
  uint64_t u64();
  float f32();
  double f64();
  glm::vec2 vec2();
  glm::vec3 vec3();
  glm::vec4 vec4();
  glm::dvec2 dvec2();
  glm::dvec3 dvec3();
  QString str();
  QByteArray blob();

private:
  const uint8_t *m_data {};
  size_t m_size = 0;
  size_t m_pos = 0;
  bool m_ok = true;
};

void writeValue(Writer &w, const Value &v);
Value readValue(Reader &r);
void writeProperties(Writer &w, const PropertyList &props);
PropertyList readProperties(Reader &r);

// Payload is zlib-compressed when it pays for itself.
void writeChunk(Writer &out, uint32_t tag, uint32_t version, const QByteArray &payload);
bool readChunkHeader(Reader &r, uint32_t &tag, uint32_t &version, QByteArray &payload,
                    QString *error = nullptr);

void writeMeshRecord(Writer &w, const MeshRecord &m);
MeshRecord readMeshRecord(Reader &r, uint32_t version = 2);
void writeNodeRecord(Writer &w, const NodeRecord &n);
NodeRecord readNodeRecord(Reader &r, uint32_t version = 1);
void writeEntityRecord(Writer &w, const EntityRecord &e);
EntityRecord readEntityRecord(Reader &r);
void writeSceneState(Writer &w, const SceneState &s);
SceneState readSceneState(Reader &r, uint32_t version);
void writeCameraState(Writer &w, const CameraState &c);
CameraState readCameraState(Reader &r);

bool saveMap(const QString &path, const Document &doc, QString *error = nullptr);
bool loadMap(const QString &path, Document &doc, QString *error = nullptr);

[[nodiscard]] QString tagToString(uint32_t tag);

} // namespace fmap
