#pragma once

#include <array>
#include <map>
#include <memory>

#include <QColor>
#include <QSize>
#include <QString>
#include <QVector>
#include <glm/glm.hpp>

class QRhi;
class QRhiBuffer;
class QRhiTexture;
class QRhiShaderResourceBindings;
class QRhiGraphicsPipeline;
class QRhiCommandBuffer;
class QRhiRenderPassDescriptor;
class QRhiResourceUpdateBatch;

namespace vk {

// Dimension label decals: QPainter-rasterised, cached text sprites drawn as quads in each face's u/v basis, so the
// text lies on the surface.
class TextRenderer {
public:
  // The vertex shader is a parameter: one sprite cache serves the 3D face decals and the 2D grid labels, which
  // project through a different UBO. The fragment shader always reads the sprite from binding 4.
  explicit TextRenderer(const QString &vertexShader = QStringLiteral(":/shaders/text.vert.qsb"));
  ~TextRenderer();

  void invalidate();

  // Size of the labels queued from now on; each decal keeps the size it was queued at.
  void setPixelSize(int pixels);

  [[nodiscard]] QSize size(const QString &text);

  void drawDecal(const QString &text,
                 const glm::vec3 &origin,
                 const glm::vec3 &uAxis,
                 const glm::vec3 &vAxis,
                 float worldWidth,
                 float worldHeight,
                 const QColor &color,
                 float alpha = 1.0f);

  // Rasterises new labels into sprite textures and fills the vertex buffer through `rub`. Call before the pass.
  void prepare(QRhi *rhi, QRhiResourceUpdateBatch *rub, QRhiBuffer *sceneUbo,
               QRhiRenderPassDescriptor *renderPass, int sampleCount);

  // Issues the draw calls. Call inside the pass.
  void draw(QRhiCommandBuffer *cb);

private:
  struct Vertex {
    glm::vec3 position;
    glm::vec2 uv;
    glm::vec4 color;
  };

  struct Decal {
    QString text;
    int pixelSize = 0;
    std::array<Vertex, 6> vertices;
  };

  struct Sprite {
    QRhiTexture *texture = nullptr;
    QRhiShaderResourceBindings *srb = nullptr;
    int width = 0;
    int height = 0;
    quint64 lastUsed = 0;
  };

  [[nodiscard]] static QString cacheKey(int pixelSize, const QString &text);
  [[nodiscard]] static QSize measure(int pixelSize, const QString &text);
  Sprite *spriteFor(QRhi *rhi, QRhiResourceUpdateBatch *rub, const QString &text, int pixelSize,
                    QRhiBuffer *sceneUbo);
  void clearCache();
  void ensurePipeline(QRhi *rhi, QRhiRenderPassDescriptor *renderPass, int sampleCount,
                      QRhiBuffer *sceneUbo);

  std::map<QString, Sprite> m_cache;
  QString m_vertexShader;
  static constexpr size_t MAX_CACHED_LABELS = 128;
  quint64 m_frameCounter = 0;
  int m_pixelSize = 32;

  QRhi *m_rhi = nullptr;
  QRhiRenderPassDescriptor *m_builtRenderPass = nullptr;
  int m_builtSampleCount = 0;
  std::unique_ptr<QRhiGraphicsPipeline> m_pipeline;
  QRhiBuffer *m_vbo = nullptr;
  quint32 m_vboCapacity = 0;
  QRhiShaderResourceBindings *m_templateSrb = nullptr;

  QVector<Decal> m_frame;

  struct Range {
    // Named (by cache key), not pointed at: a sprite can be evicted between
    // prepare() and draw(), which would dangle a raw pointer.
    QString key;
    quint32 first;
    quint32 count;
  };
  QVector<Range> m_ranges;
};

} // namespace vk
