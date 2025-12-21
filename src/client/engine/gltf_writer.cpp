#include "gltf_writer.h"

#include <QJsonArray>
#include <QJsonDocument>

#include <cstring>
#include <limits>

namespace eng::gltf {

namespace {

constexpr quint32 kMagic = 0x46546C67;     // "glTF"
constexpr quint32 kChunkJson = 0x4E4F534A; // "JSON"
constexpr quint32 kChunkBin = 0x004E4942;  // "BIN\0"
constexpr int kFloat = 5126;
constexpr int kUInt = 5125;
constexpr int kArrayBuffer = 34962;
constexpr int kElementArrayBuffer = 34963;

void putU32(QByteArray &out, quint32 v) {
  char b[4];
  std::memcpy(b, &v, 4);  // little endian hosts only (x86 / arm)
  out.append(b, 4);
}

QJsonArray arr(std::initializer_list<double> v) {
  QJsonArray a;
  for (const double d: v)
    a.append(d);
  return a;
}

struct Builder {
  QByteArray bin;
  QJsonArray views;
  QJsonArray accessors;

  int view(const void *data, qsizetype bytes, int target) {
    while (bin.size() % 4)
      bin.append('\0');
    QJsonObject v;
    v[QStringLiteral("buffer")] = 0;
    v[QStringLiteral("byteOffset")] = qint64(bin.size());
    v[QStringLiteral("byteLength")] = qint64(bytes);
    v[QStringLiteral("target")] = target;
    bin.append(static_cast<const char *>(data), bytes);
    views.append(v);
    return int(views.size()) - 1;
  }

