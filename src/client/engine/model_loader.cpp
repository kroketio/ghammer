#include "model_loader.h"

#include <QDir>
#include <QFile>
#include <QImage>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QPointer>
#include <QThreadPool>

#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include "client/vulkan/vk_shared.h"

namespace eng {

static QLoggingCategory modelLog("eng.model", QtInfoMsg);

size_t ModelData::triangleCount() const {
  size_t n = 0;
  for (const auto &s: subMeshes)
    n += s.indices.size() / 3;
  return n;
}

ModelCache &ModelCache::instance() {
  static ModelCache c;
  return c;
}

QStringList ModelCache::fileFilters() {
  return {QStringLiteral("*.obj"), QStringLiteral("*.fbx"), QStringLiteral("*.gltf"), QStringLiteral("*.glb"),
          QStringLiteral("*.dae"), QStringLiteral("*.3ds"), QStringLiteral("*.ply"), QStringLiteral("*.stl"),
          QStringLiteral("*.blend")};
}

namespace {

// Embedded textures (GLB, FBX) are written out once next to the cache, to share the file-material path.
QString extractEmbedded(const aiScene *scene, const char *ref, const QString &modelPath) {
  const aiTexture *tex = scene->GetEmbeddedTexture(ref);
  if (!tex)
    return {};
  const QString dir = QDir::tempPath() + QStringLiteral("/editor_model_textures");
  QDir().mkpath(dir);
  const QString hint = QString::fromLatin1(tex->achFormatHint).isEmpty() ? QStringLiteral("png")
                                                                          : QString::fromLatin1(tex->achFormatHint);
  const QString out = QStringLiteral("%1/%2_%3.%4")
                          .arg(dir)
                          .arg(qHash(modelPath), 0, 16)
                          .arg(QString::fromLatin1(ref).remove(QLatin1Char('*')).remove(QLatin1Char('/')))
                          .arg(tex->mHeight == 0 ? hint : QStringLiteral("png"));
  if (QFileInfo::exists(out))
    return out;
  if (tex->mHeight == 0) {
    QFile f(out);
    if (!f.open(QIODevice::WriteOnly))
      return {};
    f.write(reinterpret_cast<const char *>(tex->pcData), qint64(tex->mWidth));
  } else {
    QImage img(int(tex->mWidth), int(tex->mHeight), QImage::Format_RGBA8888);
    for (unsigned y = 0; y < tex->mHeight; ++y)
      for (unsigned x = 0; x < tex->mWidth; ++x) {
        const aiTexel &t = tex->pcData[y * tex->mWidth + x];
        img.setPixel(int(x), int(y), qRgba(t.r, t.g, t.b, t.a));
      }
    if (!img.save(out))
      return {};
  }
  return out;
}

thread_local const aiScene *g_scene = nullptr; // per import (loads run on worker threads)
thread_local QString g_modelPath;

QString texturePath(const aiMaterial *mat, aiTextureType type, const QDir &dir) {
  aiString p;
  if (mat->GetTextureCount(type) == 0 || mat->GetTexture(type, 0, &p) != AI_SUCCESS)
    return {};
  QString s = QString::fromUtf8(p.C_Str());
  if (g_scene && g_scene->GetEmbeddedTexture(p.C_Str()))
    return extractEmbedded(g_scene, p.C_Str(), g_modelPath);
  if (s.startsWith(QLatin1Char('*')))
    return {};
  s.replace(QLatin1Char('\\'), QLatin1Char('/'));
  QFileInfo fi(s);
  if (fi.isRelative())
    fi = QFileInfo(dir.filePath(s));
  if (!fi.exists()) // models often reference textures next to themselves
    fi = QFileInfo(dir.filePath(QFileInfo(s).fileName()));
  return fi.exists() ? fi.absoluteFilePath() : QString();
}

void walk(const aiScene *scene, const aiNode *node, const aiMatrix4x4 &parent, const QDir &dir, float unitScale,
          ModelData &out) {
  const aiMatrix4x4 xf = parent * node->mTransformation;
  aiMatrix3x3 nxf(xf);
  nxf.Inverse().Transpose();
  for (unsigned m = 0; m < node->mNumMeshes; ++m) {
    const aiMesh *mesh = scene->mMeshes[node->mMeshes[m]];
    if (!mesh->HasFaces() || !mesh->HasPositions())
      continue;
    ModelData::SubMesh sub;
    sub.vertices.reserve(mesh->mNumVertices);
    for (unsigned v = 0; v < mesh->mNumVertices; ++v) {
      const aiVector3D p = xf * mesh->mVertices[v];
      Vertex vx {};
      vx.position = glm::vec3(p.x, p.y, p.z) * unitScale;
      if (mesh->HasNormals()) {
        const aiVector3D n = (nxf * mesh->mNormals[v]).Normalize();
        vx.normal = glm::vec3(n.x, n.y, n.z);
      }
      if (mesh->HasTextureCoords(0))
        vx.texCoord = glm::vec2(mesh->mTextureCoords[0][v].x, mesh->mTextureCoords[0][v].y);
      if (mesh->HasTangentsAndBitangents()) {
        const aiVector3D t = (nxf * mesh->mTangents[v]).Normalize();
        const aiVector3D b = (nxf * mesh->mBitangents[v]).Normalize();
        vx.tangent = glm::vec3(t.x, t.y, t.z);
        vx.bitangent = glm::vec3(b.x, b.y, b.z);
      }
      out.boundsMin = glm::min(out.boundsMin, vx.position);
      out.boundsMax = glm::max(out.boundsMax, vx.position);
      sub.vertices.push_back(vx);
    }
    for (unsigned f = 0; f < mesh->mNumFaces; ++f) {
      const aiFace &face = mesh->mFaces[f];
      if (face.mNumIndices != 3)
        continue;
      sub.indices.insert(sub.indices.end(), {face.mIndices[0], face.mIndices[1], face.mIndices[2]});
    }
    if (mesh->mMaterialIndex < scene->mNumMaterials) {
      const aiMaterial *mat = scene->mMaterials[mesh->mMaterialIndex];
      sub.albedoPath = texturePath(mat, aiTextureType_BASE_COLOR, dir);
      if (sub.albedoPath.isEmpty())
        sub.albedoPath = texturePath(mat, aiTextureType_DIFFUSE, dir);
      sub.normalPath = texturePath(mat, aiTextureType_NORMALS, dir);
      if (sub.normalPath.isEmpty())
        sub.normalPath = texturePath(mat, aiTextureType_HEIGHT, dir);
      aiColor4D c;
      if (aiGetMaterialColor(mat, AI_MATKEY_COLOR_DIFFUSE, &c) == AI_SUCCESS)
        sub.baseColor = glm::vec3(c.r, c.g, c.b);
    }
    if (!sub.indices.empty())
      out.subMeshes.push_back(std::move(sub));
  }
  for (unsigned c = 0; c < node->mNumChildren; ++c)
    walk(scene, node->mChildren[c], xf, dir, unitScale, out);
}

} // namespace

std::shared_ptr<const ModelData> ModelCache::get(const QString &path, const float unitScale) {
  const QString key = QStringLiteral("%1@%2").arg(path).arg(unitScale);
  {
    QMutexLocker lock(&m_mutex);
    if (auto it = m_cache.constFind(key); it != m_cache.constEnd())
      return *it;
  }
  auto data = std::make_shared<ModelData>();
  data->path = path;
  Assimp::Importer importer;
  importer.SetPropertyInteger(AI_CONFIG_PP_SBP_REMOVE, aiPrimitiveType_POINT | aiPrimitiveType_LINE);
  const aiScene *scene = importer.ReadFile(
      path.toStdString(), aiProcess_Triangulate | aiProcess_GenSmoothNormals | aiProcess_CalcTangentSpace
                              | aiProcess_JoinIdenticalVertices | aiProcess_FlipUVs | aiProcess_SortByPType
                              | aiProcess_ImproveCacheLocality | aiProcess_OptimizeMeshes | aiProcess_ValidateDataStructure);
  if (!scene || !scene->mRootNode || (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE)) {
    data->error = QString::fromUtf8(importer.GetErrorString());
    qCWarning(modelLog).noquote() << QStringLiteral("[model] %1: %2").arg(path, data->error);
  } else {
    data->boundsMin = glm::vec3(1e30f);
    data->boundsMax = glm::vec3(-1e30f);
    g_scene = scene;
    g_modelPath = path;
    walk(scene, scene->mRootNode, aiMatrix4x4(), QFileInfo(path).absoluteDir(), unitScale, *data);
    g_scene = nullptr;
    if (data->boundsMin.x > data->boundsMax.x)
      data->boundsMin = data->boundsMax = glm::vec3(0.0f);
    if (data->subMeshes.empty())
      data->error = QStringLiteral("no triangle meshes");
    qCInfo(modelLog).noquote() << QStringLiteral("[model] %1: %2 sub-meshes, %3 triangles")
                                      .arg(path).arg(data->subMeshes.size()).arg(data->triangleCount());
  }
  QMutexLocker lock(&m_mutex);
  m_cache.insert(key, data);
  return data;
}

std::shared_ptr<const ModelData> ModelCache::request(const QString &path, const float unitScale, QObject *context,
                                                     std::function<void()> onReady) {
  const QString key = QStringLiteral("%1@%2").arg(path).arg(unitScale);
  {
    QMutexLocker lock(&m_mutex);
    if (auto it = m_cache.constFind(key); it != m_cache.constEnd())
      return *it;
    if (m_loading.contains(key))
      return nullptr;
    m_loading.insert(key);
  }
  QPointer<QObject> guard(context);
  QThreadPool::globalInstance()->start([this, path, unitScale, key, guard, onReady] {
    get(path, unitScale); // fills the cache
    {
      QMutexLocker lock(&m_mutex);
      m_loading.remove(key);
    }
    if (guard)
      QMetaObject::invokeMethod(guard.data(), [onReady] { onReady(); }, Qt::QueuedConnection);
  });
  return nullptr;
}

std::shared_ptr<Mesh> meshFromModel(const ModelData &model) {
  std::vector<Vertex> verts;
  std::vector<uint32_t> idx;
  std::vector<uint32_t> first, count;
  for (const auto &sub: model.subMeshes) {
    const uint32_t base = uint32_t(verts.size());
    verts.insert(verts.end(), sub.vertices.begin(), sub.vertices.end());
    first.push_back(uint32_t(idx.size()));
    for (const uint32_t i: sub.indices)
      idx.push_back(base + i);
    count.push_back(uint32_t(sub.indices.size()));
  }
  auto mesh = std::make_shared<Mesh>();
  mesh->addVertices(verts);
  mesh->addIndices(idx);
  mesh->setFaceRanges(std::move(first), std::move(count));
  auto &tm = vk::TextureManager::instance();
  for (size_t f = 0; f < model.subMeshes.size(); ++f) {
    const auto &sub = model.subMeshes[f];
    mesh->setFaceUsesVertexUV(int(f), true);
    mesh->setFaceMaterial(int(f), sub.albedoPath.isEmpty() ? tm.materialFor(QStringLiteral("devtexture01"))
                                                           : tm.materialForFile(sub.albedoPath, sub.normalPath));
  }
  return mesh;
}

} // namespace eng
