#pragma once

#include <QMutex>
#include <QObject>
#include <QSet>

#include <QHash>
#include <QImage>
#include <rhi/qshader.h>
#include <QSharedPointer>
#include <QString>

#include <functional>

#include "engine/material.h"
#include "engine/material_overrides.h"

class QRhi;
class QRhiBuffer;
class QRhiTexture;
class QRhiSampler;
class QRhiRenderTarget;
class QRhiRenderBuffer;
class QRhiRenderPassDescriptor;
class QRhiResourceUpdateBatch;
class QRhiShaderResourceBindings;
class QLoggingCategory;

namespace vk {

// VK_PERF=1 turns on the once-a-second frame report from every renderer.
bool envFlag(const char *name);
bool perfOn();

// Stage duration above which a renderer reports a stall at once instead of in the per-second summary. 50 ms by
// default; VK_PERF_SLOW_MS lowers it.
double perfSlowMs();

// Frame telemetry category, kept off the error channel. Also reachable via
// QT_LOGGING_RULES="vk.perf.info=true".
QLoggingCategory &perfLog();

// GPU state per QRhi (Qt gives each top-level window its own). Renderers call
// makeCurrent() each frame; instance() is the current context. A context is
// released when its QRhi is destroyed, after its cleanup hooks.
class Shared {
public:
  static Shared &instance();
  // True when the context was created just now.
  static bool makeCurrent(QRhi *rhi);
  static Shared *forRhi(QRhi *rhi);
  // `fn` runs once before `rhi` is destroyed, its context current. One hook per key.
  static void onRhiCleanup(QRhi *rhi, const void *key, std::function<void()> fn);
  static void removeRhiCleanup(const void *key);

  class Scope {
  public:
    explicit Scope(QRhi *rhi);
    ~Scope();
    Scope(const Scope &) = delete;
    Scope &operator=(const Scope &) = delete;

  private:
    Shared *m_prev;
  };

  [[nodiscard]] QRhi *rhi() const { return m_rhi; }
  [[nodiscard]] bool isValid() const { return m_rhi != nullptr; }

  QShader shader(const QString &resourcePath);

  // Lazily created resources upload through the current render's batch (an offscreen frame is illegal while a frame
  // is active). Renderers set this once per frame before touching any fallback.
  void setUploader(QRhiResourceUpdateBatch *rub) { m_uploader = rub; }

  QRhiSampler *linearSampler();
  QRhiSampler *nearestSampler();
  // Linear, clamped on all three axes (LUT cubes, screen-space post passes).
  QRhiSampler *clampLinearSampler();
  // Equirectangular skies: linear, no mips, U wraps round, V clamped at the poles.
  QRhiSampler *panoramaSampler();

  // 1x1 stand-ins so a material with no map of some kind still binds something
  // valid and the shader needs no branch for it.
  QRhiTexture *fallbackWhite();
  QRhiTexture *fallbackBlack();
  QRhiTexture *fallbackGray();
  QRhiTexture *fallbackNormal();
  // Purple/black checkerboard: a texture that is missing, or a server texture
  // while the asset server cannot be reached.
  QRhiTexture *fallbackChecker();

  // Replaced resources: an earlier frame may still read them, so they are freed once no frame can (beginFrame()).
  // Not at teardown: a resize storm retires viewport-sized textures every frame and would exhaust VRAM.
  void retire(QRhiBuffer *buffer);
  void retireTexture(QRhiTexture *texture);
  void retireRenderTarget(QRhiRenderTarget *target);
  void retireRenderPass(QRhiRenderPassDescriptor *renderPass);
  void retireRenderBuffer(QRhiRenderBuffer *renderBuffer);
  void retireShaderResourceBindings(QRhiShaderResourceBindings *bindings);

  // Call once per frame, after acquire() and before retiring anything. Releases what was retired one cycle ago: each
  // entry is tagged with its RHI frame slot, and reaching that slot again means the swapchain waited on its fence.
  void beginFrame();

private:
  Shared() = default;
  ~Shared();
  void release();
  static void destroyContext(QRhi *rhi);

  QRhiTexture *solidTexture(const QColor &color);

  // One retired resource per entry; only the member that matches its type is set.
  struct Retired {
    int slot = -1;
    QRhiBuffer *buffer = nullptr;
    QRhiTexture *texture = nullptr;
    QRhiRenderTarget *target = nullptr;
    QRhiRenderPassDescriptor *renderPass = nullptr;
    QRhiRenderBuffer *renderBuffer = nullptr;
    QRhiShaderResourceBindings *bindings = nullptr;

    void release();
  };

  [[nodiscard]] int currentSlot() const;
  void pushRetired(Retired entry);
  void releaseRetired(int slot, bool all);
  void releaseAllRetired();