  int accessor(int view, int componentType, qsizetype count, const char *type, const QJsonArray &mn = {},
               const QJsonArray &mx = {}) {
    QJsonObject a;
    a[QStringLiteral("bufferView")] = view;
    a[QStringLiteral("componentType")] = componentType;
    a[QStringLiteral("count")] = qint64(count);
    a[QStringLiteral("type")] = QString::fromLatin1(type);
    if (!mn.isEmpty()) {
      a[QStringLiteral("min")] = mn;
      a[QStringLiteral("max")] = mx;
    }
    accessors.append(a);
    return int(accessors.size()) - 1;
  }
};

} // namespace

QByteArray toGlb(const Document &doc) {
  Builder b;
  QJsonObject root;
  QJsonObject asset;
  asset[QStringLiteral("version")] = QStringLiteral("2.0");
  asset[QStringLiteral("generator")] = doc.generator;
  root[QStringLiteral("asset")] = asset;

  QJsonArray meshes;
  for (const Mesh &m: doc.meshes) {
    QJsonArray prims;
    for (const Primitive &p: m.primitives) {
      if (p.positions.empty() || p.indices.empty())
        continue;
      glm::vec3 mn(std::numeric_limits<float>::max()), mx(-std::numeric_limits<float>::max());
      for (const glm::vec3 &v: p.positions) {
        mn = glm::min(mn, v);
        mx = glm::max(mx, v);
      }
      QJsonObject attrs;
      const int pv = b.view(p.positions.data(), qsizetype(p.positions.size() * sizeof(glm::vec3)), kArrayBuffer);
      attrs[QStringLiteral("POSITION")] = b.accessor(pv, kFloat, qsizetype(p.positions.size()), "VEC3",
                                                     arr({mn.x, mn.y, mn.z}), arr({mx.x, mx.y, mx.z}));
      if (p.normals.size() == p.positions.size()) {
        const int nv = b.view(p.normals.data(), qsizetype(p.normals.size() * sizeof(glm::vec3)), kArrayBuffer);
        attrs[QStringLiteral("NORMAL")] = b.accessor(nv, kFloat, qsizetype(p.normals.size()), "VEC3");
      }
      if (p.uvs.size() == p.positions.size()) {
        const int uv = b.view(p.uvs.data(), qsizetype(p.uvs.size() * sizeof(glm::vec2)), kArrayBuffer);
        attrs[QStringLiteral("TEXCOORD_0")] = b.accessor(uv, kFloat, qsizetype(p.uvs.size()), "VEC2");
      }
      const int iv = b.view(p.indices.data(), qsizetype(p.indices.size() * sizeof(quint32)), kElementArrayBuffer);
      QJsonObject prim;
      prim[QStringLiteral("attributes")] = attrs;
      prim[QStringLiteral("indices")] = b.accessor(iv, kUInt, qsizetype(p.indices.size()), "SCALAR");
      prim[QStringLiteral("mode")] = 4;
      if (p.material >= 0)
        prim[QStringLiteral("material")] = p.material;
      prims.append(prim);
    }
    QJsonObject mesh;
    mesh[QStringLiteral("name")] = m.name;
    mesh[QStringLiteral("primitives")] = prims;
    meshes.append(mesh);
  }

  QJsonArray nodes;
  for (const Node &n: doc.nodes) {
    QJsonObject o;
    o[QStringLiteral("name")] = n.name;
    if (n.mesh >= 0)
      o[QStringLiteral("mesh")] = n.mesh;
    if (!n.children.empty()) {
      QJsonArray c;
      for (const int k: n.children)
        c.append(k);
      o[QStringLiteral("children")] = c;
    }
    if (n.translation != glm::vec3(0.0f))
      o[QStringLiteral("translation")] = arr({n.translation.x, n.translation.y, n.translation.z});
    if (!n.extras.isEmpty())
      o[QStringLiteral("extras")] = n.extras;
    nodes.append(o);
  }

  QJsonArray materials;
  for (const Material &m: doc.materials) {
    QJsonObject o;
    o[QStringLiteral("name")] = m.name;
    QJsonObject pbr;
    pbr[QStringLiteral("baseColorFactor")] = arr({m.baseColor.r, m.baseColor.g, m.baseColor.b, m.baseColor.a});
    if (m.baseColorTexture >= 0) {
      QJsonObject t;
      t[QStringLiteral("index")] = m.baseColorTexture;
      pbr[QStringLiteral("baseColorTexture")] = t;
    }
    pbr[QStringLiteral("metallicFactor")] = double(m.metallic);
    pbr[QStringLiteral("roughnessFactor")] = double(m.roughness);
    o[QStringLiteral("pbrMetallicRoughness")] = pbr;
    if (m.normalTexture >= 0) {
      QJsonObject t;
      t[QStringLiteral("index")] = m.normalTexture;
      t[QStringLiteral("scale")] = double(m.normalScale);
      o[QStringLiteral("normalTexture")] = t;
    }
    if (m.doubleSided)
      o[QStringLiteral("doubleSided")] = true;
    materials.append(o);
  }

  QJsonArray textures;
  for (const Texture &t: doc.textures) {
    QJsonObject o;
    o[QStringLiteral("source")] = t.image;
    o[QStringLiteral("sampler")] = 0;
    textures.append(o);
  }
  QJsonArray images;
  for (const Image &im: doc.images) {
    QJsonObject o;
    o[QStringLiteral("uri")] = im.uri;
    if (!im.name.isEmpty())
      o[QStringLiteral("name")] = im.name;
    images.append(o);
  }

  QJsonObject scene;
  QJsonArray roots;
  for (const int r: doc.roots)
    roots.append(r);
  scene[QStringLiteral("nodes")] = roots;
  root[QStringLiteral("scene")] = 0;
  root[QStringLiteral("scenes")] = QJsonArray {scene};
  root[QStringLiteral("nodes")] = nodes;
  if (!meshes.isEmpty())
    root[QStringLiteral("meshes")] = meshes;
  if (!materials.isEmpty())
    root[QStringLiteral("materials")] = materials;
  if (!textures.isEmpty()) {
    root[QStringLiteral("textures")] = textures;
    QJsonObject sampler;
    sampler[QStringLiteral("magFilter")] = 9729;  // linear
    sampler[QStringLiteral("minFilter")] = 9987;  // linear mipmap linear
    sampler[QStringLiteral("wrapS")] = 10497;     // repeat
    sampler[QStringLiteral("wrapT")] = 10497;
    root[QStringLiteral("samplers")] = QJsonArray {sampler};
  }
  if (!images.isEmpty())
    root[QStringLiteral("images")] = images;
  while (b.bin.size() % 4)
    b.bin.append('\0');
  if (!b.bin.isEmpty()) {
    QJsonObject buffer;
    buffer[QStringLiteral("byteLength")] = qint64(b.bin.size());
    root[QStringLiteral("buffers")] = QJsonArray {buffer};
    root[QStringLiteral("bufferViews")] = b.views;
    root[QStringLiteral("accessors")] = b.accessors;
  }

  QByteArray json = QJsonDocument(root).toJson(QJsonDocument::Compact);
  while (json.size() % 4)
    json.append(' ');

  QByteArray out;
  const quint32 total = 12 + 8 + quint32(json.size()) + (b.bin.isEmpty() ? 0 : 8 + quint32(b.bin.size()));
  out.reserve(qsizetype(total));
  putU32(out, kMagic);
  putU32(out, 2);
  putU32(out, total);
  putU32(out, quint32(json.size()));
  putU32(out, kChunkJson);
  out.append(json);
  if (!b.bin.isEmpty()) {
    putU32(out, quint32(b.bin.size()));
    putU32(out, kChunkBin);
    out.append(b.bin);
  }
  return out;
}

bool readGlb(const QByteArray &glb, QJsonObject *json, QByteArray *bin) {
  const auto u32 = [&glb](qsizetype at) {
    quint32 v = 0;
    std::memcpy(&v, glb.constData() + at, 4);
    return v;
  };
  if (glb.size() < 20 || u32(0) != kMagic || u32(4) != 2 || u32(8) != quint32(glb.size()))
    return false;
  const quint32 jl = u32(12);
  if (u32(16) != kChunkJson || 20 + qsizetype(jl) > glb.size())
    return false;
  if (json) {
    QJsonParseError err {};
    const QJsonDocument d = QJsonDocument::fromJson(glb.mid(20, jl), &err);
    if (err.error != QJsonParseError::NoError || !d.isObject())
      return false;
    *json = d.object();
  }
  const qsizetype at = 20 + jl;
  if (bin) {
    bin->clear();
    if (at + 8 <= glb.size() && u32(at + 4) == kChunkBin)
      *bin = glb.mid(at + 8, u32(at));
  }
  return true;
}

} // namespace eng::gltf
