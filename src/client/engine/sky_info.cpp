#include "sky_info.h"

#include <algorithm>
#include <cmath>

#include <QJsonArray>

namespace eng {
namespace {

constexpr double kSunPerSource = 3.678 / 300.0;
constexpr double kAmbientPerSource = 1.347 / 250.0;

QColor colorOf(const QJsonValue &v, double *brightness = nullptr) {
  const QJsonArray a = v.toArray();
  if (a.size() < 3)
    return {};
  const auto ch = [&a](int i) { return std::clamp(a.at(i).toInt(), 0, 255); };
  if (brightness && a.size() >= 4)
    *brightness = a.at(3).toDouble();
  return QColor(ch(0), ch(1), ch(2));
}

} // namespace

SkyInfo skyInfoFromJson(const QJsonObject &sky) {
  SkyInfo s;
  if (sky.isEmpty())
    return s;
  s.valid = true;
  s.title = sky.value(QStringLiteral("title")).toString();
  s.time = sky.value(QStringLiteral("time")).toString();
  s.weather = sky.value(QStringLiteral("weather")).toString();
  s.source = sky.value(QStringLiteral("source")).toString();
  const QJsonArray angles = sky.value(QStringLiteral("sun_angles")).toArray();
  if (angles.size() >= 2)
    s.yaw = angles.at(1).toDouble();
  // Source's "pitch" key wins over the angles' pitch, as in light_environment.
  if (sky.contains(QStringLiteral("sun_pitch")))
    s.pitch = sky.value(QStringLiteral("sun_pitch")).toDouble();
  else if (!angles.isEmpty())
    s.pitch = angles.at(0).toDouble();
  if (const QColor c = colorOf(sky.value(QStringLiteral("sun_light")), &s.sunBrightness); c.isValid())
    s.sunColor = c;
  if (const QColor c = colorOf(sky.value(QStringLiteral("ambient")), &s.ambientBrightness); c.isValid())
    s.ambientColor = c;
  s.fogPrimary = colorOf(sky.value(QStringLiteral("fog_primary")));
  s.fogSecondary = colorOf(sky.value(QStringLiteral("fog_secondary")));
  return s;
}

SkyLighting skyLighting(const SkyInfo &info) {
  SkyLighting l;
  double az = std::fmod(info.yaw + 90.0, 360.0);
  if (az > 180.0)
    az -= 360.0;
  else if (az < -180.0)
    az += 360.0;
  l.sunAzimuth = float(az);
  l.sunElevation = float(std::clamp(-info.pitch, 0.0, 90.0));
  l.sunColor = info.sunColor;
  l.sunIntensity = float(std::clamp(info.sunBrightness * kSunPerSource, 0.0, 50.0));
  l.ambientColor = info.ambientColor;
  l.ambientIntensity = float(std::clamp(info.ambientBrightness * kAmbientPerSource, 0.0, 10.0));
  l.fogColor = info.fogPrimary;
  return l;
}

} // namespace eng
