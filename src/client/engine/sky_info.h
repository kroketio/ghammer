#pragma once

#include <QColor>
#include <QJsonObject>
#include <QString>

// A skybox's lighting from its pack JSON "sky" entry (Source light_environment / env_fog_controller values).
namespace eng {

struct SkyInfo {
  bool valid = false;
  QString title;        // "Clear Morning / 17"
  QString time;         // "Morning"
  QString weather;      // "Clear"
  QString source;       // web page
  double yaw = 0.0;     // light_environment yaw the light travels toward (degrees)
  double pitch = -45.0; // -90 = straight down
  QColor sunColor = QColor(255, 255, 255);
  double sunBrightness = 300.0;  // Source _light brightness
  QColor ambientColor = QColor(128, 128, 128);
  double ambientBrightness = 200.0;  // Source _ambient brightness
  QColor fogPrimary, fogSecondary;   // invalid when the sky gives none
};

SkyInfo skyInfoFromJson(const QJsonObject &sky);

struct SkyLighting {
  float sunAzimuth = 0.0f;    // Engine3D::setSunAzimuth (0 = +X, 90 = -Z)
  float sunElevation = 45.0f; // degrees above the horizon
  QColor sunColor;
  float sunIntensity = 0.0f;
  QColor ambientColor;
  float ambientIntensity = 0.0f;
  QColor fogColor;            // invalid: leave the fog alone
};

// u = atan2(x, -z) / 2pi as Godot maps panoramas: azimuth = yaw + 90, elevation = -pitch. Brightness scales so
// Source's typical day (sun 300, ambient 250) gives the editor defaults (3.678, 1.347).
SkyLighting skyLighting(const SkyInfo &info);

} // namespace eng