  QRhi *m_rhi = nullptr;
  QRhiResourceUpdateBatch *m_uploader = nullptr;
  QHash<QString, QShader> m_shaders;
  QRhiSampler *m_linear = nullptr;
  QRhiSampler *m_nearest = nullptr;
  QRhiSampler *m_clampLinear = nullptr;
  QRhiSampler *m_panorama = nullptr;
  QRhiTexture *m_white = nullptr;
  QRhiTexture *m_black = nullptr;
  QRhiTexture *m_gray = nullptr;
  QRhiTexture *m_normal = nullptr;
  QRhiTexture *m_checker = nullptr;
  QVector<Retired> m_retired;
  int m_activeSlot = -1;
  int m_framesInFlight = 1;
};

// Material notifications for the UI (rename of a texture that faces use, ...).
class MaterialEvents final : public QObject {
  Q_OBJECT

public:
  static MaterialEvents &instance();
  void notifyRenamed(const QString &oldRef, const QString &newRef);

signals:
  void renamed(const QString &oldRef, const QString &newRef);

private:
  MaterialEvents() = default;
};

// Name -> Material, GPU side filled in lazily: materials are created during map loads, before any QRhi exists.
// Kinds (eng::Material::Kind):
//   Server   "pack/Name": entry from net::TextureCatalog, 512 px editor maps from net::FileFetcher; server changes
//            flow back in through the catalogue's signals.
//   Library  the local texture library (plus the built-in dev texture for unknown names).
//   File     model textures, straight from image files.
// Effective props are resolved here from the property stack (the map's eng::MaterialOverrides + the catalogue
// entry) and refreshed whenever a layer changes.
class TextureManager {
public:
  TextureManager();
  ~TextureManager();

  static TextureManager &instance();

  // Safe to call with no QRhi: returns the CPU-side material only.
  eng::MaterialPtr materialFor(const QString &name);
  // A material made straight from image files (model textures).
  eng::MaterialPtr materialForFile(const QString &albedoPath, const QString &normalPath = {});
  // An existing material, or null.
  [[nodiscard]] eng::MaterialPtr find(const QString &name) const;

  // Makes sure the material has something bound: the first call binds the shared fallbacks and starts loading the
  // real maps off the render thread; pumpUploads() swaps them in a later frame.
  void ensureGpu(eng::Material *material, QRhiResourceUpdateBatch *rub);

  // Uploads up to `budget` decoded materials. Call once per frame before
  // binding materials. Returns true when anything was uploaded.
  bool pumpUploads(QRhiResourceUpdateBatch *rub, int budget = 24);

  // Materials whose maps are downloading / decoding (continuous redraw while
  // > 0, status bar); with `rhi`, only those loading for it.
  [[nodiscard]] int pendingCount() const;
  [[nodiscard]] int pendingCount(QRhi *rhi) const;

  // Drops every material texture and SRB made on `rhi` (its context is going).
  void releaseRhi(QRhi *rhi);
  // Drops the SRBs an engine built (its buffers are going).
  void releaseSrbsOf(const void *owner);

  // Materials outside the name cache, for scenes of their own (previews): never
  // shared with the level, released with their last reference.
  eng::MaterialPtr privateMaterial(const QString &ref);
  eng::MaterialPtr privateMaterial(const TextureEntry &entry, const MaterialProps &props);
  void setPrivateProps(eng::Material &m, const MaterialProps &props);

  // Re-checks every library material against the texture library on disk (after a rescan).
  void reresolveLibrary();

  static QImage loadImage(const QString &path);

  // Only affects materials decoded afterwards.
  void setMaxTextureSize(int px) { m_maxTextureSize = px; }

  // Every material that exists, for the material browser's "used" filter.
  [[nodiscard]] QList<eng::MaterialPtr> allMaterials() const;

  // The overrides of the map being edited (owned by its Scene).
  void setOverrides(eng::MaterialOverrides *overrides);
  [[nodiscard]] eng::MaterialOverrides *overrides() const { return m_overrides; }
  // The whole property stack of one material (for the properties panel).
  [[nodiscard]] eng::ResolvedProps resolvedFor(const eng::Material &material) const;
  // Re-resolves the effective properties of every material (or the ones of one
  // base ref and its variants) after a layer changed.
  void refreshProps();
  void refreshProps(const QString &ref);
  // Drops the GPU maps of a material (and its variants) so they reload.
  void reload(const QString &ref);

private:
  QRhiTexture *uploadImage(const QImage &image, bool srgb, QRhiResourceUpdateBatch *rub);
  static bool resolveFromLibrary(eng::Material &m);
  void connectSignals();
  void startServerLoad(const eng::MaterialPtr &material);
  void onResolved(const eng::MaterialPtr &material);
  void applyEntry(const eng::MaterialPtr &material, const TextureEntry &entry);
  void refreshMaterialProps(eng::Material &m);
  void dropGpu(eng::Material &m);
  void setError(eng::Material &m);
  void useDevFallback(eng::Material &m);
  eng::MaterialPtr newPrivate(const QString &ref);
  void forgetPrivate(eng::Material *m);
  void releaseSrbs(eng::Material &m, QRhi *rhi);
  [[nodiscard]] bool isKnownServerRef(const QString &ref) const;
  [[nodiscard]] QList<eng::MaterialPtr> materialsOfBase(const QString &ref) const;

  struct Decoded {
    eng::Material *material = nullptr;
    quint64 ticket = 0;
    QImage diffuse, normal, orm, emission;
    bool error = false;
  };

  QHash<QString, eng::MaterialPtr> m_byName;
  QHash<eng::Material *, QWeakPointer<eng::Material>> m_private;
  quint64 m_ticket = 0;
  eng::MaterialOverrides *m_overrides = nullptr;
  QObject *m_signals = nullptr;  // receiver for catalogue / client signals

  mutable QMutex m_mutex;
  QVector<Decoded> m_ready;          // guarded by m_mutex
  QSet<eng::Material *> m_pending;   // GUI thread only
  int m_maxTextureSize = 2048;
};

} // namespace vk
