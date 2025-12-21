#pragma once

#include <QObject>

#include "shared/models/texture_entry.h"
#include <QString>
#include <QStringList>

class QMenu;
class QWidget;

namespace vk {
class Engine3D;
}

namespace ui {

// Requests from texture views; the main window routes them, so the views stay independent.
class TextureHub final : public QObject {
  Q_OBJECT

public:
  static TextureHub &instance();

signals:
  void showPropertiesRequested(const QString &ref);   // Inspector section, or the browser
  void openBrowserRequested(const QString &ref);      // Texture Browser window (reveal ref)
  void openPacksRequested(const QString &ref);        // Texture Packs window (select ref)
  void copyRequested(const QString &ref);             // Copy-to-pack dialog
  void editRequested(const QString &ref, const QString &mapType);  // external editor session

private:
  TextureHub() = default;
};

// Newest first, at most 50, kept in the editor config.
QStringList recentTextures();
void noteRecentTexture(const QString &ref);

// One entry per map, opening it in the external image editor. Base-pack textures: adds nothing, false.
bool addEditMapActions(QMenu *menu, const TextureEntry &e, QObject *context);

// A skybox → Engine3D::applySkybox, else onto the selected faces. Returns the status line.
QString useTexture(vk::Engine3D *engine, const QString &ref);

// The texture card context menu.
void populateTextureMenu(QMenu &menu, vk::Engine3D *engine, const QString &ref, QWidget *parent,
                         bool inBrowser = false);

} // namespace ui
