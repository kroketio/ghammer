#include <QAction>
#include <QDir>
#include <QQmlEngine>
#include <filesystem>

#include "shared/lib/utils.h"

#include "ctx.h"

#include "client/prefs/preferences.h"
#include "client/ui/action_manager.h"
#include "client/ui/scale.h"
#include "client/vulkan/vk_engine.h"

Ctx::Ctx() {
  g_instance = this;

  gs::configRoot = QDir::homePath();
  gs::homeDir = QDir::homePath();
  gs::configDirectory = QString("%1/.config/%2/").arg(gs::configRoot, QCoreApplication::applicationName());
  gs::configDirectoryAssets = QString("%1%2").arg(gs::configDirectory, "data");
  gs::configDirectoryAssetsTextures = QString("%1/%2").arg(gs::configDirectoryAssets, "textures");
  gs::configDirectoryAssetsModels = QString("%1/%2").arg(gs::configDirectoryAssets, "models");
  gs::cacheDirectory = QString("%1/cache").arg(gs::configDirectory);

  gs::pathDatabase = QFileInfo(gs::configDirectory + QDir::separator() + "db.sqlite3");

  createConfigDirectory(QStringList({
    gs::configDirectory,
    gs::cacheDirectory,
    gs::configDirectoryAssets,
    gs::configDirectoryAssetsModels
  }));

  qmlRegisterUncreatableMetaObject(g::staticMetaObject, "GlobalEnums", 1, 0, "Editor", "Enums only");

  auto applyScale = [this] {
    const double f = ui::scale::factor();
    if (!qFuzzyCompare(f, scaleFactor)) {
      scaleFactor = f;
      emit scaleFactorChanged(scaleFactor);
    }
    const double t = ui::scale::textFactor();
    if (!qFuzzyCompare(t, textScaleFactor)) {
      textScaleFactor = t;
      emit textScaleFactorChanged(textScaleFactor);
    }
  };
  ui::scale::bind(this, applyScale);
  applyScale();

  switchMode(g::EditorMode::OBJECT);
}

void Ctx::createConfigDirectory(const QStringList &lst) {
  for (const auto &d: lst) {
    if (!std::filesystem::exists(d.toStdString())) {
      qDebug() << QString("Creating directory: %1").arg(d);
      if (!QDir().mkpath(d))
        throw std::runtime_error("Could not create directory " + d.toStdString());
    }
  }
}

QString Ctx::shortcutSuffix(const QString &actionId, int) const {
  const QAction *a = ui::act(actionId);
  return a && !a->shortcut().isEmpty() ? QStringLiteral(" (%1)").arg(a->shortcut().toString(QKeySequence::NativeText))
                                       : QString();
}

void Ctx::setScaleFactor(const double value) {
  // The preference clamps; ui::scale applies it.
  prefs::get().setValue(QStringLiteral("General/UiScale"), value);
}

void Ctx::switchMode(g::EditorMode mode) {
  if (mode == editorMode)
    return;

  if (mode == g::EditorMode::EDIT)
    modeBeforeEdit = editorMode;
  editorMode = mode;
  g::editorMode = mode;

  if (g::engine3D) {
    g::engine3D->setMaterialMode(mode == g::EditorMode::MATERIAL);
    g::engine3D->setEditMode(mode == g::EditorMode::EDIT);
  }

  emit editorModeChanged(editorMode);
}

void Ctx::syncMode(g::EditorMode mode) {
  if (mode == editorMode)
    return;
  editorMode = mode;
  g::editorMode = mode;
  emit editorModeChanged(editorMode);
}

Ctx* g_instance = nullptr;
