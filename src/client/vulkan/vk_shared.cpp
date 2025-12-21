#include "vk_shared.h"

#include <algorithm>

#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QSet>
#include <QThreadPool>
#include <QMutexLocker>
#include <QLoggingCategory>

#include <rhi/qrhi.h>

#include "shared/lib/globals.h"
#include "client/engine/texture_library.h"
#include "client/engine/orm_compose.h"
#include "client/net/file_fetcher.h"
#include "client/net/texture_catalog.h"
#include "client/prefs/preferences.h"

namespace vk {

static QLoggingCategory vkLog("vk.shared", QtWarningMsg);

bool envFlag(const char *name) {
  const QByteArray v = qgetenv(name).toLower();
  return v == "1" || v == "on" || v == "true" || v == "yes";
}

QLoggingCategory &perfLog() {
  // Silent by default; perfOn() reads this to decide whether to meter.
  static QLoggingCategory category("vk.perf", QtWarningMsg);
  return category;
}

bool perfOn() {
  // VK_PERF must also open the category (silent by default). Resolved once.
  static const bool v = [] {
    const bool env = envFlag("VK_PERF");
    if (env)
      perfLog().setEnabled(QtInfoMsg, true);
    return env || perfLog().isInfoEnabled();
  }();
  return v;
}

double perfSlowMs() {
  static const double v = [] {
    const QByteArray raw = qgetenv("VK_PERF_SLOW_MS");
    return raw.isEmpty() ? 50.0 : raw.toDouble();
  }();
  return v;
}

namespace {

struct CleanupHook {
  QRhi *rhi = nullptr;
  const void *key = nullptr;
  std::function<void()> fn;
};

// Leaked on purpose: no teardown at static destruction.
QHash<QRhi *, Shared *> &contexts() {
  static auto *c = new QHash<QRhi *, Shared *>();
  return *c;
}

QVector<CleanupHook> &cleanupHooks() {
  static auto *h = new QVector<CleanupHook>();
  return *h;
}

Shared *s_current = nullptr;

} // namespace

Shared &Shared::instance() {
  static Shared *none = new Shared();
  return s_current ? *s_current : *none;
}

Shared::~Shared() {
  release();
}

Shared *Shared::forRhi(QRhi *rhi) {
  return rhi ? contexts().value(rhi) : nullptr;
}

bool Shared::makeCurrent(QRhi *rhi) {
  if (!rhi)
    return false;
  if (s_current && s_current->m_rhi == rhi)
    return false;
  if (Shared *ctx = forRhi(rhi)) {
    s_current = ctx;
    return false;
  }
  auto *ctx = new Shared();
  ctx->m_rhi = rhi;
  ctx->m_framesInFlight = qMax(1, rhi->resourceLimit(QRhi::FramesInFlight));
  ctx->m_activeSlot = -1;
  contexts().insert(rhi, ctx);
  rhi->addCleanupCallback(ctx, [](QRhi *r) { destroyContext(r); });
  s_current = ctx;
  qInfo(vkLog, "new context for QRhi %p (backend=%d)", (void *) rhi, int(rhi->backend()));
  return true;
}

void Shared::onRhiCleanup(QRhi *rhi, const void *key, std::function<void()> fn) {
  removeRhiCleanup(key);
  if (rhi && key)
    cleanupHooks().push_back({rhi, key, std::move(fn)});
}

void Shared::removeRhiCleanup(const void *key) {
  auto &hooks = cleanupHooks();
  hooks.erase(std::remove_if(hooks.begin(), hooks.end(), [key](const CleanupHook &h) { return h.key == key; }),
              hooks.end());
}

void Shared::destroyContext(QRhi *rhi) {
  Shared *ctx = forRhi(rhi);
  if (!ctx)
    return;
  Shared *prev = s_current;
  s_current = ctx;
  // Hooks first: they retire into this context.
  for (;;) {
    auto &hooks = cleanupHooks();
    auto it = std::find_if(hooks.begin(), hooks.end(), [rhi](const CleanupHook &h) { return h.rhi == rhi; });
    if (it == hooks.end())
      break;
    const std::function<void()> fn = std::move(it->fn);
    hooks.erase(it);
    if (fn)
      fn();
  }
  TextureManager::instance().releaseRhi(rhi);
  ctx->release();
  contexts().remove(rhi);
  delete ctx;
  s_current = prev == ctx ? nullptr : prev;
  qInfo(vkLog, "released the context of QRhi %p", (void *) rhi);
}

Shared::Scope::Scope(QRhi *rhi) : m_prev(s_current) {
  if (Shared *ctx = forRhi(rhi))
    s_current = ctx;
}

Shared::Scope::~Scope() {
  s_current = m_prev && contexts().values().contains(m_prev) ? m_prev : nullptr;
}

void Shared::release() {
  if (!m_rhi)
    return;

  delete m_linear;
  delete m_nearest;
  delete m_clampLinear;
  m_clampLinear = nullptr;
  delete m_panorama;
  m_panorama = nullptr;
  delete m_white;
  delete m_black;
  delete m_gray;
  delete m_normal;
  delete m_checker;
  releaseAllRetired();

  m_linear = m_nearest = nullptr;
  m_white = m_black = m_gray = m_normal = m_checker = nullptr;
  m_shaders.clear();
  m_uploader = nullptr;
  m_rhi = nullptr;
  m_activeSlot = -1;
  m_framesInFlight = 1;
}

QShader loadBakedShader(const QString &resourcePath) {
  QFile f(resourcePath);
  if (!f.open(QIODevice::ReadOnly)) {
    qWarning(vkLog, "cannot open baked shader %s", qPrintable(resourcePath));
    return {};
  }
  auto shader = QShader::fromSerialized(f.readAll());
  if (!shader.isValid())
    qWarning(vkLog, "baked shader %s is not valid for this backend", qPrintable(resourcePath));
  return shader;
}

QShader Shared::shader(const QString &resourcePath) {
  const auto it = m_shaders.constFind(resourcePath);
  if (it != m_shaders.constEnd())
    return *it;
  const QShader shader = loadBakedShader(resourcePath);
  m_shaders.insert(resourcePath, shader);
  return shader;
}

QRhiSampler *Shared::linearSampler() {
  if (!m_linear) {
    m_linear = m_rhi->newSampler(QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::Linear,
                                QRhiSampler::Repeat, QRhiSampler::Repeat);
    m_linear->create();
  }
  return m_linear;
}

QRhiSampler *Shared::clampLinearSampler() {
  if (!m_clampLinear) {
    m_clampLinear = m_rhi->newSampler(QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None,
                                     QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge,
                                     QRhiSampler::ClampToEdge);
    m_clampLinear->create();
  }
  return m_clampLinear;
}

QRhiSampler *Shared::panoramaSampler() {
  if (!m_panorama) {
    m_panorama = m_rhi->newSampler(QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None,
                                  QRhiSampler::Repeat, QRhiSampler::ClampToEdge);
    m_panorama->create();
  }
  return m_panorama;
}

QRhiSampler *Shared::nearestSampler() {
  if (!m_nearest) {
    m_nearest = m_rhi->newSampler(QRhiSampler::Nearest, QRhiSampler::Nearest, QRhiSampler::None,
                                 QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge);
    m_nearest->create();
  }
  return m_nearest;
}

QRhiTexture *Shared::solidTexture(const QColor &color) {
  QImage img(1, 1, QImage::Format_RGBA8888);
  img.fill(color);

  auto *tex = m_rhi->newTexture(QRhiTexture::RGBA8, QSize(1, 1));
  if (!tex->create()) {
    delete tex;
    return nullptr;
  }

  if (!m_uploader) {
    qWarning(vkLog, "no upload batch set; fallback texture stays empty");
    delete tex;
    return nullptr;
  }
  m_uploader->uploadTexture(tex, img);
  return tex;
}

QRhiTexture *Shared::fallbackWhite() {
  if (!m_white)
    m_white = solidTexture(QColor(255, 255, 255));
  return m_white;
}

QRhiTexture *Shared::fallbackBlack() {
  if (!m_black)
    m_black = solidTexture(QColor(0, 0, 0));
  return m_black;
}

QRhiTexture *Shared::fallbackGray() {
  if (!m_gray)
    m_gray = solidTexture(QColor(128, 128, 128));
  return m_gray;
}

QRhiTexture *Shared::fallbackNormal() {
  if (!m_normal)
    m_normal = solidTexture(QColor(128, 128, 255, 255));
  return m_normal;
}

QRhiTexture *Shared::fallbackChecker() {
  if (m_checker)
    return m_checker;
  if (!m_rhi || !m_uploader)
    return nullptr;
  const QImage img = eng::errorChecker();
  auto *tex = m_rhi->newTexture(QRhiTexture::RGBA8, img.size(), 1, QRhiTexture::sRGB);
  if (!tex->create()) {
    delete tex;
    return nullptr;
  }
  m_uploader->uploadTexture(tex, img);
  m_checker = tex;
  return m_checker;
}

void Shared::retire(QRhiBuffer *buffer) {
  Retired entry;
  entry.buffer = buffer;
  pushRetired(entry);
}

void Shared::retireTexture(QRhiTexture *texture) {
  Retired entry;
  entry.texture = texture;
  pushRetired(entry);
}

void Shared::retireRenderTarget(QRhiRenderTarget *target) {
  Retired entry;
  entry.target = target;
  pushRetired(entry);
}

void Shared::retireRenderPass(QRhiRenderPassDescriptor *renderPass) {
  Retired entry;
  entry.renderPass = renderPass;
  pushRetired(entry);
}

void Shared::retireRenderBuffer(QRhiRenderBuffer *renderBuffer) {
  Retired entry;
  entry.renderBuffer = renderBuffer;
  pushRetired(entry);
}

void Shared::retireShaderResourceBindings(QRhiShaderResourceBindings *bindings) {
  Retired entry;
  entry.bindings = bindings;
  pushRetired(entry);
}

void Shared::Retired::release() {
  delete buffer;
  delete texture;
  delete target;
  delete renderPass;
  delete renderBuffer;
  delete bindings;
}

int Shared::currentSlot() const {
  return m_rhi ? m_rhi->currentFrameSlot() : -1;
}

void Shared::pushRetired(Retired entry) {
  QRhiResource *res = entry.buffer ? static_cast<QRhiResource *>(entry.buffer)
                      : entry.texture ? static_cast<QRhiResource *>(entry.texture)
                      : entry.target ? static_cast<QRhiResource *>(entry.target)
                      : entry.renderPass ? static_cast<QRhiResource *>(entry.renderPass)
                      : entry.renderBuffer ? static_cast<QRhiResource *>(entry.renderBuffer)
                                           : static_cast<QRhiResource *>(entry.bindings);
  if (!res)
    return;
  // Freed through the context of the QRhi that made it.
  Shared *owner = forRhi(res->rhi());
  if (!owner)
    owner = this;
  entry.slot = owner->currentSlot();
  owner->m_retired.push_back(entry);
}

void Shared::beginFrame() {
  if (!m_rhi || m_retired.isEmpty())
    return;

  const int slot = currentSlot();
  if (slot < 0)
    return;

  // One frame in flight: beginFrame's wait covers every earlier frame, so all pending is free. More: the slot must
  // come round first (all items of one frame share a slot, so a slot change marks a new frame).
  const bool all = m_framesInFlight <= 1;
  if (!all && slot == m_activeSlot)
    return;

  m_activeSlot = slot;
  releaseRetired(slot, all);
}

void Shared::releaseRetired(const int slot, const bool all) {
  QVector<Retired> keep;
  keep.reserve(m_retired.size());
  for (auto &entry: m_retired) {
    // Retired outside a frame: nothing has ever drawn with it, so the next
    // cycle is more than enough.
    if (all || entry.slot == slot || entry.slot < 0)
      entry.release();
    else
      keep.push_back(entry);
  }
  m_retired.swap(keep);
}

void Shared::releaseAllRetired() {
  for (auto &entry: m_retired)
    entry.release();
  m_retired.clear();
}

namespace {

QImage clampSize(QImage img, int maxEdge) {
  if (img.isNull() || maxEdge <= 0)
    return img;
  if (img.width() > maxEdge || img.height() > maxEdge)
    img = img.scaled(QSize(maxEdge, maxEdge), Qt::KeepAspectRatio, Qt::SmoothTransformation);
  return img;
}

QImage loadLinear(const QString &path) {
  if (path.isEmpty())
    return {};
  return TextureManager::loadImage(path);
}

void flipGreen(QImage &img) {
  for (int y = 0; y < img.height(); ++y) {
    uchar *row = img.scanLine(y);
    for (int x = 0; x < img.width(); ++x)
      row[x * 4 + 1] = uchar(255 - row[x * 4 + 1]);
  }
}

const QString kDevFallback = QStringLiteral(":/assets/measurewall01_Color.jpg");

QString baseKeyOf(const QString &ref) { return refKey(MaterialRef::parse(ref).base()); }

// The editor's own dev textures: always the built-in measurewall, never looked
// up on the asset server (so new brushes look right offline too).
bool isBuiltInName(const QString &key) {
  return eng::isDevMaterial(key, prefs::s("Brush/DefaultMaterial"));
}

// The texture channels the ORM image is composed with.
struct Channels {
  matprops::Channel ao, rough, metal;
};
Channels channelsOf(const MaterialProps &p) { return {p.aoChannel, p.roughnessChannel, p.metallicChannel}; }

} // namespace

TextureManager::TextureManager() = default;
TextureManager::~TextureManager() = default;

TextureManager &TextureManager::instance() {
  static TextureManager mgr;
  mgr.connectSignals();
  return mgr;
}

QImage TextureManager::loadImage(const QString &path) {
  // Missing map slots come in as empty paths: skip them, or QImage warns "QFSFileEngine::open: No file name specified".
  if (path.isEmpty())
    return {};
  QImage img(path);
  if (img.isNull())
    return {};
  if (img.format() != QImage::Format_RGBA8888)
    img = img.convertToFormat(QImage::Format_RGBA8888);
  return img;
}

QList<eng::MaterialPtr> TextureManager::allMaterials() const {
  // m_byName also holds alias keys (legacy name -> resolved ref): de-duplicate.
  QList<eng::MaterialPtr> out;
  QSet<eng::Material *> seen;
  for (const auto &m: m_byName)
    if (!seen.contains(m.data())) {
      seen.insert(m.data());
      out.append(m);
    }
  return out;
}

eng::MaterialPtr TextureManager::find(const QString &name) const { return m_byName.value(name.trimmed().toLower()); }

void TextureManager::connectSignals() {
  if (m_signals)
    return;
  m_signals = new QObject();
  auto &catalog = net::TextureCatalog::instance();
  QObject::connect(&catalog, &net::TextureCatalog::entryChanged, m_signals, [this](const QString &ref) {
    const TextureEntry *e = net::TextureCatalog::instance().find(ref);
    if (!e)
      return;
    for (const auto &m: materialsOfBase(ref))
      applyEntry(m, *e);
  });
  QObject::connect(&catalog, &net::TextureCatalog::entryRemoved, m_signals, [this](const QString &ref) {
    for (const auto &m: materialsOfBase(ref)) {
      m->entry = TextureEntry();
      dropGpu(*m);
      setError(*m);
    }
  });
  QObject::connect(&catalog, &net::TextureCatalog::entryRenamed, m_signals,
                   [this](const QString &oldRef, const QString &newRef) {
                     // Faces hold the material, so renaming the material renames every
                     // reference to it in the open map.
                     const MaterialRef nr = MaterialRef::parse(newRef);
                     for (const auto &m: materialsOfBase(oldRef)) {
                       const MaterialRef r = MaterialRef::parse(m->ref);
                       const QString renamed = MaterialRef::make(nr.pack, nr.name, r.variant);
                       m_byName.insert(renamed.toLower(), m);
                       m->ref = renamed;
                       m->name = renamed.toLower();
                       if (m->gpuSource.isNull())
                         m->touch();
                       else
                         eng::Material::propsChanged();
                     }
                     if (m_overrides)
                       m_overrides->renameTexture(oldRef, newRef);
                     MaterialEvents::instance().notifyRenamed(oldRef, newRef);
                   });
  QObject::connect(&catalog, &net::TextureCatalog::serverChanged, m_signals, [this] {
    for (const auto &m: allMaterials()) {
      const bool devFallback = m->kind == eng::Material::Kind::Library && m->libAlbedo == kDevFallback
                               && !isBuiltInName(m->name);
      if (m->kind != eng::Material::Kind::Server && !devFallback)
        continue;
      if (devFallback) {
        m->kind = eng::Material::Kind::Server;
        m->libAlbedo.clear();
      }
      m->entry = TextureEntry();
      m->state = eng::Material::State::Pending;
      dropGpu(*m);
      refreshMaterialProps(*m);
      m->touch();
      if (m->gpuSource.isNull())
        net::TextureCatalog::instance().resolve({m->ref}, m_signals, [this, m] { onResolved(m); });
    }
  });
  QObject::connect(&net::AssetClient::instance(), &net::AssetClient::stateChanged, m_signals,
                   [this](net::AssetClient::State st) {
                     if (st != net::AssetClient::State::Online)
                       return;
                     // Back online: everything that failed gets another go.
                     for (const auto &m: allMaterials()) {
                       if (m->kind != eng::Material::Kind::Server || m->state != eng::Material::State::Error)
                         continue;
                       m->state = eng::Material::State::Pending;
                       dropGpu(*m);
                       if (!m->hasEntry())
                         net::TextureCatalog::instance().resolve({m->ref}, m_signals,
                                                                 [this, m] { onResolved(m); });
                     }
                   });
}

bool TextureManager::isKnownServerRef(const QString &ref) const {
  if (m_overrides && !m_overrides->idFor(ref).isEmpty())
    return true;
  const MaterialRef r = MaterialRef::parse(ref);
  return !r.isLegacy() && net::TextureCatalog::instance().pack(r.pack) != nullptr;
}

QList<eng::MaterialPtr> TextureManager::materialsOfBase(const QString &ref) const {
  const QString key = baseKeyOf(ref);
  QList<eng::MaterialPtr> out;
  for (const auto &m: allMaterials())
    if (m->kind == eng::Material::Kind::Server && (baseKeyOf(m->ref) == key ||
                                                   (m->hasEntry() && refKey(m->entry.ref) == key)))
      out.append(m);
  return out;
}

eng::MaterialPtr TextureManager::materialFor(const QString &nameIn) {
  const QString name = nameIn.trimmed();
  const QString key = name.toLower();
  if (key.isEmpty())
    return materialFor(QStringLiteral("devtexture01"));

  if (const auto existing = m_byName.constFind(key); existing != m_byName.constEnd())
    return *existing;

  auto material = QSharedPointer<eng::Material>::create(key);
  material->ref = name;
  m_byName.insert(key, material);

  const MaterialRef r = MaterialRef::parse(name);
  if (r.isVariant()) {
    // A map-local variant draws with its base texture's maps.
    const eng::MaterialPtr base = materialFor(r.base());
    material->kind = eng::Material::Kind::Server;
    material->gpuSource = base;
    if (base->hasEntry())
      applyEntry(material, base->entry);
    else
      refreshMaterialProps(*material);
    return material;
  }

  // 0. built-in dev textures (a library texture of the same name still wins)
  if (isBuiltInName(key) && !resolveFromLibrary(*material)) {
    useDevFallback(*material);
    return material;
  }
  // 1. the local texture library
  if (resolveFromLibrary(*material)) {
    material->kind = eng::Material::Kind::Library;
    refreshMaterialProps(*material);
    return material;
  }
  // 2. the server catalogue (cached, or asked for)
  material->kind = eng::Material::Kind::Server;
  if (const TextureEntry *e = net::TextureCatalog::instance().find(name)) {
    applyEntry(material, *e);
  } else {
    refreshMaterialProps(*material);
    net::TextureCatalog::instance().resolve({name}, m_signals, [this, material] { onResolved(material); });
  }
  return material;
}

void TextureManager::onResolved(const eng::MaterialPtr &material) {
  if (material->hasEntry())
    return;
  const TextureEntry *e = net::TextureCatalog::instance().find(material->ref);
  if (e) {
    applyEntry(material, *e);
    return;
  }
  // 3. nobody has it
  const bool serverSaidMissing = net::TextureCatalog::instance().isKnownMissing(material->ref);
  if (!serverSaidMissing || isKnownServerRef(material->ref)) {
    // Unreachable server, or a server texture that is gone: the error checker.
    setError(*material);
    return;
  }
  // A name the server never had (VMF import, old dev textures).
  useDevFallback(*material);
}

void TextureManager::applyEntry(const eng::MaterialPtr &m, const TextureEntry &entry) {
  const bool mapsChanged = !m->hasEntry() || m->entry.maps != entry.maps;
  m->entry = entry;
  m->kind = eng::Material::Kind::Server;
  if (entry.w > 0 && entry.h > 0) {
    m->texWidth = entry.w;
    m->texHeight = entry.h;
  }
  // A legacy name that the server resolved is saved as the real ref from now on.
  if (MaterialRef::parse(m->ref).isLegacy()) {
    m->ref = entry.ref;
    if (!m_private.contains(m.data()))
      m_byName.insert(entry.ref.toLower(), m);
  }
  if (m_overrides)
    m_overrides->rememberId(entry.ref, entry.id);
  if (m->state == eng::Material::State::Error)
    m->state = eng::Material::State::Pending;
  if (mapsChanged && m->gpuSource.isNull())
    dropGpu(*m);
  refreshMaterialProps(*m);
  m->touch();
}

void TextureManager::setError(eng::Material &m) {
  m.state = eng::Material::State::Error;
  m_pending.remove(&m);
  dropGpu(m);  // next ensureGpu binds the checker
  m.touch();
}

void TextureManager::useDevFallback(eng::Material &m) {
  m.kind = eng::Material::Kind::Library;
  m.state = eng::Material::State::Pending;
  m.libAlbedo = kDevFallback;
  dropGpu(m);
  refreshMaterialProps(m);
  m.touch();
}

void TextureManager::dropGpu(eng::Material &m) {
  auto &shared = Shared::instance();
  if (m.ownsTextures) {
    for (QRhiTexture *t: {m.ownedAlbedo, m.ownedNormal, m.ownedMr, m.ownedEmission})
      if (t)
        shared.retireTexture(t);
  }
  m.ownedAlbedo = m.ownedNormal = m.ownedMr = m.ownedEmission = nullptr;
  m.ownsTextures = false;
  m.albedo = m.normal = m.metallicRoughness = m.emission = nullptr;
  m.realNormal = m.realOrm = m.realEmission = false;
  m.gpuRhi = nullptr;
  m_pending.remove(&m);
}

eng::MaterialPtr TextureManager::materialForFile(const QString &albedoPath, const QString &normalPath) {
  const QString key = QStringLiteral("file:") + albedoPath.toLower();
  if (auto it = m_byName.constFind(key); it != m_byName.constEnd())
    return *it;
  auto material = QSharedPointer<eng::Material>::create(key);
  material->kind = eng::Material::Kind::File;
  material->libAlbedo = albedoPath;
  material->libNormal = normalPath;
  QImageReader reader(albedoPath);
  if (reader.size().isValid()) {
    material->texWidth = reader.size().width();
    material->texHeight = reader.size().height();
  }
  m_byName.insert(key, material);
  refreshMaterialProps(*material);
  return material;
}

QRhiTexture *TextureManager::uploadImage(const QImage &image, const bool srgb,
                                        QRhiResourceUpdateBatch *rub) {
  if (image.isNull())
    return nullptr;

  auto &shared = Shared::instance();
  // MipMapped allocates the chain; UsedWithGenerateMips alone gives generateMips() a single level (shimmering).
  QRhiTexture::Flags flags = QRhiTexture::MipMapped | QRhiTexture::UsedWithGenerateMips;
  if (srgb)
    flags |= QRhiTexture::sRGB;

  auto *tex = shared.rhi()->newTexture(QRhiTexture::RGBA8, image.size(), 1, flags);
  if (!tex->create()) {
    delete tex;
    return nullptr;
  }
  rub->uploadTexture(tex, image);
  rub->generateMips(tex);
  return tex;
}

bool TextureManager::resolveFromLibrary(eng::Material &m) {
  eng::TextureLibrary::Entry e;
  if (!eng::TextureLibrary::instance().find(m.name, e))
    return false;
  if (m.libAlbedo == e.albedo && m.libNormal == e.normal)
    return !m.libAlbedo.isEmpty();
  m.libAlbedo = e.albedo;
  m.libNormal = e.normal;
  m.libRoughness = e.roughness;
  m.libMetallic = e.metallic;
  m.libAo = e.ao;
  m.libArm = e.arm;
  m.libEmissive = e.emissive;
  m.libNormalDirectX = e.normalIsDirectX;
  if (e.size.isValid()) {
    m.texWidth = e.size.width();
    m.texHeight = e.size.height();
  }
  return true;
}

void TextureManager::reresolveLibrary() {
  // Materials created before the library scan finished (a map loads faster
  // than a big folder scans) pick their real maps up here.
  for (const auto &material: allMaterials()) {
    if (material->kind == eng::Material::Kind::Server && material->hasEntry())
      continue;
    if (material->kind == eng::Material::Kind::File || material->gpuSource)
      continue;
    const QString before = material->libAlbedo;
    eng::TextureLibrary::Entry e;
    if (!eng::TextureLibrary::instance().find(material->name, e) || e.albedo == before)
      continue;
    resolveFromLibrary(*material);
    material->kind = eng::Material::Kind::Library;
    material->state = eng::Material::State::Pending;
    dropGpu(*material);
    refreshMaterialProps(*material);
    material->touch();
  }
}

void TextureManager::setOverrides(eng::MaterialOverrides *overrides) {
  m_overrides = overrides;
  // MATL ids of the new map make its server refs "known" (checker if missing).
  refreshProps();
}

eng::ResolvedProps TextureManager::resolvedFor(const eng::Material &m) const {
  static const eng::MaterialOverrides kEmpty;
  const eng::MaterialOverrides &ov = m_overrides ? *m_overrides : kEmpty;
  if (m.kind == eng::Material::Kind::Server) {
    const TextureEntry *e = m.hasEntry() ? &m.entry : nullptr;
    return ov.resolve(m.ref, e);
  }
  const TextureEntry standIn =
      eng::libraryStandIn(QFileInfo(m.libAlbedo.isEmpty() ? m.name : m.libAlbedo).completeBaseName(),
                          !m.libArm.isEmpty(), !m.libRoughness.isEmpty(), !m.libMetallic.isEmpty(),
                          !m.libAo.isEmpty(), !m.libEmissive.isEmpty());
  return ov.resolve(m.ref, &standIn);
}

void TextureManager::refreshMaterialProps(eng::Material &m) {
  const MaterialProps p = resolvedFor(m).props;
  const bool changed = p != m.props;
  m.props = p;
  // The ORM image bakes the texture channels in: a channel change re-composes.
  const Channels want = channelsOf(p);
  const bool channelsDiffer = want.ao != m.loadedAoChannel || want.rough != m.loadedRoughnessChannel ||
                              want.metal != m.loadedMetallicChannel;
  if (m.gpuSource) {
    // A variant shares its base's maps unless its channels differ.
    const Channels base = channelsOf(m.gpuSource->props);
    if (want.ao != base.ao || want.rough != base.rough || want.metal != base.metal) {
      m.gpuSource.reset();
      dropGpu(m);
      m.touch();
    }
  } else if (channelsDiffer && m.realOrm) {
    dropGpu(m);
    m.touch();
  }
  if (changed)
    eng::Material::propsChanged();
}

void TextureManager::refreshProps() {
  for (const auto &m: allMaterials())
    refreshMaterialProps(*m);
}

void TextureManager::refreshProps(const QString &ref) {
  const QString key = baseKeyOf(ref);
  for (const auto &m: allMaterials())
    if (baseKeyOf(m->ref) == key || m->name == ref.toLower())
      refreshMaterialProps(*m);
}

void TextureManager::reload(const QString &ref) {
  for (const auto &m: materialsOfBase(ref)) {
    if (m->gpuSource)
      continue;
    dropGpu(*m);
    m->state = eng::Material::State::Pending;
    m->touch();
  }
}

void TextureManager::startServerLoad(const eng::MaterialPtr &material) {
  // Runs on the GUI thread (FileFetcher lives there).
  const TextureEntry e = material->entry;
  struct Want {
    MapType type;
    QString file;
  };
  QVector<Want> wants;
  auto want = [&](MapType t) {
    if (const MapFile *f = e.map(t); f && !f->editor.isEmpty())
      wants.append({t, f->editor});
  };
  // The same slot rule as the Godot export (slotMapType).
  QSet<MapType> wanted;
  for (const MaterialSlot s: {MaterialSlot::Albedo, MaterialSlot::Normal, MaterialSlot::Ao, MaterialSlot::Roughness,
                              MaterialSlot::Metallic, MaterialSlot::Emission})
    if (const MapType t = slotMapType(e, s); t != MapType::Count && !wanted.contains(t)) {
      wanted.insert(t);
      want(t);
    }
  want(MapType::Opacity);

  struct Batch {
    QHash<MapType, QString> paths;
    int remaining = 0;
  };
  auto batch = QSharedPointer<Batch>::create();
  batch->remaining = int(wants.size());
  const Channels ch = channelsOf(material->props);
  const int maxEdge = m_maxTextureSize;
  eng::Material *raw = material.data();
  const quint64 ticket = material->loadTicket;
  auto finished = [this, batch, raw, ticket, ch, maxEdge, layout = e.layout()] {
    if (!batch->paths.contains(MapType::Color)) {
      QMutexLocker lock(&m_mutex);
      m_ready.push_back(Decoded{raw, ticket, {}, {}, {}, {}, true});
      return;
    }
    const auto paths = batch->paths;
    QThreadPool::globalInstance()->start([this, paths, raw, ticket, ch, maxEdge, layout] {
      Decoded d;
      d.material = raw;
      d.ticket = ticket;
      d.diffuse = clampSize(loadImage(paths.value(MapType::Color)), maxEdge);
      if (paths.contains(MapType::Opacity))
        d.diffuse = eng::mergeOpacity(d.diffuse, loadLinear(paths.value(MapType::Opacity)));
      d.normal = clampSize(loadLinear(paths.value(MapType::Normal)), maxEdge);  // the server sends GL normals
      if (layout == MapLayout::Arm) {
        const QImage arm = loadLinear(paths.value(MapType::Arm));
        d.orm = eng::composeOrm(arm, ch.ao, arm, ch.rough, arm, ch.metal);
      } else if (layout == MapLayout::Separate) {
        d.orm = eng::composeOrm(loadLinear(paths.value(MapType::Ao)), ch.ao, loadLinear(paths.value(MapType::Roughness)),
                                ch.rough, loadLinear(paths.value(MapType::Metalness)), ch.metal);
      }
      d.orm = clampSize(d.orm, maxEdge);
      d.emission = clampSize(loadImage(paths.value(MapType::Emission)), maxEdge);
      d.error = d.diffuse.isNull();
      QMutexLocker lock(&m_mutex);
      m_ready.push_back(std::move(d));
    });
  };
  if (wants.isEmpty()) {
    finished();
    return;
  }
  material->loadedAoChannel = ch.ao;
  material->loadedRoughnessChannel = ch.rough;
  material->loadedMetallicChannel = ch.metal;
  for (const auto &w: wants) {
    net::FileFetcher::instance().fetch(net::FileKind::Editor, w.file, net::FetchPriority::Scene, m_signals,
                                       [batch, w, finished](const QString &path) {
                                         if (!path.isEmpty())
                                           batch->paths.insert(w.type, path);
                                         if (--batch->remaining == 0)
                                           finished();
                                       });
  }
}

void TextureManager::ensureGpu(eng::Material *material, QRhiResourceUpdateBatch *rub) {
  Q_UNUSED(rub);
  if (!material || !Shared::instance().isValid())
    return;
  auto &shared = Shared::instance();
  // Bound on another window's QRhi: load again for this one.
  if (material->gpuRhi && material->gpuRhi != shared.rhi())
    dropGpu(*material);

  // Variants mirror their base material's maps.
  if (material->gpuSource) {
    eng::Material *base = material->gpuSource.data();
    ensureGpu(base, rub);
    if (material->albedo && material->gpuSourceVersion == base->version && material->gpuRhi == base->gpuRhi)
      return;
    material->gpuRhi = base->gpuRhi;
    material->albedo = base->albedo;
    material->normal = base->normal;
    material->metallicRoughness = base->metallicRoughness;
    material->emission = base->emission;
    material->realNormal = base->realNormal;
    material->realOrm = base->realOrm;
    material->realEmission = base->realEmission;
    material->state = base->state;
    material->ownsTextures = false;
    material->gpuSourceVersion = base->version;
    material->touch();
    return;
  }

  if (material->albedo)
    return;

  // Something valid has to be bound from this frame on, so the face draws
  // immediately with the fallbacks and picks up its real maps when they land.
  const bool error = material->state == eng::Material::State::Error;
  material->albedo = error ? shared.fallbackChecker() : shared.fallbackGray();
  material->normal = shared.fallbackNormal();
  material->metallicRoughness = shared.fallbackWhite();
  material->emission = shared.fallbackBlack();
  material->ownsTextures = false;
  material->gpuRhi = shared.rhi();
  material->touch();
  if (error || m_pending.contains(material))
    return;
  material->loadTicket = ++m_ticket;

  if (material->kind == eng::Material::Kind::Server) {
    // Waiting for the catalogue entry: nothing to download yet (and no
    // continuous redraw); applyEntry() drops the fallbacks when it arrives.
    if (!material->hasEntry())
      return;
    m_pending.insert(material);
    const eng::MaterialPtr strong = m_private.contains(material) ? m_private.value(material).toStrongRef()
                                                                 : m_byName.value(material->name);
    if (strong.isNull())
      return;
    QMetaObject::invokeMethod(m_signals, [this, strong] { startServerLoad(strong); }, Qt::AutoConnection);
    return;
  }

  // Library / file materials: decode straight from disk.
  m_pending.insert(material);
  const int maxEdge = m_maxTextureSize;
  const Channels ch = channelsOf(material->props);
  material->loadedAoChannel = ch.ao;
  material->loadedRoughnessChannel = ch.rough;
  material->loadedMetallicChannel = ch.metal;
  struct LibMaps {
    QString albedo, normal, rough, metal, ao, arm, emissive;
    bool dx = false;
  } lib{material->libAlbedo, material->libNormal, material->libRoughness, material->libMetallic,
        material->libAo,     material->libArm,    material->libEmissive,  material->libNormalDirectX};
  QThreadPool::globalInstance()->start([this, material, ticket = material->loadTicket, maxEdge, lib, ch] {
    Decoded d;
    d.material = material;
    d.ticket = ticket;
    d.diffuse = clampSize(loadImage(lib.albedo.isEmpty() ? kDevFallback : lib.albedo), maxEdge);
    if (d.diffuse.isNull())
      d.diffuse = loadImage(kDevFallback);
    d.normal = clampSize(loadLinear(lib.normal), maxEdge);
    if (lib.dx && !d.normal.isNull())
      flipGreen(d.normal);
    if (!lib.arm.isEmpty()) {
      const QImage arm = loadLinear(lib.arm);
      d.orm = eng::composeOrm(arm, ch.ao, arm, ch.rough, arm, ch.metal);
    } else {
      d.orm = eng::composeOrm(loadLinear(lib.ao), ch.ao, loadLinear(lib.rough), ch.rough, loadLinear(lib.metal),
                              ch.metal);
    }
    d.orm = clampSize(d.orm, maxEdge);
    d.emission = clampSize(loadImage(lib.emissive), maxEdge);
    QMutexLocker lock(&m_mutex);
    m_ready.push_back(std::move(d));
  });
}

bool TextureManager::pumpUploads(QRhiResourceUpdateBatch *rub, int budget) {
  if (!rub || !Shared::instance().isValid())
    return false;
  QVector<Decoded> batch;
  {
    QMutexLocker lock(&m_mutex);
    if (m_ready.isEmpty())
      return false;
    batch.swap(m_ready);
  }

  auto &shared = Shared::instance();
  QVector<Decoded> later;
  bool any = false;
  for (auto &d: batch) {
    eng::Material *m = d.material;
    // Dropped meanwhile (reload, error, rename, released) or reloaded since.
    if (!m_pending.contains(m) || m->loadTicket != d.ticket)
      continue;
    if (m->gpuRhi != shared.rhi() || budget <= 0) {
      if (Shared::forRhi(m->gpuRhi))
        later.push_back(std::move(d));  // another window's frame uploads it
      continue;
    }
    --budget;
    any = true;
    m_pending.remove(m);
    if (d.error) {
      m->state = eng::Material::State::Error;
      m->albedo = shared.fallbackChecker();
      m->touch();
      continue;
    }
    QRhiTexture *albedo = uploadImage(d.diffuse, true, rub);
    QRhiTexture *normal = uploadImage(d.normal, false, rub);
    QRhiTexture *orm = uploadImage(d.orm, false, rub);
    QRhiTexture *emission = uploadImage(d.emission, true, rub);
    // The material's SRB still points at the fallbacks; touch() makes the
    // engine rebuild (and retire) it before the next draw.
    m->albedo = albedo ? albedo : shared.fallbackChecker();
    m->normal = normal ? normal : shared.fallbackNormal();
    m->metallicRoughness = orm ? orm : shared.fallbackWhite();
    m->emission = emission ? emission : shared.fallbackBlack();
    m->ownedAlbedo = albedo;
    m->ownedNormal = normal;
    m->ownedMr = orm;
    m->ownedEmission = emission;
    m->ownsTextures = true;
    m->realNormal = normal != nullptr;
    m->realOrm = orm != nullptr;
    m->realEmission = emission != nullptr;
    m->state = albedo ? eng::Material::State::Ready : eng::Material::State::Error;
    m->touch();
  }
  if (!later.isEmpty()) {
    QMutexLocker lock(&m_mutex);
    later.append(std::move(m_ready));
    m_ready.swap(later);
  }
  return any;
}

int TextureManager::pendingCount() const {
  return m_pending.size();
}

int TextureManager::pendingCount(QRhi *rhi) const {
  int n = 0;
  for (const eng::Material *m: m_pending)
    n += m->gpuRhi == rhi ? 1 : 0;
  return n;
}

void TextureManager::releaseSrbs(eng::Material &m, QRhi *rhi) {
  auto &shared = Shared::instance();
  for (QRhiShaderResourceBindings **srb: {&m.srb, &m.srbShadow})
    if (*srb && (!rhi || (*srb)->rhi() == rhi)) {
      shared.retireShaderResourceBindings(*srb);
      *srb = nullptr;
    }
  if (!m.srb && !m.srbShadow)
    m.srbOwner = nullptr;
}

void TextureManager::releaseRhi(QRhi *rhi) {
  QList<eng::MaterialPtr> all = allMaterials();
  for (const auto &weak: std::as_const(m_private))
    if (const eng::MaterialPtr m = weak.toStrongRef())
      all.append(m);
  for (const auto &m: all) {
    releaseSrbs(*m, rhi);
    if (m->gpuRhi == rhi) {
      dropGpu(*m);
      m->touch();
    }
  }
}

void TextureManager::releaseSrbsOf(const void *owner) {
  QList<eng::MaterialPtr> all = allMaterials();
  for (const auto &weak: std::as_const(m_private))
    if (const eng::MaterialPtr m = weak.toStrongRef())
      all.append(m);
  for (const auto &m: all)
    if (m->srbOwner == owner)
      releaseSrbs(*m, nullptr);
}

eng::MaterialPtr TextureManager::newPrivate(const QString &ref) {
  auto *raw = new eng::Material(ref.toLower());
  raw->ref = ref;
  eng::MaterialPtr m(raw, [](eng::Material *p) {
    TextureManager::instance().forgetPrivate(p);
    delete p;
  });
  m_private.insert(raw, m.toWeakRef());
  return m;
}

void TextureManager::forgetPrivate(eng::Material *m) {
  m_private.remove(m);
  releaseSrbs(*m, nullptr);
  dropGpu(*m);
}

eng::MaterialPtr TextureManager::privateMaterial(const QString &nameIn) {
  const QString name = nameIn.trimmed();
  const eng::MaterialPtr m = newPrivate(name);
  const QString key = m->name;
  if (isBuiltInName(key) && !resolveFromLibrary(*m)) {
    useDevFallback(*m);
  } else if (resolveFromLibrary(*m)) {
    m->kind = eng::Material::Kind::Library;
    refreshMaterialProps(*m);
  } else if (const TextureEntry *e = net::TextureCatalog::instance().find(name)) {
    applyEntry(m, *e);
  } else {
    useDevFallback(*m);
  }
  return m;
}

eng::MaterialPtr TextureManager::privateMaterial(const TextureEntry &entry, const MaterialProps &props) {
  const eng::MaterialPtr m = newPrivate(entry.ref);
  applyEntry(m, entry);
  setPrivateProps(*m, props);
  return m;
}

void TextureManager::setPrivateProps(eng::Material &m, const MaterialProps &props) {
  // The ORM image bakes the channels in.
  const Channels want = channelsOf(props);
  if (m.realOrm && (want.ao != m.loadedAoChannel || want.rough != m.loadedRoughnessChannel ||
                    want.metal != m.loadedMetallicChannel)) {
    dropGpu(m);
    m.touch();
  }
  if (props != m.props) {
    m.props = props;
    eng::Material::propsChanged();
  }
}

MaterialEvents &MaterialEvents::instance() {
  static MaterialEvents *e = new MaterialEvents();
  return *e;
}

void MaterialEvents::notifyRenamed(const QString &oldRef, const QString &newRef) { emit renamed(oldRef, newRef); }

} // namespace vk
