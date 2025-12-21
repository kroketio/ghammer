#include "texture_actions.h"

#include <QApplication>
#include <QClipboard>
#include <QMenu>

#include "client/engine/sky_info.h"
#include "client/net/texture_catalog.h"
#include "shared/lib/config.h"
#include "client/ui/action_manager.h"
#include "client/vulkan/vk_engine.h"

namespace ui {

TextureHub &TextureHub::instance() {
  static TextureHub *h = new TextureHub();
  return *h;
}

QStringList recentTextures() {
  return config()->getRaw(QStringLiteral("Textures/Recent")).toStringList();
}

void noteRecentTexture(const QString &ref) {
  const MaterialRef r = MaterialRef::parse(ref);
  if (r.isLegacy() || r.isEmpty())
    return;
  QStringList list = recentTextures();
  list.removeIf([&](const QString &s) { return refKey(s) == refKey(r.base()); });
  list.prepend(r.base());
  while (list.size() > 50)
    list.removeLast();
  config()->setRaw(QStringLiteral("Textures/Recent"), list);
}

QString useTexture(vk::Engine3D *engine, const QString &ref) {
  if (!engine || ref.isEmpty())
    return {};
  const QString base = MaterialRef::parse(ref).base();
  if (const TextureEntry *e = net::TextureCatalog::instance().find(base); e && e->isSky()) {
    engine->applySkybox(base, eng::skyInfoFromJson(e->sky));
    return QObject::tr("Skybox: %1").arg(base);
  }
  engine->setCurrentMaterial(ref);
  engine->applyMaterialToSelection(ref);
  return QObject::tr("Applied %1").arg(ref);
}

void populateTextureMenu(QMenu &menu, vk::Engine3D *engine, const QString &ref, QWidget *parent, bool inBrowser) {
  const MaterialRef r = MaterialRef::parse(ref);
  const TextureEntry *e = net::TextureCatalog::instance().find(r.base());
  auto &hub = TextureHub::instance();

  if (e && e->isSky()) {
    menu.addAction(ActionManager::icon(QStringLiteral("world")), QObject::tr("Use as Skybox"), parent,
                   [engine, ref] { useTexture(engine, ref); });
    menu.addSeparator();
    menu.addAction(ActionManager::icon(QStringLiteral("copydown")), QObject::tr("Copy reference"), parent,
                   [ref = r.base()] { QApplication::clipboard()->setText(ref); });
    return;
  }

  menu.addAction(ActionManager::icon(QStringLiteral("brush_data")), QObject::tr("Apply to selection"), parent,
                 [engine, ref] {
                   if (engine) {
                     engine->setCurrentMaterial(ref);
                     engine->applyMaterialToSelection(ref);
                   }
                 });
  menu.addAction(QObject::tr("Set as current material"), parent, [engine, ref] {
    if (engine)
      engine->setCurrentMaterial(ref);
  });
  menu.addAction(ActionManager::icon(QStringLiteral("restrict_select_off")), QObject::tr("Select faces using it"),
                 parent, [engine, ref] {
                   if (engine)
                     engine->selectFacesWithMaterial(ref);
                 });
  menu.addSeparator();
  menu.addAction(ActionManager::icon(QStringLiteral("material")), QObject::tr("Texture properties…"), parent,
                 [&hub, ref] { emit hub.showPropertiesRequested(ref); });
  if (e) {
    // Base (read-only) textures are copied first; user-pack ones are edited in place.
    if (e->base) {
      menu.addAction(ActionManager::icon(QStringLiteral("duplicate")), QObject::tr("Copy into a pack…"), parent,
                     [&hub, ref = r.base()] { emit hub.copyRequested(ref); });
      QAction *edit = menu.addAction(ActionManager::icon(QStringLiteral("greasepencil")),
                                     QObject::tr("Edit maps… (copy first)"), parent,
                                     [&hub, ref = r.base()] { emit hub.copyRequested(ref); });
      edit->setToolTip(QObject::tr("Base textures are read-only: make a copy in your project pack first."));
    } else {
      QMenu *edit = menu.addMenu(ActionManager::icon(QStringLiteral("greasepencil")),
                                 QObject::tr("Edit map externally"));
      addEditMapActions(edit, *e, parent);
    }
    menu.addSeparator();
    if (!inBrowser)
      menu.addAction(ActionManager::icon(QStringLiteral("viewzoom")), QObject::tr("Show in Texture Browser"), parent,
                     [&hub, ref = r.base()] { emit hub.openBrowserRequested(ref); });
    menu.addAction(ActionManager::icon(QStringLiteral("file_folder")), QObject::tr("Show in Texture Packs"), parent,
                   [&hub, ref = r.base()] { emit hub.openPacksRequested(ref); });
  }
  menu.addAction(ActionManager::icon(QStringLiteral("copydown")), QObject::tr("Copy reference"), parent,
                 [ref] { QApplication::clipboard()->setText(ref); });
}

bool addEditMapActions(QMenu *menu, const TextureEntry &e, QObject *context) {
  if (e.base || !e.isValid())
    return false;
  auto &hub = TextureHub::instance();
  for (auto it = e.maps.begin(); it != e.maps.end(); ++it) {
    QAction *a = menu->addAction(mapTypeLabel(it.key()), context, [&hub, ref = e.ref, key = QString::fromLatin1(
                                                                          mapTypeKey(it.key()))] {
      emit hub.editRequested(ref, key);
    });
    a->setToolTip(QObject::tr("Opens %1 in the image editor (Preferences › External applications); saving "
                              "uploads it.")
                      .arg(it->file));
  }
  menu->setToolTipsVisible(true);
  return !e.maps.isEmpty();
}

} // namespace ui
