#pragma once

#include <QPointer>
#include <QQuickRhiItem>

namespace vk {

class Engine3D;

// The 3D viewport: rendering lives in Engine3D; this item hands it the RHI command buffer and render target.
class Viewport3D : public QQuickRhiItem {
  Q_OBJECT
  // 0 / 2 / 4. 0 is "MSAA off", which the RHI spells as a sample count of 1.
  Q_PROPERTY(int msaaSamples READ msaaSamples WRITE setMsaaSamples NOTIFY msaaSamplesChanged)
  // Unset: the editor's engine (g::engine3D).
  Q_PROPERTY(QObject *engine READ engineObject WRITE setEngineObject NOTIFY engineChanged)

public:
  explicit Viewport3D(QQuickItem *parent = nullptr);

  [[nodiscard]] int msaaSamples() const { return m_msaaSamples; }
  void setMsaaSamples(int samples);

  // Clamps the requested sample count to what the device supports. Called from synchronize() (live QRhi).
  void clampMsaaToSupported(QRhi *rhi);

  QQuickRhiItemRenderer *createRenderer() override;

  [[nodiscard]] Engine3D *engine() const;
  void setEngine(Engine3D *engine);
  [[nodiscard]] QObject *engineObject() const;
  void setEngineObject(QObject *engine);

signals:
  void msaaSamplesChanged();
  void engineChanged();

private:
  // On-demand redraw: polls the scene's change counters at display rate (GUI thread) and schedules a frame only
  // when one moved, or when the engine / the "redraw continuously" preference asks.
  void pollForChanges();

  QPointer<Engine3D> m_engine;
  QMetaObject::Connection m_redrawConn;
  int m_msaaSamples = 2;
  class QTimer *m_poll = nullptr;
  quint64 m_lastSignature = 0;
  bool m_continuous = false;
};

class Viewport3DRenderer final : public QQuickRhiItemRenderer {
  QPointer<Engine3D> m_engine;

protected:
  void initialize(QRhiCommandBuffer *cb) override;
  void synchronize(QQuickRhiItem *item) override;
  void render(QRhiCommandBuffer *cb) override;
};

} // namespace vk
