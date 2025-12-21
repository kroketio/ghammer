#pragma once

// Minimal binary glTF 2.0 writer. Not assimp: the Godot export needs exact node names (-col / -colonly
// suffixes), material names (matched to .tres files), relative image URIs (never embedded) and deterministic
// output (unchanged levels must not reimport). Nodes with TRS, indexed triangles (POSITION, NORMAL, TEXCOORD_0),
// metallic-roughness materials, textures by URI.

#include <QByteArray>
#include <QJsonObject>
#include <QString>

#include <vector>

#include <glm/glm.hpp>

namespace eng::gltf {

struct Image {
  QString uri;  // relative to the .glb ("../../textures/pack/X.png")
  QString name;
};

struct Texture {
  int image = -1;
};

struct Material {
  QString name;
  glm::vec4 baseColor {1.0f};  // linear
  int baseColorTexture = -1;
  int normalTexture = -1;
  float normalScale = 1.0f;
  // Always written: Godot's importer treats a missing factor as 1.0.
  float metallic = 0.0f;
  float roughness = 1.0f;
  bool doubleSided = false;
};

struct Primitive {
  std::vector<glm::vec3> positions;
  std::vector<glm::vec3> normals;
  std::vector<glm::vec2> uvs;  // origin top-left, as glTF
  std::vector<quint32> indices;  // counter-clockwise front faces
  int material = -1;
};

struct Mesh {
  QString name;
  std::vector<Primitive> primitives;
};

struct Node {
  QString name;
  int mesh = -1;
  std::vector<int> children;
  glm::vec3 translation {0.0f};
  QJsonObject extras;
};

struct Document {
  QString generator = QStringLiteral("level editor");
  std::vector<int> roots;  // scene root nodes
  std::vector<Node> nodes;
  std::vector<Mesh> meshes;
  std::vector<Material> materials;
  std::vector<Texture> textures;
  std::vector<Image> images;
};

QByteArray toGlb(const Document &doc);

// Splits a .glb into its JSON and binary chunks (tests, tools).
bool readGlb(const QByteArray &glb, QJsonObject *json, QByteArray *bin);

} // namespace eng::gltf
