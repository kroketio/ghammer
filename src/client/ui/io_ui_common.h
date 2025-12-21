#pragma once

// Helpers shared by the entity I/O widgets.

#include <QColor>
#include <QIcon>
#include <QPainter>
#include <QPixmap>
#include <QString>

#include <vector>

#include "client/engine/entity.h"
#include "client/engine/io.h"
#include "client/vulkan/vk_engine.h"

namespace ui::iocommon {

inline const QColor kOk(110, 210, 130);
inline const QColor kBroken(255, 110, 110);
inline const QColor kWarn(255, 200, 90);
inline const QColor kRuntime(120, 180, 255);
inline const QColor kDisabled(130, 136, 150);
inline const QColor kMuted(138, 154, 184);
inline const QColor kAccent(79, 209, 255);  // outputs / leaving
inline const QColor kIncoming(255, 158, 64); // inputs / arriving

// A connection: its owner and its index in Entity::outputs.
struct ConnRef {
  quint32 entity = 0;
  int row = -1;
  bool operator==(const ConnRef &o) const = default;
};

inline QIcon dot(const QColor &c, int size = 10) {
  QPixmap pm(size, size);
  pm.fill(Qt::transparent);
  QPainter p(&pm);
  p.setRenderHint(QPainter::Antialiasing);
  p.setPen(Qt::NoPen);
  p.setBrush(c);
  p.drawEllipse(QRectF(1, 1, size - 2, size - 2));
  return QIcon(pm);
}

inline const std::vector<eng::Entity> &entitiesOf(const vk::Engine3D *engine) {
  static const std::vector<eng::Entity> none;
  const auto *v = engine ? engine->sceneEntities() : nullptr;
  return v ? *v : none;
}

inline QString entityLabel(const eng::Entity &e) {
  if (!e.name.isEmpty())
    return e.name;
  const eng::EntityClass *c = e.cls();
  return QStringLiteral("(%1)").arg(c ? c->label.toLower() : e.classname);
}

inline QString delayText(const float d) {
  return d > 0.0f ? QStringLiteral("%1 s").arg(QString::number(double(d), 'g', 4)) : QString();
}

// "OnTrigger → door.Open(1) · 1 s" for lists.
inline QString shortText(const eng::io::Connection &c) {
  QString s = QStringLiteral("%1 → %2.%3").arg(c.output, c.target.isEmpty() ? QStringLiteral("?") : c.target,
                                              c.input.isEmpty() ? QStringLiteral("?") : c.input);
  if (!c.param.isEmpty())
    s += QStringLiteral("(%1)").arg(c.param);
  if (c.delay > 0.0f)
    s += QStringLiteral(" · %1").arg(delayText(c.delay));
  if (c.times == 1)
    s += QStringLiteral(" · once");
  else if (c.times > 1)
    s += QStringLiteral(" · ×%1").arg(c.times);
  return s;
}

// The connection's worst issue as a colour.
inline QColor statusColor(const std::vector<eng::Entity> &ents, const eng::Entity &owner, int row,
                          QStringList *messages = nullptr) {
  if (row < 0 || row >= owner.outputs.size())
    return kBroken;
  if (owner.outputs[row].disabled)
    return kDisabled;
  QColor c = kOk;
  for (const auto &is: eng::io::validateConnection(ents, owner, row)) {
    if (messages)
      *messages << is.text;
    if (is.severity == eng::io::Issue::Severity::Error)
      c = kBroken;
    else if (is.severity == eng::io::Issue::Severity::Warning && c != kBroken)
      c = kWarn;
  }
  return c;
}

} // namespace ui::iocommon
