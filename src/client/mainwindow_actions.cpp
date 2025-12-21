// Every editor command, registered with the ActionManager, plus the menu bar and the QML viewport menus.

#include <QApplication>
#include <QDockWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QDir>
#include <QFileInfo>
#include <QInputDialog>
#include <QVBoxLayout>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDialog>
#include <QLineEdit>

#include "client/engine/brush_select.h"
#include "client/engine/face_select.h"
#include "client/engine/scene.h"
#include "client/ui/scale.h"
#include <QQuickItem>

#include "client/ctx.h"
#include "client/net/asset_client.h"
#include "client/net/local_server.h"
#include "client/prefs/preferences.h"
#include "client/prefs/preferences_dialog.h"
#include "client/ui/action_manager.h"
#include "client/ui/command_palette.h"
#include "client/ui/io_editor.h"
#include "client/ui/layout_designer.h"
#include "client/ui/pie_menu.h"
#include "client/ui/replace_texture_dialog.h"
#include "client/ui/about_dialog.h"
#include "client/ui/texture_browser.h"
#include "client/ui/texture_packs.h"
#include "client/ui/texture_actions.h"
#include "client/vulkan/vk_engine.h"
#include "mainwindow.h"
#include "client/engine/entity.h"
#include "client/project/document_manager.h"
#include "client/project/project_manager.h"
#include "ghammer_version.h"
#include <QActionGroup>
#include "client/ui/external_apps.h"
#include "client/ui/file_dialog.h"
#include "client/ui/pack_install_window.h"
#include <QTimer>
#include <QCursor>

using ui::ActionManager;

namespace {

vk::Engine3D *E() { return g::engine3D; }
bool hasScene() { return E() && E()->scene(); }
bool hasSelection() {
  return E() && (!E()->selectedObjects().empty() || !E()->selectedFaces().empty() || !E()->selectedMods().empty()
                 || !E()->selectedPaths().empty() || !E()->selectedEntities().empty());
}
bool hasBrushSelection() { return E() && !E()->selectedObjects().empty(); }
// Ctrl+D: brushes, modified objects and point entities.
bool hasDuplicable() {
  return E() && (!E()->selectedObjects().empty() || !E()->selectedMods().empty() || !E()->selectedEntities().empty());
}
bool hasIOSelection() { return E() && !E()->ioSelectionEntities().empty(); }
bool hasFaceTargets() { return E() && (!E()->selectedObjects().empty() || !E()->selectedFaces().empty()); }

} // namespace

void MainWindow::registerActions() {
  auto &am = ActionManager::instance();
  using Spec = ActionManager::Spec;

  am.add(Spec{"file.newProject", tr("&Project…"), "file_new", "Ctrl+Shift+N", tr("Create a new project"), "File"},
         [this] { openProjectDialog(true); });
  am.add(Spec{"file.new", tr("&Blank Level"), "file_3d", "Ctrl+N", tr("A new, empty level"), "File"},
         [this] { newLevelDialog(); });
  am.add(Spec{"file.newPlayground", tr("&Playground Level"), "file_3d", "",
              tr("A new level with a walled floor, a rigid-body crate and a light"), "File"},
         [this] { newPlayground(); });
  am.add(Spec{"file.open", tr("&Open…"), "file_folder", "Ctrl+O", tr("Open a project"), "File"},
         [this] { openProjectDialog(false); });
  am.add(Spec{"file.openLevel", tr("Open &Level…"), "file_3d", "Ctrl+Shift+O", tr("Open a level (.ghmap, .vmf) in a tab"),
              "File"},
         [this] { openLevelDialog(); });
  am.add(Spec{"file.save", tr("&Save"), "file_tick", "Ctrl+S",
              tr("Save every level with changes (and export them on save)"), "File"},
         [this] { saveAll(); });
  am.add(Spec{"file.saveAs", tr("Save Level &As…"), "file_tick", "Ctrl+Shift+S", tr("Save the level to another file"),
              "File"},
         [this] { saveLevelAs(); }, [this] { return m_docs && m_docs->engineLevel(); });
  am.add(Spec{"file.projectSettings", tr("Project Se&ttings…"), "file_folder", "", tr("The open project's settings"),
              "File"},
         [this] { prefs::PreferencesDialog::showDialog(this, QStringLiteral("Project")); },
         [] { return proj::currentProject() != nullptr; });
  am.add(Spec{"file.levelSettings", tr("Le&vel Settings…"), "file_3d", "", tr("The level's own settings"), "File"},
         [this] { prefs::PreferencesDialog::showDialog(this, QStringLiteral("Level")); },
         [this] { return m_docs && m_docs->engineLevel(); });
  am.add(Spec{"file.exportGodot", tr("Export to &Godot…"), "export", "",
              tr("Export the level into a Godot project: textures, materials, meshes, the level scene and its I/O"),
              "File"},
         [this] { exportGodot(); }, hasScene);
  am.add(Spec{"file.startGodot", tr("Start &Godot"), "Godot", "",
              tr("Open the Godot project levels export to in the Godot editor"), "File"},
         [this] {
           if (const QString dir = godotStartDir(); !dir.isEmpty())
             ui::external::startGodot(dir, this);
         },
         [this] { return !godotStartDir().isEmpty(); });
  am.add(Spec{"file.exportOnSave", tr("Godot &Auto-Export"), "export", "",
              tr("Godot auto-export: export to the chosen project on every save"), "File", true},
         [this] { setExportOnSave(!exportOnSave()); }, {}, [this] { return exportOnSave(); });
  am.add(Spec{"file.prefs", tr("&Preferences…"), "preferences", "Ctrl+,", tr("Editor preferences"), "File"},
         [this] { prefs::PreferencesDialog::showDialog(this); });
  am.add(Spec{"file.quit", tr("&Quit"), "", "Ctrl+Q", {}, "File"}, [this] { close(); });

  // Documents (the tabs)
  const auto stepDocument = [this](int dir) {
    const int n = m_docs->count();
    if (n < 2)
      return;
    QString error;
    if (!m_docs->activate((m_docs->activeIndex() + dir + n) % n, &error) && !error.isEmpty())
      showToast(error);
  };
  am.add(Spec{"doc.next", tr("&Next Document"), "", "Ctrl+Tab", tr("Show the next tab"), "Documents"},
         [stepDocument] { stepDocument(1); }, [this] { return m_docs && m_docs->count() > 1; });
  am.add(Spec{"doc.previous", tr("&Previous Document"), "", "Ctrl+Shift+Tab", tr("Show the previous tab"), "Documents"},
         [stepDocument] { stepDocument(-1); }, [this] { return m_docs && m_docs->count() > 1; });
  am.add(Spec{"doc.nextAlt", tr("Next Document (alternate)"), "", "Alt+Right", tr("Show the next tab"), "Documents"},
         [stepDocument] { stepDocument(1); }, [this] { return m_docs && m_docs->count() > 1; });
  am.add(Spec{"doc.previousAlt", tr("Previous Document (alternate)"), "", "Alt+Left", tr("Show the previous tab"),
              "Documents"},
         [stepDocument] { stepDocument(-1); }, [this] { return m_docs && m_docs->count() > 1; });
  am.add(Spec{"doc.close", tr("&Close Document"), "", "Ctrl+F4", tr("Close the tab shown"), "Documents"},
         [this] { closeDocument(m_docs->activeIndex()); }, [this] { return m_docs && m_docs->active() != nullptr; });

  am.add(Spec{"edit.undo", tr("&Undo"), "loop_back", "Ctrl+Z", {}, "Edit"}, [] { E()->undo(); },
         [] { return E() && E()->canUndo(); });
  am.add(Spec{"edit.redo", tr("&Redo"), "loop_forwards", "Ctrl+Shift+Z", {}, "Edit"}, [] { E()->redo(); },
         [] { return E() && E()->canRedo(); });
  am.add(Spec{"edit.redoAlt", tr("Redo (alternate)"), "", "Ctrl+Y", {}, "Edit"}, [] { E()->redo(); },
         [] { return E() && E()->canRedo(); });
  am.add(Spec{"edit.copy", tr("&Copy"), "copydown", "Ctrl+C", tr("Copy the selected brushes and entities"), "Edit"},
         [this] {
           if (E()->copySelection()) {
             E()->m_pasteCount = 0;
             showToast(tr("Copied"));
           }
         },
         hasSelection);
  am.add(Spec{"edit.cut", tr("Cu&t"), "", "Ctrl+X", {}, "Edit"}, [] { E()->cutSelection(); }, hasSelection);
  am.add(Spec{"edit.paste", tr("&Paste"), "pastedown", "Ctrl+V", tr("Paste, offset by one grid step"), "Edit"},
         [] { E()->paste(false); }, [] { return E() && E()->canPaste(); });
  am.add(Spec{"edit.pasteInPlace", tr("Paste in &Place"), "pastedown", "Ctrl+Shift+V", {}, "Edit"},
         [] { E()->paste(true); }, [] { return E() && E()->canPaste(); });
  am.add(Spec{"edit.pasteUnique", tr("Paste with &Unique Names"), "pastedown", "",
              tr("Paste, giving the pasted entities new names (door → door_1); connections inside the pasted set follow "
                 "the new names"),
              "Edit"},
         [] { E()->paste(false, true); }, [] { return E() && E()->canPaste(); });
  am.add(Spec{"edit.delete", tr("&Delete"), "trash", "Del", tr("Delete the selection"), "Edit"},
         [] { E()->deleteSelected(); }, hasSelection);
  am.add(Spec{"edit.duplicate", tr("D&uplicate"), "duplicate", "Ctrl+D", tr("Duplicate the selection"),
              "Edit"},
         [] { E()->duplicateSelection(); }, hasDuplicable);
  am.add(Spec{"edit.selectAll", tr("Select &All"), "select_extend", "Ctrl+A", {}, "Edit"},
         [this] {
           if (ctx_->editorMode == g::EditorMode::EDIT && E()->editMode() && !E()->selectedObjects().empty())
             E()->selectAllVertices(); // inside the vertex tool: every corner
           else
             E()->selectAll();
         },
         hasScene);
  am.add(Spec{"edit.selectNone", tr("Select &None"), "select_set", "Ctrl+Shift+A", {}, "Edit"},
         [] { E()->clearSelection(); }, hasSelection);
  am.add(Spec{"edit.invert", tr("&Invert Selection"), "select_difference", "Ctrl+I", {}, "Edit"},
         [] { E()->invertSelection(); }, hasScene);
  am.add(Spec{"edit.selectMaterial", tr("Select Same &Material"), "material", "Shift+M",
              tr("Select everything that uses a material of the selection"), "Edit"},
         [] { E()->selectByMaterial(); }, hasSelection);
  am.add(Spec{"face.selectByGroup", tr("Select Adjacent by Face &Group"), "face_maps", "",
              tr("Material mode: grow the selection over the faces sharing the picked face's group"), "Edit"},
         [] { E()->selectByFaceGroup(); },
         [] { return E() && E()->materialMode() && !E()->selectedFaces().empty(); });
  am.add(Spec{"edit.rename", tr("Rena&me…"), "", "F2", {}, "Edit"}, [this] {
    if (m_dockOutliner) {
      m_dockOutliner->show();
      m_dockOutliner->raise();
    }
  }, hasBrushSelection);
  am.add(Spec{"group.new", tr("New Mesh &Group…"), "outliner_collection", "Ctrl+G",
              tr("Create a mesh group (the selection moves into it); new brushes keep going to the active group"),
              "Groups"},
         [this] {
           bool ok = false;
           const QString name = QInputDialog::getText(this, tr("New Mesh Group"), tr("Name"), QLineEdit::Normal,
                                                      E()->uniqueGroupName(QStringLiteral("group")), &ok);
           if (!ok || name.trimmed().isEmpty())
             return;
           // Grouping does not change the active group (where new brushes go).
           E()->createMeshGroup(name, true, false);
           showToast(tr("Group \"%1\" created").arg(name.trimmed()));
         },
         hasScene);
  am.add(Spec{"group.toWorldspawn", tr("Move Selection to &worldspawn"), "world", "Ctrl+Shift+W",
              tr("Put the selected brushes back into worldspawn: out of their group and brush entity"), "Groups"},
         [this] {
           if (E()->selectionInPhysicsSimulation()) {
             showToast(tr("Reset the physics simulation first: the selection is part of it"));
             return;
           }
           E()->moveSelectionToWorldspawn();
         },
         hasBrushSelection);
  am.add(Spec{"group.toActive", tr("Move Selection to Active Group"), "outliner_collection", "",
              tr("Put the selected brushes into the active mesh group"), "Groups"},
         [] { E()->moveSelectionToGroup(E()->activeMeshGroup()); }, hasBrushSelection);
  am.add(Spec{"edit.lock", tr("&Lock Selection"), "locked", "Ctrl+L", tr("Locked objects cannot be selected or moved"),
              "Edit"},
         [] { E()->lockSelected(); }, hasBrushSelection);
  am.add(Spec{"edit.unlock", tr("Unlock All"), "unlocked", "Ctrl+Shift+L", {}, "Edit"}, [] { E()->unlockAll(); },
         hasScene);
  static const char *axisNames[3] = {"X", "Y", "Z"};
  struct Nudge {
    const char *id;
    const char *key;
    int axis, dir;
  };
  for (const Nudge &n: {Nudge{"edit.nudgeXm", "Left", 0, -1}, Nudge{"edit.nudgeXp", "Right", 0, 1},
                        Nudge{"edit.nudgeZm", "Up", 2, -1}, Nudge{"edit.nudgeZp", "Down", 2, 1},
                        Nudge{"edit.nudgeYp", "PgUp", 1, 1}, Nudge{"edit.nudgeYm", "PgDown", 1, -1}}) {
    Spec s{n.id, tr("Nudge %1%2").arg(n.dir < 0 ? QStringLiteral("−") : QStringLiteral("+"),
                                      QLatin1String(axisNames[n.axis])),
           "", n.key, tr("Move the selection one grid step"), "Edit"};
    s.viewportOnly = true;
    const int axis = n.axis, dir = n.dir;
    am.add(s, [axis, dir] { E()->nudgeSelection(axis, dir); }, hasSelection);
  }

  // Add (entities)
  // Placed on the surface under the cursor when opened with Shift+A over the 3D view, else in front of the camera.
  struct AddSpec {
    const char *id, *cls, *icon, *tip;
    QString text;
  };
  const AddSpec adds[] = {
      {"add.pointLight", "light", "light_point", "Omnidirectional light with shadows", tr("Omni Light")},
      {"add.spotLight", "light_spot", "light_spot", "Cone light with shadows", tr("Spot Light")},
      {"add.playerStart", "info_player_start", "armature_data", "Where the player spawns", tr("Player Start")},
      {"add.camera", "camera_3d", "outliner_ob_camera", "A Godot Camera3D; select it and tick Preview to look through it",
       tr("Camera3D")},
      {"add.logicAuto", "logic_auto", "play", "Fires its outputs when the map starts", tr("Logic Auto")},
      {"add.logicRelay", "logic_relay", "decorate_linked", "Fans one input out to many outputs", tr("Logic Relay")},
      {"add.logicTimer", "logic_timer", "time", "Fires at a fixed or random interval", tr("Logic Timer")},
      {"add.logicBranch", "logic_branch", "split_vertical", "Fires OnTrue / OnFalse", tr("Logic Branch")},
      {"add.mathCounter", "math_counter", "sequence", "A number with limits", tr("Math Counter")},
      {"add.logicEvent", "logic_event", "export", "Bridge to Godot gameplay code", tr("Godot Event")},
      {"add.pathAnchor", "path_anchor", "con_followpath", "One end of a connection (face it into the gap)",
       tr("Path Anchor")},
  };
  for (const AddSpec &a: adds) {
    am.add(Spec{QString::fromLatin1(a.id), a.text, QString::fromLatin1(a.icon), {}, tr(a.tip), "Add"},
           [this, cls = QString::fromLatin1(a.cls)] { addEntityFromMenu(cls); }, hasScene);
  }
  am.add(Spec{"add.menu", tr("Add…"), "add", "Shift+A", tr("Add a brush, light or entity at the cursor"), "Add"},
         [this] { showAddMenuAtCursor(); });

  am.add(Spec{"entity.makeBrush", tr("Make &Brush Entity…"), "mod_wireframe", "Ctrl+T",
              tr("Tie the selected brushes to a brush entity (func_detail, func_door, trigger…); they stay editable"),
              "Entities"},
         [this] {
           if (E()->selectionInPhysicsSimulation()) {
             showToast(tr("Reset the physics simulation first: the selection is part of it"));
             return;
           }
           // The class of a brush entity already in the selection comes first.
           QDialog dlg(this);
           dlg.setWindowTitle(tr("Make Brush Entity"));
           auto *v = new QVBoxLayout(&dlg);
           auto *combo = new QComboBox(&dlg);
           ui::scale::bind(combo, [combo] { combo->setIconSize(ui::scale::icon(ui::scale::Icon::Small)); });
           QString current;
           for (const quint32 id: E()->selectedObjects())
             if (const auto o = E()->scene()->findById(id); o && o->brushEntity())
               if (const eng::Entity *e = E()->entityById(o->brushEntity()))
                 current = e->classname;
           for (const auto &c: eng::EntityRegistry::instance().classes())
             if (c.isBrushEntity()) {
               combo->addItem(ActionManager::icon(c.icon), c.name, c.name);
               combo->setItemData(combo->count() - 1, c.description, Qt::ToolTipRole);
             }
           combo->setCurrentIndex(std::max(0, combo->findData(current.isEmpty() ? QStringLiteral("func_detail") : current)));
           v->addWidget(combo);
           auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
           connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
           connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
           v->addWidget(buttons);
           if (dlg.exec() != QDialog::Accepted)
             return;
           const QString cls = combo->currentData().toString();
           if (E()->makeBrushEntity(cls))
             showToast(tr("%1 brush(es) tied to %2").arg(E()->selectedObjects().size()).arg(cls));
         },
         hasBrushSelection);
  am.add(Spec{"entity.toWorld", tr("Move Brushes to &World"), "world", "",
              tr("Untie the selected brushes from their brush entity"), "Entities"},
         [this] {
           if (E()->selectionInPhysicsSimulation()) {
             showToast(tr("Reset the physics simulation first: the selection is part of it"));
             return;
           }
           showToast(E()->moveSelectionToWorld() ? tr("Moved to world") : tr("No brush entity in the selection"));
         },
         [] { return E() && E()->selectionHasBrushEntity(); });

  am.add(Spec{"io.selectTargets", tr("Select I/O &Targets"), "restrict_select_off", "",
              tr("Select the entities the selection's outputs target"), "Entities"},
         [this] {
           const int n = E()->selectIOTargets();
           showToast(n ? tr("%1 target(s)").arg(n) : tr("No targets"));
         },
         hasIOSelection);
  am.add(Spec{"io.selectSources", tr("Select I/O &Sources"), "import", "",
              tr("Select the entities whose outputs target the selection"), "Entities"},
         [this] {
           const int n = E()->selectIOSources();
           showToast(n ? tr("%1 source(s)").arg(n) : tr("Nothing targets the selection"));
         },
         hasIOSelection);
  am.add(Spec{"io.selectSameName", tr("Select Same &Name"), "select_extend", "",
              tr("Select every entity sharing the selection's names"), "Entities"},
         [this] {
           const int n = E()->selectSameName();
           showToast(n ? tr("%1 entit(ies)").arg(n) : tr("The selection has no names"));
         },
         hasIOSelection);
  am.add(Spec{"io.editor", tr("&I/O Editor…"), "decorate_linked", "Ctrl+Alt+I",
              tr("Wire entity inputs and outputs in a flow graph, see every connection, and test the logic"),
              "Entities"},
         [this] { ui::IOEditor::open(E(), this, E()->ioFocusEntity()); }, hasScene);
  am.add(Spec{"io.fireInput", tr("&Fire Input…"), "play", "",
              tr("Send an input to the selected entity in the I/O simulator (like ent_fire)"), "Entities"},
         [this] { ui::IOEditor::open(E(), this, E()->ioFocusEntity(), -1, {}, true); }, hasScene);
  am.add(Spec{"io.simToggle", tr("Run I/O &Simulation"), "play", "",
              tr("Run the map's logic in the editor: logic_auto fires, timers tick, pulses travel along the "
                 "connection lines. Nothing in the map is changed."),
              "Entities", true},
         [] {
           if (E()->ioSimRunning())
             E()->ioSimStop();
           else
             E()->ioSimStart();
         },
         hasScene, [] { return E() && E()->ioSimRunning(); });
  am.add(Spec{"io.simPause", tr("Pause I/O Simulation"), "pause", "", {}, "Entities", true},
         [] { E()->setIOSimPaused(!E()->ioSimPaused()); }, [] { return E() && E()->ioSimRunning(); },
         [] { return E() && E()->ioSimPaused(); });
  am.add(Spec{"io.simStep", tr("Step I/O Simulation"), "frame_next", "", tr("Deliver the next queued event"),
              "Entities"},
         [] { E()->ioSimStep(1); }, [] { return E() && E()->ioSimRunning(); });

  // Physics simulation (vk_physics_sim.cpp)
  {
    using PS = vk::Engine3D::PhysicsSimState;
    // Resumes / pauses a simulation of this scope, else starts one.
    const auto play = [this](const bool selected) {
      if (E()->physicsSimState() != PS::None && E()->physicsSimSelectedOnly() == selected) {
        if (E()->physicsSimState() == PS::Running)
          E()->physicsPause();
        else
          E()->physicsPlay(selected);
        return;
      }
      E()->physicsReset();
      E()->physicsPlay(selected);
    };
    const auto canPlay = [](const bool selected) {
      if (!E() || g::editorMode != g::EditorMode::OBJECT || !prefs::b("Debug/Physics"))
        return false;
      // A simulation started with the other button keeps this one off until reset.
      if (E()->physicsSimState() != PS::None)
        return E()->physicsSimSelectedOnly() == selected;
      return selected ? E()->selectionHasRigidBody() : E()->levelHasRigidBody();
    };
    const auto playing = [](const bool selected) {
      return E() && E()->physicsSimState() == PS::Running && E()->physicsSimSelectedOnly() == selected;
    };
    am.add(Spec{"physics.playAll", tr("Simulate &All Rigid Bodies"), "Play", "",
                tr("Play the physics simulation: every rigid body falls"), "Physics", true},
           [play] { play(false); }, [canPlay] { return canPlay(false); }, [playing] { return playing(false); });
    am.add(Spec{"physics.playSelected", tr("Simulate &Selected Rigid Bodies"), "PlaySelected", "",
                tr("Play the physics simulation: only the selected rigid bodies move"), "Physics", true},
           [play] { play(true); }, [canPlay] { return canPlay(true); }, [playing] { return playing(true); });
    // Checked while paused; pressed again it resumes.
    am.add(Spec{"physics.pause", tr("&Pause Simulation"), "Pause", "",
                tr("Pause the physics simulation: the bodies stay where they are"), "Physics", true},
           [] {
             if (E()->physicsSimState() == PS::Running)
               E()->physicsPause();
             else
               E()->physicsPlay(E()->physicsSimSelectedOnly());
           },
           [] { return E() && E()->physicsSimState() != PS::None; },
           [] { return E() && E()->physicsSimState() == PS::Paused; });
    am.add(Spec{"physics.reset", tr("&Reset Simulation"), "Reload", "",
                tr("End the physics simulation: every body goes back to where it was"), "Physics"},
           [] { E()->physicsReset(); }, [] { return E() && E()->physicsSimState() != PS::None; });
    am.add(Spec{"physics.apply", tr("Apply &Simulated Transform"), "checkmark", "",
                tr("Keep the selected bodies where the simulation put them (Reset leaves them there)"), "Physics"},
           [] { E()->physicsApply(); }, [] { return E() && E()->selectionPhysicsDirty(); });
  }

  am.add(Spec{"view.toggleLayout", tr("Toggle Quad / Single View"), "mesh_grid", "F",
              tr("Switch between the quad view (3D + Top / Front / Side) and the 3D view alone"), "View"},
         [this] { setViewportLayout(viewportLayout() == 0 ? 1 : 0); });
  am.add(Spec{"view.focus", tr("&Frame Selection"), "zoom_selected", "Shift+F",
              tr("Frame the selection in the 3D view"), "View"},
         [] { E()->focusSelection(); }, hasScene);
  am.add(Spec{"view.hide", tr("&Hide Selected"), "hide_on", "H", {}, "View"}, [] { E()->hideSelected(); },
         hasBrushSelection);
  am.add(Spec{"view.isolate", tr("&Isolate Selected"), "restrict_view_on", "Shift+H",
              tr("Hide everything except the selection"), "View"},
         [] { E()->isolateSelected(); }, hasBrushSelection);
  am.add(Spec{"view.showAll", tr("&Show All"), "hide_off", "Alt+H", tr("Show all hidden brushes"), "View"}, [] { E()->showAll(); },
         [] { return E() && E()->hiddenCount() > 0; });
  {
    Spec s{"view.layoutSingle", tr("Single View"), "view3d", "Ctrl+Alt+1", tr("Single 3D view"), "View", true};
    am.add(s, [this] { setViewportLayout(0); });
    s = Spec{"view.layoutQuad", tr("Quad View"), "mesh_grid", "Ctrl+Alt+2", tr("3D view with top, front and side views"), "View", true};
    am.add(s, [this] { setViewportLayout(1); });
    s = Spec{"view.layoutSplit", tr("2 + 1 View"), "view_ortho", "Ctrl+Alt+3", tr("3D view and one 2D view"), "View", true};
    am.add(s, [this] { setViewportLayout(2); });
  }
  am.add(Spec{"view.lit", tr("&Lit Shading"), "shading_rendered", "F4", tr("Lit shading (off: fullbright)"),
              "View", true},
         [] { E()->setRenderMode(E()->renderMode() == 0 ? 1 : 0); }, {}, [] { return E() && E()->renderMode() != 0; });
  am.add(Spec{"view.specular", tr("&Specular Reflections"), "shading_texture", {},
              tr("Show or hide specular highlights and reflections in the 3D view"), "View", true},
         [] { E()->setSpecularEnabled(!E()->specularEnabled()); }, {}, [] { return E() && E()->specularEnabled(); });
  am.add(Spec{"view.normals", tr("&Normal Maps"), "normals_face", {}, tr("Show or hide normal maps in the 3D view"),
              "View", true},
         [] { E()->setNormalMapsEnabled(!E()->normalMapsEnabled()); }, {}, [] { return E() && E()->normalMapsEnabled(); });
  am.add(Spec{"view.lut", tr("Colour &LUT"), "color", {}, tr("Show or hide the level's colour LUT in the 3D view"),
              "View", true},
         [] { E()->setColorLutEnabled(!E()->colorLutEnabled()); }, {}, [] { return E() && E()->colorLutEnabled(); });
  {
    am.add(Spec{"view.zen", tr("&Zen Mode"), "shading_bbox", "Alt+Z",
                tr("Zen mode: the 2D views show only what is around the 3D camera"), "View", true},
           [] { E()->setZenMode(!E()->zenMode()); }, {}, [] { return E() && E()->zenMode(); });
    // Same switch as the Modifiers panel's checkbox.
    QAction *gen = am.add(Spec{"view.showGenerated", tr("Show &Generated Brushes"), "modifier_on", "G",
                               tr("Show generated brushes (off: show the modifier sources)"),
                               "View", true},
                          [] { E()->setShowModOutput(!E()->showModOutput()); }, {},
                          [] { return E() && E()->showModOutput(); });
    QIcon icon;
    icon.addPixmap(ActionManager::icon(QStringLiteral("modifier_on")).pixmap(64, 64), QIcon::Normal, QIcon::On);
    icon.addPixmap(ActionManager::icon(QStringLiteral("modifier_off")).pixmap(64, 64), QIcon::Normal, QIcon::Off);
    gen->setIcon(icon);
  }
  am.add(Spec{"view.ao", tr("&Ambient Occlusion"), "shading_solid", {},
              tr("Show or hide ambient occlusion (SSAO) in the 3D view (also Preferences › Rendering)"), "View", true},
         [] { prefs::get().setValue(QStringLiteral("Render/Ssao"), !prefs::b("Render/Ssao")); }, {},
         [] { return prefs::b("Render/Ssao"); });
  am.add(Spec{"view.collision", tr("Show &Clip Brushes"), "mod_physics", {},
              tr("Show or hide clip brushes (func_clip, e.g. the stairs' ramp) in the 3D view"), "View", true},
         [] { E()->setShowCollision(!E()->showCollision()); }, {}, [] { return E() && E()->showCollision(); });
  am.add(Spec{"view.triggers", tr("Show &Trigger Brushes"), "physics", {},
              tr("Show or hide trigger_* brushes in the 3D view"), "View", true},
         [] { E()->setShowTriggers(!E()->showTriggers()); }, {}, [] { return E() && E()->showTriggers(); });
  am.add(Spec{"view.ioLines", tr("Show &I/O Connections"), "decorate_linked", {},
              tr("Lines from entities to the entities their outputs target (3D and 2D views)"), "View", true},
         [] { E()->setShowIOLines(!E()->showIOLines()); }, {}, [] { return E() && E()->showIOLines(); });
  am.add(Spec{"view.ioLinesAll", tr("Show &All I/O Connections"), "decorate_linked", {},
              tr("On: every connection, the selection's highlighted. Off: only the selection's connections"),
              "View", true},
         [] { E()->setIOLinesAll(!E()->ioLinesAll()); }, [] { return E() && E()->showIOLines(); },
         [] { return E() && E()->ioLinesAll(); });
  am.add(Spec{"view.freelook", tr("Toggle Free Look"), "view_camera_unselected", "Z",
              tr("Mouse-look without holding the right button"), "View"},
         [this] {
           const bool on = E()->toggleFreeLook();
           if (on) {
             grabMouse();
             setCursor(Qt::BlankCursor);
           } else {
             releaseMouse();
             unsetCursor();
           }
         });
  am.add(Spec{"view.palette", tr("Command &Palette…"), "viewzoom", "Ctrl+Shift+P", tr("Search all commands"), "View"},
         [this] { ui::CommandPalette::open(this); });
  for (int b = 0; b < 4; ++b) {
    am.add(Spec{QStringLiteral("view.bookmarkStore%1").arg(b + 1), tr("Store Camera %1").arg(b + 1), "bookmarks",
                QStringLiteral("Ctrl+F%1").arg(b + 5), tr("Remember the 3D camera position"), "View"},
           [this, b] {
             E()->storeBookmark(b);
             showToast(tr("Camera %1 stored").arg(b + 1));
           });
    am.add(Spec{QStringLiteral("view.bookmark%1").arg(b + 1), tr("Go to Camera %1").arg(b + 1), "bookmarks",
                QStringLiteral("F%1").arg(b + 5), {}, "View"},
           [this, b] {
             if (!E()->recallBookmark(b))
               showToast(tr("Camera %1 not stored yet (Ctrl+F%2)").arg(b + 1).arg(b + 5));
           });
  }
  am.add(Spec{"view.resetLayout", tr("Reset Window Layout"), "", "", {}, "View"}, [this] { resetWindowLayout(); });
  // UI scale and text size both ±0.25, within their 1×-2× range.
  const auto scaleBoth = [](double delta) {
    for (const QString &key: {QStringLiteral("General/UiScale"), QStringLiteral("General/FontScale")}) {
      const prefs::Def *d = prefs::get().def(key);
      if (!d)
        continue;
      double lo = 1.0, hi = 2.0;
      if (!d->choiceValues.isEmpty()) {
        lo = d->choiceValues.front().toDouble();
        hi = d->choiceValues.back().toDouble();
      }
      prefs::get().setValue(key, std::clamp(prefs::get().doubleValue(key) + delta, lo, hi));
    }
  };
  am.add(Spec{"view.uiLarger", tr("Larger Interface"), "zoom_in", "Ctrl+=", tr("UI scale and text size +0.25"), "View"},
         [scaleBoth] { scaleBoth(+0.25); });
  am.add(Spec{"view.uiSmaller", tr("Smaller Interface"), "zoom_out", "Ctrl+-", tr("UI scale and text size -0.25"), "View"},
         [scaleBoth] { scaleBoth(-0.25); });

  {
    Spec s{"mode.object", tr("&Object Mode"), "object_datamode", "1", tr("Object mode: select and move whole brushes"),
           "Mode", true};
    am.add(s, [this] { ctx_->switchMode(g::EditorMode::OBJECT); }, {},
           [this] { return ctx_->editorMode == g::EditorMode::OBJECT; });
    s = Spec{"mode.edit", tr("&Edit Mode"), "editmode_hlt", "2", tr("Edit mode: move vertices, edges and faces"), "Mode", true};
    am.add(s, [this] { ctx_->switchMode(g::EditorMode::EDIT); }, {},
           [this] { return ctx_->editorMode == g::EditorMode::EDIT; });
    s = Spec{"mode.material", tr("&Material Mode"), "material", "3", tr("Material mode: select faces and edit their textures"),
             "Mode", true};
    am.add(s, [this] { ctx_->switchMode(g::EditorMode::MATERIAL); }, {},
           [this] { return ctx_->editorMode == g::EditorMode::MATERIAL; });
    s = Spec{"mode.toggle", tr("Toggle Object / Material Mode"), "", "", {}, "Mode"};
    s.viewportOnly = true;
    am.add(s, [this] {
      ctx_->switchMode(ctx_->editorMode == g::EditorMode::MATERIAL ? g::EditorMode::OBJECT : g::EditorMode::MATERIAL);
    });
  }

  // Pie menu (Shift+S over the viewports) and its one-step commands
  {
    Spec s{"view.pieMenu", tr("Pie Menu"), "", "Shift+S",
           tr("One-step commands for the current mode, picked by direction"), "View"};
    s.viewportOnly = true;
    am.add(s, [this] {
      if (m_pie)
        m_pie->popup();
    });

    // Auto-select kinds that need no further choice, run with the Auto-select panel's defaults (contiguous, this
    // side, replacing).
    const QString kAutoSelectIcon = QStringLiteral("select_extend");
    const auto mode = [this] { return ctx_->editorMode; };
    const auto seeded = [mode] {
      if (!E() || mode() == g::EditorMode::EDIT)
        return false;
      const bool seed = mode() == g::EditorMode::MATERIAL ? E()->selectedFaceCount() > 0
                                                          : !E()->selectedObjects().empty();
      return seed || E()->hasMarquee() || E()->autoSelectActive();
    };
    const auto inObject = [mode, seeded] { return mode() == g::EditorMode::OBJECT && seeded(); };
    const auto inMaterial = [mode, seeded] { return mode() == g::EditorMode::MATERIAL && seeded(); };
    const auto run = [](const int intent, const quint32 group = 0) {
      E()->runAutoSelect(intent, 0, false, group, true, false);
    };
    am.add(Spec{"autosel.ceiling", tr("Auto-select Ceilings"), kAutoSelectIcon, "",
                tr("Ceilings grown from the picked faces or brushes"), "Auto-select"},
           [run] {
             run(E()->materialMode() ? int(eng::sel::Intent::Ceiling) : int(eng::sel::BrushIntent::Ceiling));
           },
           seeded);
    am.add(Spec{"autosel.sameSize", tr("Auto-select Same Size"), kAutoSelectIcon, "",
                tr("Brushes with the picked brush's dimensions (turned by 90° counts)"), "Auto-select"},
           [run] { run(int(eng::sel::BrushIntent::SameSize)); }, inObject);
    am.add(Spec{"autosel.sameMaterial", tr("Auto-select Same Material"), kAutoSelectIcon, "",
                tr("Object mode: brushes whose main material is the picked brush's. Material mode: every face "
                   "in the map with a material of the picked faces"),
                "Auto-select"},
           [run] {
             if (E()->materialMode())
               E()->selectSameMaterialFaces(false);
             else
               run(int(eng::sel::BrushIntent::SameMaterial));
           },
           [inObject, mode] {
             return inObject()
                 || (E() && mode() == g::EditorMode::MATERIAL
                     && (E()->selectedFaceCount() > 0 || E()->autoSelectActive()));
           });
    am.add(Spec{"autosel.touching", tr("Auto-select Touching"), kAutoSelectIcon, "",
                tr("Every brush connected to the picked one through contact"), "Auto-select"},
           [run] { run(int(eng::sel::BrushIntent::Touching)); }, inObject);
    am.add(Spec{"autosel.group", tr("Auto-select Group"), kAutoSelectIcon, "",
                tr("Faces of the picked face's group (its brush group first, else its face group)"), "Auto-select"},
           [run] { run(int(eng::sel::Intent::Group), E()->defaultSeedGroup()); },
           [inMaterial] { return inMaterial() && E()->defaultSeedGroup() != 0; });
    am.add(Spec{"autosel.sameBrush", tr("Auto-select Same Brush"), kAutoSelectIcon, "",
                tr("Every face of the brushes the picked faces are on"), "Auto-select"},
           [] { E()->selectSameBrushFaces(false); },
           [mode] {
             return E() && mode() == g::EditorMode::MATERIAL
                 && (E()->selectedFaceCount() > 0 || E()->autoSelectActive());
           });
    am.add(Spec{"autosel.sameMirror", tr("Auto-select Same Mirror"), kAutoSelectIcon, "",
                tr("Everything in the mirror collections the selection is in, both sides"), "Auto-select"},
           [] { E()->selectSameMirror(false); },
           [mode] {
             return E() && mode() != g::EditorMode::EDIT
                 && (!E()->selectedMirrorSets().empty() || E()->autoSelectActive());
           });

    // Each replaces whatever the other mode had selected.
    am.add(Spec{"select.toObjectMode", tr("Select Brushes in Object Mode"), "object_datamode", "",
                tr("Switch to Object mode with the brushes of the selected faces selected"), "Select"},
           [this] {
             const std::set<quint32> brushes = E()->selectedBrushIds();
             ctx_->switchMode(g::EditorMode::OBJECT);
             E()->selectBrushUnits(brushes);
           },
           [mode] { return E() && mode() == g::EditorMode::MATERIAL && E()->selectedFaceCount() > 0; });
    am.add(Spec{"select.toMaterialMode", tr("Select Faces in Material Mode"), "facesel", "",
                tr("Switch to Material mode with every face of the selected brushes selected"), "Select"},
           [this] {
             const std::vector<quint32> faces = E()->facePicksOfBrushes(E()->selectedObjects());
             ctx_->switchMode(g::EditorMode::MATERIAL);
             E()->clearAutoSelect();
             E()->setSelected({}, faces);
           },
           [mode] { return E() && mode() != g::EditorMode::MATERIAL && !E()->selectedObjects().empty(); });
    am.add(Spec{"tex.quickMaterial", tr("Quick Material"), "shading_texture", "",
                tr("Materials near the selection and recent ones: a click applies one"), "Texture"},
           [this] {
             if (m_pie)
               m_pie->openQuickMaterial();
           },
           [mode] {
             return E()
                 && ((mode() == g::EditorMode::MATERIAL && E()->selectedFaceCount() > 0)
                     || (mode() == g::EditorMode::OBJECT && !E()->selectedObjects().empty()));
           });
  }

  // Vertex tool (Edit mode)
  {
    const auto editing = [this] { return ctx_->editorMode == g::EditorMode::EDIT && hasBrushSelection(); };
    const auto haveVerts = [this] {
      return ctx_->editorMode == g::EditorMode::EDIT && E() && !E()->selectedVertices().empty();
    };
    struct Sub {
      const char *id, *icon;
      QString text;
      int mode;
    };
    for (const Sub &sm: {Sub{"vertex.subAll", "vertexsel", tr("Vertices + Edges"), 0},
                         Sub{"vertex.subVerts", "vertexsel", tr("Vertices Only"), 1},
                         Sub{"vertex.subEdges", "edgesel", tr("Edges Only"), 2},
                         Sub{"vertex.subFaces", "facesel", tr("Faces"), 3}}) {
      Spec s{sm.id, sm.text, sm.icon, "", tr("Vertex tool handles: %1").arg(sm.text), "Vertex", true};
      const int mode = sm.mode;
      am.add(s, [this, mode] {
        if (ctx_->editorMode != g::EditorMode::EDIT)
          ctx_->switchMode(g::EditorMode::EDIT);
        E()->setVertexSubMode(mode);
      }, {}, [mode] { return E() && E()->vertexSubMode() == mode; });
    }
    am.add(Spec{"vertex.cycle", tr("Cycle Vertex Handles"), "vertexsel", "Shift+V",
                tr("Vertices + edges → vertices → edges → faces"), "Vertex"},
           [this] {
             if (ctx_->editorMode != g::EditorMode::EDIT) {
               ctx_->switchMode(g::EditorMode::EDIT);
               return;
             }
             E()->cycleVertexSubMode();
           },
           hasBrushSelection);
    am.add(Spec{"vertex.selectAll", tr("Select All Vertices"), "select_extend", "", {}, "Vertex"},
           [] { E()->selectAllVertices(); }, editing);
    am.add(Spec{"vertex.clear", tr("Deselect Vertices"), "select_set", "", {}, "Vertex"},
           [] { E()->clearVertexSelection(); }, haveVerts);
    am.add(Spec{"vertex.scale", tr("Scale Vertices…"), "con_sizelike", "Alt+E",
                tr("Scale the selected vertices about a pivot (drag the pivot in a 2D view; Enter / Esc close)"),
                "Vertex"},
           [this] {
             if (!E()->beginVertexScale())
               showToast(tr("Select two or more vertices to scale"));
           },
           haveVerts);
    am.add(Spec{"vertex.weld", tr("Weld Vertices"), "automerge_on", "Ctrl+W",
                tr("Merge the selected vertices of each brush into their centre"), "Vertex"},
           [this] { showToast(E()->weldSelectedVertices() ? tr("Welded") : tr("Select two or more vertices of a brush")); },
           haveVerts);
    am.add(Spec{"vertex.splitEdge", tr("Split Edges"), "mod_subsurf", "",
                tr("Add a corner in the middle of each selected edge, pulled out one grid step "
                   "(Ctrl+drag an edge handle in a 2D view to pull one out by hand)"),
                "Vertex"},
           [this] {
             if (!E()->splitSelectedEdges(0.0))
               showToast(tr("Select both corners of an edge first"));
           },
           haveVerts);
    am.add(Spec{"vertex.snap", tr("Snap Vertices"), "snap_grid", "", tr("Move the selected vertices onto the grid"),
                "Vertex"},
           [this] { showToast(E()->snapSelectedVertices() ? tr("Snapped") : tr("Already on the grid")); }, haveVerts);
    connect(E(), &vk::Engine3D::vertexToolChanged, this, [] { ActionManager::instance().refresh(); });
  }

  {
    using M = vk::Engine3D::GizmoMode;
    struct G {
      const char *id, *icon, *key;
      QString text, tip;
      M mode;
    };
    for (const G &g: {G{"tool.move", "orientation_global", "W", tr("&Move"), tr("Move tool"), M::Translate},
                      G{"tool.rotate", "con_rotlike", "E", tr("&Rotate"),
                        tr("Rotate tool"), M::Rotate},
                      G{"tool.scale", "con_sizelike", "R", tr("&Scale"), tr("Scale tool"),
                        M::Scale}}) {
      Spec s{g.id, g.text, g.icon, g.key, g.tip, "Tools", true};
      const M mode = g.mode;
      am.add(s, [mode] { E()->setGizmoMode(mode); }, {}, [mode] { return E() && E()->gizmoMode() == mode; });
    }
    connect(E(), &vk::Engine3D::gizmoModeChanged, this, [] { ActionManager::instance().refresh(); });
  }

  // While a stairs / arch panel is open, [ and ] change its step / segment count.
  am.add(Spec{"grid.smaller", tr("Smaller Grid"), "", "[", {}, "Grid"}, [] { E()->stepGridLevel(-1); });
  am.add(Spec{"grid.bigger", tr("Bigger Grid"), "", "]", {}, "Grid"}, [] { E()->stepGridLevel(1); });

  am.add(Spec{"brush.create", tr("&Create Brush from Box"), "mesh_cube", "",
              tr("Create a brush from the selection box (shape: arrow menu)"), "Brush"},
         [this] {
           if (E()->shapeToolActive()) {
             if (E()->commitShapeTool())
               showToast(tr("Created"));
             return;
           }
           // Stairs and arch are tools of their own: an old saved choice of either makes a box.
           int kind = prefs::i("Brush/Shape");
           if (kind == 6 || kind == 7) {
             kind = 0;
             prefs::get().setValue(QStringLiteral("Brush/Shape"), 0);
           }
           if (E()->createPrimitiveFromMarquee(kind))
             showToast(tr("Brush created"));
         },
         [] { return E() && E()->hasMarquee(); });
  // Bevel mode (vk_bevel.cpp).
  am.add(Spec{"brush.bevel", tr("&Bevel"), "edge_bevel", "Ctrl+B",
              tr("Bevel mode: round or chamfer edges of the selected brushes"), "Brush", true},
         [] {
           if (E()->bevelMode())
             E()->leaveBevelMode();
           else
             E()->enterBevelMode();
         },
         [] {
           return E() && E()->scene() && !E()->materialMode()
                  && (E()->bevelMode() || !E()->selectedObjects().empty() || !E()->selectedVertices().empty());
         },
         [] { return E() && E()->bevelMode(); });
  am.add(Spec{"brush.room", tr("Create &Room"), "mesh_plane", "",
              tr("Room from the selection box: the box is the inside, walls go around it"), "Brush"},
         [this] {
           if (E()->createRoomFromMarquee() > 0)
             showToast(tr("Room created"));
         },
         [] { return E() && E()->hasMarquee(); });
  am.add(Spec{"brush.stairs", tr("&Stairs"), "mod_array", "Ctrl+Shift+B",
              tr("Stairs in the 2D selection box, or in place of the selected brush. Tweak them in the Inspector"),
              "Brush"},
         [this] { beginShapeTool(6); },
         [] { return E() && (E()->hasMarquee() || (!E()->selectedObjects().empty() && !E()->selectedProcObject())); });
  am.add(Spec{"proc.dissolve", tr("Convert to Plain Brushes"), "mesh_cube", "",
              tr("Turn the selected generated object (stairs) into ordinary brushes"), "Brush"},
         [] { E()->dissolveProc(E()->selectedProcObject()); },
         [] { return E() && E()->selectedProcObject() != 0; });
  am.add(Spec{"tool.layoutDesigner", tr("&Layout Designer…"), "mesh_grid", "Ctrl+Alt+L",
              tr("Draw corridors, rooms and vents on a top-down grid and generate them as brushes"), "Brush"},
         [this] {
           int layout = 0;
           if (!E()->selectedObjects().empty())
             layout = E()->layoutOfObject(*E()->selectedObjects().begin());
           ui::LayoutDesigner::open(E(), ctx_, this, layout);
         });
  am.add(Spec{"layout.edit", tr("Edit Layout of Selection…"), "mesh_grid", "",
              tr("Open the Layout Designer on the layout the selected brush was generated from"), "Brush"},
         [this] { ui::LayoutDesigner::open(E(), ctx_, this, E()->layoutOfObject(*E()->selectedObjects().begin())); },
         [] { return E() && !E()->selectedObjects().empty() && E()->layoutOfObject(*E()->selectedObjects().begin()) > 0; });
  am.add(Spec{"brush.arch", tr("&Arch…"), "mod_bevel", "",
              tr("Arch, archway or arched opening from the 2D selection box (panel in the 2D view, Enter applies)"),
              "Brush"},
         [this] { beginShapeTool(7); }, [] { return E() && E()->hasMarquee(); });
  am.add(Spec{"brush.cylinder", tr("C&ylinder…"), "mesh_cylinder", "",
              tr("Cylinder or tube from the 2D selection box (options in the Inspector, Enter applies)"), "Brush"},
         [this] { beginShapeTool(vk::Engine3D::kShapeCylinder); }, [] { return E() && E()->hasMarquee(); });
  am.add(Spec{"brush.capsule", tr("Ca&psule…"), "mesh_capsule", "",
              tr("Capsule from the 2D selection box (options in the Inspector, Enter applies)"), "Brush"},
         [this] { beginShapeTool(vk::Engine3D::kShapeCapsule); }, [] { return E() && E()->hasMarquee(); });
  am.add(Spec{"brush.pyramid", tr("P&yramid…"), "mesh_cone", "",
              tr("Pyramid or cone from the 2D selection box (options in the Inspector, Enter applies)"), "Brush"},
         [this] { beginShapeTool(vk::Engine3D::kShapePyramid); }, [] { return E() && E()->hasMarquee(); });
  struct Shape {
    const char *id, *icon;
    QString name;
    int kind;
  };
  const QList<Shape> shapes = {{"brush.shapeBox", "mesh_cube", tr("Box"), 0},
                               {"brush.shapeWedge", "mesh_plane", tr("Wedge"), 1},
                               {"brush.shapeCylinder", "mesh_cylinder", tr("Cylinder"), 2},
                               {"brush.shapeCone", "mesh_cone", tr("Cone"), 3},
                               {"brush.shapePyramid", "mesh_cone", tr("Pyramid"), 8},
                               {"brush.shapeSphere", "mesh_uvsphere", tr("Sphere"), 5},
                               {"brush.shapeStairs", "mod_array", tr("Stairs"), 6},
                               {"brush.shapeArch", "mod_bevel", tr("Arch"), 7}};
  for (const Shape &sh: shapes) {
    Spec s{sh.id, sh.name, sh.icon, "", tr("Brush shape: %1").arg(sh.name), "Brush", true};
    const int kind = sh.kind;
    const QString name = sh.name;
    am.add(s, [this, kind, name] {
      if (kind != 6 && kind != 7)
        prefs::get().setValue(QStringLiteral("Brush/Shape"), kind);
      if (kind == 6)
        beginShapeTool(kind);
      else if (kind == 7 && E()->hasMarquee())
        beginShapeTool(kind);
      else if (E()->hasMarquee() && E()->createPrimitiveFromMarquee(kind))
        showToast(tr("%1 created").arg(name));
    }, {}, [kind] { return prefs::i("Brush/Shape") == kind; });
  }
  am.add(Spec{"csg.subtract", tr("&Subtract"), "mod_boolean", "Ctrl+Shift+-",
              tr("Subtract: carve the selection out of the brushes it touches"), "CSG"},
         [this] {
           const int n = E()->csgSubtract();
           showToast(n > 0 ? tr("Subtract: %1 fragments").arg(n) : tr("Nothing to subtract from"));
         },
         hasBrushSelection);
  am.add(Spec{"csg.hollow", tr("&Hollow"), "mod_solidify", "Ctrl+Shift+H",
              tr("Hollow: turn the selection into a room with walls"), "CSG"},
         [this] {
           const int n = E()->csgHollow();
           showToast(n > 0 ? tr("Hollowed into %1 walls").arg(n) : tr("Too thin to hollow"));
         },
         hasBrushSelection);
  am.add(Spec{"csg.merge", tr("&Merge (Convex Hull)"), "mod_remesh", "Ctrl+Shift+M",
              tr("Merge: join the selection into one convex brush"), "CSG"},
         [this] {
           if (E()->csgMerge()) {
             showToast(tr("Merged"));
             return;
           }
           if (E()->lastMergeWouldFillGaps()) {
             if (QMessageBox::question(this, tr("Merge"),
                                       tr("The brushes do not form a convex shape together; merging fills the gaps "
                                          "between them with their convex hull.\n\nMerge anyway?"))
                 == QMessageBox::Yes)
               showToast(E()->csgMerge(true) ? tr("Merged (gaps filled)") : tr("Merge failed"));
             return;
           }
           showToast(tr("Select two or more brushes to merge"));
         },
         [] { return E() && E()->selectedObjects().size() >= 2; });
  am.add(Spec{"csg.intersect", tr("&Intersect"), "select_intersect", "",
              tr("Keep only the volume common to all selected brushes"), "CSG"},
         [this] { showToast(E()->csgIntersect() ? tr("Intersected") : tr("The brushes do not overlap")); },
         [] { return E() && E()->selectedObjects().size() >= 2; });
  static const char *axisN[3] = {"X", "Y", "Z"};
  for (int a = 0; a < 3; ++a) {
    const QString ax = QLatin1String(axisN[a]);
    am.add(Spec{QStringLiteral("xform.rot%1cw").arg(ax), tr("Rotate 90° around %1").arg(ax), "loop_forwards",
                a == 1 ? QStringLiteral("Ctrl+R") : QString(), {}, "Transform"},
           [a] { E()->rotateSelection(a, -90.0); }, hasBrushSelection);
    am.add(Spec{QStringLiteral("xform.rot%1ccw").arg(ax), tr("Rotate −90° around %1").arg(ax), "loop_back",
                a == 1 ? QStringLiteral("Ctrl+Shift+R") : QString(), {}, "Transform"},
           [a] { E()->rotateSelection(a, 90.0); }, hasBrushSelection);
    am.add(Spec{QStringLiteral("xform.mirror%1").arg(ax), tr("Mirror along %1").arg(ax), "mod_mirror", "", {},
                "Transform"},
           [a] { E()->mirrorSelection(a); }, hasBrushSelection);
  }
  am.add(Spec{"brush.extrudeOut", tr("E&xtrude Faces Out"), "normals_face", "Ctrl+Alt+E",
              tr("Move the selected faces (Material mode picks or vertex-tool faces) out by one grid step"), "Brush"},
         [this] {
           if (!E()->extrudeSelectedFaces(E()->gridStepWorld(), false))
             showToast(tr("Pick faces first (Material mode, or Faces in the vertex tool)"));
         },
         [] { return E() && !E()->selectedBrushFaces().empty(); });
  am.add(Spec{"brush.extrudeIn", tr("Extrude Faces In"), "normals_face", "Ctrl+Alt+Shift+E",
              tr("Move the selected faces in by one grid step"), "Brush"},
         [this] {
           if (!E()->extrudeSelectedFaces(-E()->gridStepWorld(), false))
             showToast(tr("Cannot push the face through the brush"));
         },
         [] { return E() && !E()->selectedBrushFaces().empty(); });
  am.add(Spec{"brush.extrudeNew", tr("Extrude Faces to New Brush"), "normals_face", "Ctrl+Alt+N",
              tr("Add a new brush on each selected face, one grid step thick"), "Brush"},
         [this] { E()->extrudeSelectedFaces(E()->gridStepWorld(), true); },
         [] { return E() && !E()->selectedBrushFaces().empty(); });
  am.add(Spec{"xform.shear", tr("S&hear…"), "mod_simpledeform", "",
              tr("Slant the selected brushes: one axis moves in proportion to another"), "Transform"},
         [this] {
           const QStringList modes = {tr("X along height (Y)"), tr("Z along height (Y)"), tr("X along Z"),
                                      tr("Z along X"), tr("Height (Y) along X"), tr("Height (Y) along Z")};
           static const int ax[6][2] = {{0, 1}, {2, 1}, {0, 2}, {2, 0}, {1, 0}, {1, 2}};
           bool ok = false;
           const QString pick = QInputDialog::getItem(this, tr("Shear"), tr("Direction"), modes, 0, false, &ok);
           if (!ok)
             return;
           const double f = QInputDialog::getDouble(this, tr("Shear"), tr("Offset per unit (1 = 45°)"), 0.5, -10.0, 10.0,
                                                    3, &ok);
           if (!ok)
             return;
           const int i = int(modes.indexOf(pick));
           showToast(E()->shearSelection(ax[i][0], ax[i][1], f) ? tr("Sheared") : tr("Nothing to shear"));
         },
         hasBrushSelection);
  am.add(Spec{"xform.snapToGrid", tr("Snap to Grid"), "snap_grid", "",
              tr("Move the selection onto the grid: brushes by their corners, entities to the middle of their cell"),
              "Transform"},
         [this] { showToast(E()->snapSelectionToGridCell() ? tr("Snapped to grid") : tr("Already on the grid")); },
         [] {
           return E() && !E()->materialMode()
                  && (!E()->selectedObjects().empty() || !E()->selectedEntities().empty() || !E()->selectedMods().empty());
         });
  am.add(Spec{"xform.snap", tr("Snap Vertices to Grid"), "snap_grid", "", tr("Move every vertex onto the grid"),
              "Transform"},
         [this] { showToast(E()->snapSelectionToGrid() ? tr("Snapped to grid") : tr("Nothing changed")); },
         hasBrushSelection);
  am.add(Spec{"brush.cut", tr("C&ut Tool"), "cursor_knife", "",
              tr("Cut tool: slice brushes or cut openings (Left Shift+X)"), "Brush"},
         [this] {
           if (E()->cutMode() == vk::Engine3D::CutMode::Off)
             E()->enterCutMode();
           else
             E()->cycleCutMode();
         },
         hasBrushSelection);
  am.add(Spec{"brush.mirrorMode", tr("M&irror Mode"), "mod_mirror", "Ctrl+M",
              tr("New brushes are also made mirrored across the mirror line, and stay linked"), "Brush", true},
         [] { E()->setMirrorActive(!E()->mirrorActive()); }, {},
         [] { return E() && E()->mirrorActive(); });
  am.add(Spec{"brush.mirrorSelection", tr("Mirror &Selection"), "mod_mirror", "",
              tr("Mirror the selected brushes across the mirror line (linked; an identical copy already there is linked instead)"),
              "Brush"},
         [this] { showToast(E()->mirrorSelectionAcrossLine()); },
         [] { return E() && !E()->selectedBrushIds().empty(); });
  am.add(Spec{"brush.addToMirror", tr("&Add to Mirror"), "mod_mirror", "",
              tr("The selected brushes that are in no mirror join the mirror of the others (each gets its twin)"),
              "Brush"},
         [this] { showToast(E()->addSelectionToMirrorSet()); }, [] { return E() && E()->mirrorSetToAddTo() != 0; });
  am.add(Spec{"brush.mirrorSelectTwin", tr("Select Mirror &Twin"), "mod_mirror", "",
              tr("Select the linked mirror copies of the selection"), "Brush"},
         [] { E()->selectMirrorTwins(); }, [] { return E() && E()->selectedMirrorLinked() > 0; });
  am.add(Spec{"brush.mirrorDetach", tr("&Detach Mirror"), "unlinked", "",
              tr("Unlink the selection from its mirror copies"), "Brush"},
         [] { E()->detachMirror(); }, [] { return E() && E()->selectedMirrorLinked() > 0; });
  auto objectMode = [] { return E() && !E()->materialMode(); };
  am.add(Spec{"mod.addArray", tr("Add &Array Modifier"), "mod_array", "",
              tr("Copies of the selection (or the modified object) along an offset, a length or a path"), "Modifiers"},
         [] { E()->addModifier(QStringLiteral("array")); },
         [] { return E() && !E()->materialMode() && (!E()->selectedObjects().empty() || E()->panelModHost()); });
  am.add(Spec{"mod.addCurve", tr("Add &Curve Modifier"), "mod_curve", "",
              tr("The selection (or the modified object) follows a path"), "Modifiers"},
         [] { E()->addModifier(QStringLiteral("curve")); },
         [] { return E() && !E()->materialMode() && (!E()->selectedObjects().empty() || E()->panelModHost()); });
  am.add(Spec{"mod.editSource", tr("&Edit Source"), "editmode_hlt", "Shift+Tab",
              tr("Show and edit the original brushes of the modified object; the result keeps following"), "Modifiers",
              true, true},
         [] { E()->toggleEditSource(); }, [] { return E() && (E()->panelModHost() || E()->editSourceHost()); },
         [] { return E() && E()->editSourceHost() != 0; });
  am.add(Spec{"mod.applyAll", tr("A&pply Modifiers"), "checkmark", "",
              tr("Turn the modified object's result into ordinary brushes"), "Modifiers"},
         [] {
           if (const quint32 h = E()->panelModHost())
             E()->applyModifiers(h, false);
         },
         [] { return E() && E()->panelModHost(); });
  am.add(Spec{"mod.dissolve", tr("&Remove Modifiers"), "x", "",
              tr("Drop the modifiers: the original brushes come back"), "Modifiers"},
         [] {
           if (const quint32 h = E()->panelModHost())
             E()->dissolveModHost(h);
         },
         [] { return E() && E()->panelModHost(); });
  am.add(Spec{"path.pen", tr("Draw &Path"), "curve_bezcurve", "Shift+P",
              tr("Draw a path in a 2D view: click adds a point, drag pulls its handles, Enter or a double click finishes"),
              "Path", true},
         [] { E()->setPenActive(!E()->penActive()); }, objectMode, [] { return E() && E()->penActive(); });
  am.add(Spec{"path.connect", tr("&Connect Anchors"), "con_followpath", "",
              tr("A path between the two selected Path Anchors (it bends to join them)"), "Path"},
         [] { E()->connectSelectedAnchors(); }, [] { return E() && E()->selectedEntities().size() == 2; });
  am.add(Spec{"path.brushesBetween", tr("&Brushes Between Anchors"), "con_followpath", "",
              tr("The selected brushes fill the gap between the two selected Path Anchors (Array + Curve)"), "Path"},
         [] { E()->brushesBetweenAnchors(); },
         [] {
           return E() && E()->selectedEntities().size() == 2
                  && (!E()->selectedObjects().empty() || E()->selectedMods().size() == 1);
         });
  am.add(Spec{"path.pointType", tr("Cycle Point &Type"), "handle_auto", "",
              tr("Auto, aligned, free or vector handles for the selected path points"), "Path"},
         [] { E()->cyclePathPointType(); }, [] { return E() && !E()->selectedPathPoints().empty(); });
  am.add(Spec{"brush.textureLock", tr("&Texture Lock"), "linked", "Shift+T",
              tr("Texture lock: textures move with brushes"), "Brush", true},
         [] { E()->setTextureLock(!E()->textureLock()); }, {}, [] { return E() && E()->textureLock(); });
  am.add(Spec{"brush.scaleLock", tr("Texture &Scale Lock"), "con_sizelike", "Shift+R",
              tr("Texture scale lock: textures scale with brushes"), "Brush", true},
         [] { E()->setTextureScaleLock(!E()->textureScaleLock()); }, {},
         [] { return E() && E()->textureScaleLock(); });

  am.add(Spec{"tex.apply", tr("&Apply Current Material"), "brush_data", "Ctrl+Shift+T",
              tr("Apply the current material to the selection"), "Texture"},
         [] { E()->applyMaterialToSelection(E()->currentMaterial()); },
         [] { return hasFaceTargets() && !E()->currentMaterial().isEmpty(); });
  am.add(Spec{"tex.pick", tr("&Pick Material"), "eyedropper", "Alt+T", tr("Pick the selection's material as the current material"),
              "Texture"},
         [] { E()->setCurrentMaterial(E()->pickMaterialFromSelection()); }, hasFaceTargets);
  am.add(Spec{"tex.rotCW", tr("Rotate Texture +90°"), "loop_forwards", "", {}, "Texture"},
         [] { E()->rotateFaceTextures(90); }, hasFaceTargets);
  am.add(Spec{"tex.rotCCW", tr("Rotate Texture −90°"), "loop_back", "", {}, "Texture"},
         [] { E()->rotateFaceTextures(-90); }, hasFaceTargets);
  am.add(Spec{"tex.flipU", tr("Flip Texture Horizontally"), "arrow_leftright", "", {}, "Texture"},
         [] { E()->flipFaceTextures(true); }, hasFaceTargets);
  am.add(Spec{"tex.flipV", tr("Flip Texture Vertically"), "mod_mirror", "", {}, "Texture"},
         [] { E()->flipFaceTextures(false); }, hasFaceTargets);
  am.add(Spec{"tex.alignWorld", tr("Align Texture to World"), "world", "", tr("Align textures to the world grid"), "Texture"},
         [] { E()->alignFaceTextures(0); }, hasFaceTargets);
  am.add(Spec{"tex.alignFace", tr("Align Texture to Face"), "facesel", "", {}, "Texture"},
         [] { E()->alignFaceTextures(1); }, hasFaceTargets);
  am.add(Spec{"tex.fit", tr("Fit Texture to Face"), "fullscreen_enter", "", tr("Fit the texture to the face"), "Texture"},
         [] { E()->fitFaceTextures(1, 1); }, hasFaceTargets);

  {
    struct J {
      const char *id, *icon;
      QString text;
      int mode;
    };
    for (const J &j: {J{"tex.justifyL", "align_left", tr("Justify Texture Left"), 0},
                      J{"tex.justifyR", "align_right", tr("Justify Texture Right"), 1},
                      J{"tex.justifyT", "align_top", tr("Justify Texture Top"), 2},
                      J{"tex.justifyB", "align_bottom", tr("Justify Texture Bottom"), 3},
                      J{"tex.justifyC", "align_center", tr("Centre Texture"), 4}}) {
      const int mode = j.mode;
      am.add(Spec{j.id, j.text, j.icon, "", tr("Snap the texture to the face's edge"), "Texture"},
             [mode] { E()->justifyFaceTextures(mode); }, hasFaceTargets);
    }
  }
  am.add(Spec{"tex.alignEdge", tr("Align Texture to Edge"), "snap_edge", "",
              tr("Run the texture along a face edge; repeat to step through the edges"), "Texture"},
         [] { E()->alignFaceTexturesToEdge(); }, hasFaceTargets);
  am.add(Spec{"tex.reset", tr("Reset Texture"), "loop_back", "",
              tr("World-aligned axes, default scale, no offset"), "Texture"},
         [] { E()->resetFaceTextures(); }, hasFaceTargets);
  am.add(Spec{"tex.copyAttr", tr("Copy Face Attributes"), "copydown", "Ctrl+Alt+C",
              tr("Remember the first selected face's material and mapping"), "Texture"},
         [this] { showToast(E()->copyFaceAttributes() ? tr("Face attributes copied") : tr("Select a face first")); },
         hasFaceTargets);
  am.add(Spec{"tex.pasteAttr", tr("Paste Face Attributes"), "pastedown", "Ctrl+Alt+V",
              tr("Material and mapping onto the selected faces (projected). In Material mode Ctrl+click a face "
                 "pastes onto it"),
              "Texture"},
         [] { E()->pasteFaceAttributes(1); }, [] { return hasFaceTargets() && E()->hasFaceClipboard(); });
  am.add(Spec{"tex.wrap", tr("Wrap Face Attributes"), "mod_curve", "Ctrl+Alt+Shift+V",
              tr("Paste rotated around the shared edge so the texture runs on seamlessly (Ctrl+Shift+click a face)"),
              "Texture"},
         [] { E()->pasteFaceAttributes(2); }, [] { return hasFaceTargets() && E()->hasFaceClipboard(); });
  am.add(Spec{"tex.pasteMaterial", tr("Paste Material Only"), "material", "",
              tr("Only the copied material, keeping each face's mapping"), "Texture"},
         [] { E()->pasteFaceAttributes(0); }, [] { return hasFaceTargets() && E()->hasFaceClipboard(); });
  {
    struct Sh {
      const char *id;
      QString text;
      double du, dv;
    };
    for (const Sh &sh: {Sh{"tex.shiftUm", tr("Shift Texture Left"), -1, 0}, Sh{"tex.shiftUp", tr("Shift Texture Right"), 1, 0},
                        Sh{"tex.shiftVm", tr("Shift Texture Up"), 0, -1}, Sh{"tex.shiftVp", tr("Shift Texture Down"), 0, 1}}) {
      const double du = sh.du, dv = sh.dv;
      am.add(Spec{sh.id, sh.text, du < 0 ? "back" : du > 0 ? "forward" : dv < 0 ? "sort_asc" : "sort_desc", "",
                  tr("Move the texture one grid step (arrow keys in Material mode)"), "Texture"},
             [du, dv] {
               const double step = E()->gridStepWorld() * vk::Engine3D::kSourceUnitsPerWorldUnit;
               E()->shiftFaceTexturesSource(du * step, dv * step);
             },
             hasFaceTargets);
    }
  }
  // Ctrl+Space as Unreal's content browser.
  am.add(Spec{"tex.browser", tr("Texture &Browser…"), "viewzoom", "Ctrl+Space",
              tr("Browse the asset server's textures by pack and tag, see every map and apply them"), "Texture"},
         [this] { ui::TextureBrowserWindow::open(E(), this); });
  // Asset server: local or remote (Server/Url). Switching reconnects and every asset view reloads.
  {
    const auto overridden = [] { return !qEnvironmentVariableIsEmpty("EDITOR_SERVER_URL"); };
    const auto remoteMode = [] { return prefs::s("Server/Mode") == QLatin1String("remote"); };
    am.add(Spec{"server.local", tr("Use &Local Asset Server"), "home", "",
                tr("Run an asset server on this computer"), "Server", true},
           [] { prefs::get().setValue(QStringLiteral("Server/Mode"), QStringLiteral("local")); },
           [overridden] { return !overridden(); }, [remoteMode] { return !remoteMode(); });
    am.add(Spec{"server.remote", tr("Use &Remote Asset Server"), "world", "",
                tr("Connect to an asset server on another computer"), "Server", true},
           [this] {
             prefs::get().setValue(QStringLiteral("Server/Mode"), QStringLiteral("remote"));
             if (prefs::s("Server/Url").trimmed().isEmpty())
               prefs::PreferencesDialog::showDialog(this, QStringLiteral("Asset Server"));
           },
           [overridden] { return !overridden(); }, remoteMode);
    am.add(Spec{"server.reconnect", tr("Re&connect to Asset Server"), "file_refresh", "",
                tr("Connect to the asset server again now"), "Server"},
           [] { net::AssetClient::instance().reconnectNow(); },
           [] { return !net::AssetClient::instance().httpBase().isEmpty(); });
    am.add(Spec{"server.restartLocal", tr("Restart Local Asset &Server"), "recover_last", "",
                tr("Stop and start the local asset server"), "Server"},
           [] { net::LocalServer::instance().restart(); },
           [overridden, remoteMode] { return !overridden() && !remoteMode() && net::LocalServer::instance().allowed(); });
    am.add(Spec{"server.installPacks", tr("&Install Asset Packs…"), "package", "",
                tr("Install texture, skybox and LUT packs on the asset server"), "Server"},
           [this] { ui::PackInstallWindow::open(this); });
    am.add(Spec{"server.settings", tr("Asset Server &Settings…"), "preferences", "",
                tr("Local or remote asset server, address and credentials"), "Server"},
           [this] { prefs::PreferencesDialog::showDialog(this, QStringLiteral("Asset Server")); });
  }
  am.add(Spec{"tex.packs", tr("Texture &Packs…"), "file_folder", "",
              tr("Manage texture packs: copies, renames, tags, editing maps, the map's project pack"), "Texture"},
         [this] { ui::TexturePacksWindow::open(E(), this); });
  am.add(Spec{"tex.properties", tr("Texture P&roperties"), "material", "Ctrl+Alt+M",
              tr("Show the Godot material properties of the selected faces' texture (Inspector), or of the "
                 "current texture in the Texture Browser"),
              "Texture"},
         [] { emit ui::TextureHub::instance().showPropertiesRequested({}); });
  am.add(Spec{"tex.replace", tr("&Replace Texture…"), "file_refresh", "",
              tr("Pick another texture for every face using this one (or only the selection), with a live preview"),
              "Texture"},
         [this] { ui::ReplaceTextureWindow::open(E(), this); },
         [] { return E() && E()->materialMode() && !E()->selectedFaces().empty(); });
  am.add(Spec{"tex.copyToPack", tr("&Copy Material into Pack…"), "duplicate", "",
              tr("Make an editable copy of the selected faces' texture in a user pack"), "Texture"},
         [this] {
           const QStringList refs = E()->selectionMaterialRefs();
           const QString ref = refs.isEmpty() ? E()->currentMaterial() : refs.first();
           if (!ref.isEmpty())
             emit ui::TextureHub::instance().copyRequested(ref);
         });
  am.add(Spec{"view.selectionTint", tr("Toggle Selection Tint"), "restrict_select_off", "V",
              tr("Hide / show the selection (the red tint on faces, the wireframe on brushes), to judge the view as it "
                 "will look"),
              "View", true},
         [] {
           E()->setMaskHidden(!E()->maskHidden());
           E()->requestRedraw();
         },
         {}, [] { return E() && E()->maskHidden(); });
  am.add(Spec{"view.uvEditor", tr("&UV Editor"), "uv", "Ctrl+Alt+U", tr("Show the UV editor panel"), "View", true},
         [this] {
           if (!m_dockUv)
             return;
           m_dockUv->setVisible(!m_dockUv->isVisible());
           if (m_dockUv->isVisible())
             m_dockUv->raise();
         },
         {}, [this] { return m_dockUv && m_dockUv->isVisible(); });

  am.add(Spec{"help.shortcuts", tr("&Keyboard Shortcuts…"), "help", "F1", {}, "Help"},
         [this] { prefs::PreferencesDialog::showDialog(this, QStringLiteral("Keyboard/Edit")); });
  am.add(Spec{"help.about", tr("&About"), "info", "", {}, "Help"}, [this] { ui::AboutDialog::showDialog(this); });

  for (auto sig: {&vk::Engine3D::selectionChanged, &vk::Engine3D::undoRedoChanged, &vk::Engine3D::sceneChanged,
                  &vk::Engine3D::marqueeChanged, &vk::Engine3D::textureLockChanged,
                  &vk::Engine3D::textureScaleLockChanged, &vk::Engine3D::renderModeChanged,
                  &vk::Engine3D::objectStateChanged, &vk::Engine3D::cutModeChanged,
                  &vk::Engine3D::meshGroupsChanged, &vk::Engine3D::shapeToolChanged, &vk::Engine3D::mirrorChanged,
                  &vk::Engine3D::modHostsChanged, &vk::Engine3D::editSourceChanged, &vk::Engine3D::penChanged,
                  &vk::Engine3D::pathsChanged, &vk::Engine3D::zenModeChanged,
                  &vk::Engine3D::physicsSimChanged, &vk::Engine3D::bevelChanged})
    connect(E(), sig, this, [] { ActionManager::instance().refresh(); });
  connect(E(), &vk::Engine3D::currentMaterialChanged, this, [] { ActionManager::instance().refresh(); });
  connect(E(), &vk::Engine3D::uvTransformChanged, this, [] { ActionManager::instance().refresh(); });
  connect(ctx_, &Ctx::editorModeChanged, this, [] { ActionManager::instance().refresh(); });
  am.refresh();
}

void MainWindow::beginShapeTool(const int kind) {
  if (kind == 6) {
    if (!E()->stairsFromMarqueeOrSelection())
      showToast(tr("Stairs need a selection box or a brush"));
    return;
  }
  if (!E()->hasMarquee()) {
    showToast(kind == vk::Engine3D::kShapeCylinder  ? tr("Cylinder needs a selection box")
              : kind == vk::Engine3D::kShapeCapsule ? tr("Capsule needs a selection box")
              : kind == vk::Engine3D::kShapePyramid ? tr("Pyramid needs a selection box")
                                                    : tr("Arch needs a selection box"));
    return;
  }
  if (!E()->beginShapeTool(kind))
    return;
  if ((kind == 7 || vk::Engine3D::isShapeKind(kind)) && m_dockInspector) {
    m_dockInspector->show();
    m_dockInspector->raise();
  }
}

QMenu *MainWindow::buildMenu(const QString &title, const QStringList &ids, QWidget *parent) {
  auto *menu = new QMenu(title, parent);
  for (const QString &id: ids) {
    if (id == QLatin1String("-")) {
      menu->addSeparator();
      continue;
    }
    if (QAction *a = ui::act(id))
      menu->addAction(a);
  }
  connect(menu, &QMenu::aboutToShow, this, [] { ActionManager::instance().refresh(); });
  return menu;
}

void MainWindow::buildMenuBar() {
  QMenuBar *mb = menuBar();
  mb->clear();

  QMenu *file = new QMenu(tr("&File"), mb);
  connect(file, &QMenu::aboutToShow, this, [] { ActionManager::instance().refresh(); });
  QMenu *create = file->addMenu(ActionManager::icon(QStringLiteral("file_new")), tr("&New"));
  create->addAction(ui::act(QStringLiteral("file.newProject")));
  create->addAction(ui::act(QStringLiteral("file.new")));
  create->addAction(ui::act(QStringLiteral("file.newPlayground")));
  file->addAction(ui::act(QStringLiteral("file.open")));
  m_recentMenu = file->addMenu(ActionManager::icon(QStringLiteral("recover_last")), tr("Open &Recent"));
  rebuildRecentProjectsMenu();
  file->addAction(ui::act(QStringLiteral("file.openLevel")));
  for (const char *id: {"-", "file.save", "file.saveAs", "-", "file.projectSettings", "file.levelSettings", "-",
                        "file.exportGodot", "-", "file.prefs", "server", "-", "file.quit"}) {
    if (QLatin1String(id) == QLatin1String("-")) {
      file->addSeparator();
    } else if (QLatin1String(id) == QLatin1String("server")) {
      file->addMenu(buildMenu(tr("Asset &Server"), {"server.local", "server.remote", "-", "server.reconnect",
                                                    "server.restartLocal", "-", "server.installPacks",
                                                    "server.settings"},
                              file));
    } else if (QLatin1String(id) == QLatin1String("file.exportGodot")) {
      // Recent projects, filled each time it opens.
      QMenu *godot = file->addMenu(ActionManager::icon(QStringLiteral("Godot")), tr("Export to &Godot"));
      connect(godot, &QMenu::aboutToShow, this, [this, godot] { populateGodotExportMenu(godot); });
      populateGodotExportMenu(godot);
    } else {
      file->addAction(ui::act(QString::fromLatin1(id)));
    }
  }
  mb->addMenu(file);

  mb->addMenu(buildMenu(tr("&Edit"), {"edit.undo", "edit.redo", "-", "edit.cut", "edit.copy", "edit.paste",
                                      "edit.pasteInPlace", "edit.pasteUnique", "-", "edit.duplicate", "edit.delete", "-",
                                      "edit.selectAll", "edit.selectNone", "edit.invert", "edit.selectMaterial", "face.selectByGroup", "-",
                                      "group.new", "group.toActive", "group.toWorldspawn", "-", "edit.lock", "edit.unlock", "-", "edit.rename"},
                        mb));

  QMenu *view = buildMenu(tr("&View"), {"view.focus", "-", "view.hide", "view.isolate", "view.showAll", "-",
                                        "view.toggleLayout", "view.layoutSingle", "view.layoutQuad", "view.layoutSplit",
                                        "-", "view.lit", "view.specular", "view.normals", "view.lut", "view.ao", "view.selectionTint", "view.collision", "view.triggers", "view.ioLines",
                                        "view.ioLinesAll", "view.freelook", "-"},
                          mb);
  {
    QMenu *docs = new QMenu(tr("Open &Documents"), view);
    view->insertMenu(view->actions().value(0), docs);
    connect(docs, &QMenu::aboutToShow, this, [this, docs] {
      docs->clear();
      auto *group = new QActionGroup(docs);
      for (int i = 0; i < m_docs->count(); ++i) {
        const proj::Document *d = m_docs->at(i);
        QAction *a = docs->addAction(d->fileName() + (m_docs->dirty(d) ? QStringLiteral(" *") : QString()), this,
                                     [this, i] { m_docs->activate(i); });
        a->setCheckable(true);
        a->setChecked(i == m_docs->activeIndex());
        a->setToolTip(QDir::toNativeSeparators(d->path));
        group->addAction(a);
      }
      if (m_docs->count() == 0)
        docs->addAction(tr("(nothing open)"))->setEnabled(false);
      docs->addSeparator();
      for (const char *id: {"doc.next", "doc.previous", "doc.close"})
        docs->addAction(ui::act(QString::fromLatin1(id)));
    });
    view->insertSeparator(view->actions().value(1));
  }
  QMenu *marks = view->addMenu(ActionManager::icon(QStringLiteral("bookmarks")), tr("Camera &Bookmarks"));
  for (int b = 1; b <= 4; ++b)
    marks->addAction(ui::act(QStringLiteral("view.bookmark%1").arg(b)));
  marks->addSeparator();
  for (int b = 1; b <= 4; ++b)
    marks->addAction(ui::act(QStringLiteral("view.bookmarkStore%1").arg(b)));
  QMenu *panels = view->addMenu(tr("&Panels"));
  for (QDockWidget *d: {m_dockProject, m_dockOutliner, m_dockEntities, m_dockModels, m_dockHistory, m_dockInspector,
                        m_dockMaterials, m_dockUv, m_dockReference, m_dockConsole, m_dockIssues})
    if (d)
      panels->addAction(d->toggleViewAction());
  view->addAction(ui::act(QStringLiteral("view.resetLayout")));
  view->addSeparator();
  view->addAction(ui::act(QStringLiteral("view.palette")));
  mb->addMenu(view);

  mb->addMenu(buildMenu(tr("&Mode"), {"mode.object", "mode.edit", "mode.material", "-", "mode.toggle", "-",
                                      "tool.move", "tool.rotate", "tool.scale"},
                        mb));
  {
    QMenu *brush = buildMenu(tr("&Brush"), {"brush.create"}, mb);
    QMenu *shape = brush->addMenu(ActionManager::icon(QStringLiteral("mesh_cube")), tr("Shape"));
    for (const char *id: {"brush.shapeBox", "brush.shapeWedge", "brush.shapeCylinder", "brush.shapeCone",
                          "brush.shapePyramid", "brush.shapeSphere", "brush.shapeStairs", "brush.shapeArch"})
      shape->addAction(ui::act(QString::fromLatin1(id)));
    brush->addAction(ui::act(QStringLiteral("brush.room")));
    brush->addAction(ui::act(QStringLiteral("brush.stairs")));
    brush->addAction(ui::act(QStringLiteral("brush.arch")));
    brush->addAction(ui::act(QStringLiteral("brush.cylinder")));
    brush->addAction(ui::act(QStringLiteral("brush.capsule")));
    brush->addAction(ui::act(QStringLiteral("brush.pyramid")));
    brush->addAction(ui::act(QStringLiteral("tool.layoutDesigner")));
    brush->addAction(ui::act(QStringLiteral("layout.edit")));
    brush->addAction(ui::act(QStringLiteral("brush.mirrorMode")));
    brush->addAction(ui::act(QStringLiteral("brush.mirrorSelection")));
    brush->addAction(ui::act(QStringLiteral("brush.addToMirror")));
    brush->addAction(ui::act(QStringLiteral("brush.mirrorSelectTwin")));
    brush->addAction(ui::act(QStringLiteral("brush.mirrorDetach")));
    brush->addSeparator();
    brush->addAction(ui::act(QStringLiteral("entity.makeBrush")));
    brush->addAction(ui::act(QStringLiteral("entity.toWorld")));
    brush->addSeparator();
    for (const char *id: {"csg.subtract", "csg.hollow", "csg.merge", "csg.intersect"})
      brush->addAction(ui::act(QString::fromLatin1(id)));
    brush->addSeparator();
    for (const char *id: {"brush.extrudeOut", "brush.extrudeIn", "brush.extrudeNew"})
      brush->addAction(ui::act(QString::fromLatin1(id)));
    brush->addSeparator();
    brush->addAction(ui::act(QStringLiteral("brush.cut")));
    brush->addAction(ui::act(QStringLiteral("brush.bevel")));
    QMenu *xf = brush->addMenu(ActionManager::icon(QStringLiteral("orientation_global")), tr("Transform"));
    for (const char *id: {"xform.rotXcw", "xform.rotXccw", "xform.rotYcw", "xform.rotYccw", "xform.rotZcw",
                          "xform.rotZccw", "-", "xform.mirrorX", "xform.mirrorY", "xform.mirrorZ", "-", "xform.shear",
                          "xform.snapToGrid", "xform.snap"}) {
      if (QLatin1String(id) == QLatin1String("-"))
        xf->addSeparator();
      else
        xf->addAction(ui::act(QString::fromLatin1(id)));
    }
    QMenu *mods = brush->addMenu(ActionManager::icon(QStringLiteral("modifier")), tr("Modifiers"));
    for (const char *id: {"mod.addArray", "mod.addCurve", "-", "mod.editSource", "-", "mod.applyAll", "mod.dissolve"}) {
      if (QLatin1String(id) == QLatin1String("-"))
        mods->addSeparator();
      else
        mods->addAction(ui::act(QString::fromLatin1(id)));
    }
    QMenu *paths = brush->addMenu(ActionManager::icon(QStringLiteral("curve_bezcurve")), tr("Path"));
    for (const char *id: {"path.pen", "path.pointType", "-", "add.pathAnchor", "path.connect", "path.brushesBetween"}) {
      if (QLatin1String(id) == QLatin1String("-"))
        paths->addSeparator();
      else
        paths->addAction(ui::act(QString::fromLatin1(id)));
    }
    brush->addSeparator();
    for (const char *id: {"brush.textureLock", "brush.scaleLock", "-", "grid.smaller", "grid.bigger"}) {
      if (QLatin1String(id) == QLatin1String("-"))
        brush->addSeparator();
      else
        brush->addAction(ui::act(QString::fromLatin1(id)));
    }
    mb->addMenu(brush);
  }
  mb->addMenu(buildMenu(tr("E&ntity"), {"io.editor", "-", "add.logicAuto", "add.logicRelay", "add.logicTimer", "add.logicBranch",
                                        "add.mathCounter", "add.logicEvent", "-", "entity.makeBrush", "entity.toWorld",
                                        "-", "io.selectTargets", "io.selectSources", "io.selectSameName", "-",
                                        "io.fireInput", "io.simToggle", "io.simPause", "io.simStep", "-",
                                        "physics.playAll", "physics.playSelected", "physics.pause", "physics.reset",
                                        "physics.apply", "-",
                                        "file.exportGodot"},
                        mb));
  mb->addMenu(buildMenu(tr("&Texture"), {"tex.browser", "tex.packs", "tex.properties", "tex.copyToPack", "-",
                                         "tex.apply", "tex.pick", "tex.replace", "-", "tex.copyAttr", "tex.pasteAttr", "tex.wrap",
                                         "tex.pasteMaterial", "-", "tex.rotCCW", "tex.rotCW", "tex.flipU",
                                         "tex.flipV", "-", "tex.alignWorld", "tex.alignFace", "tex.alignEdge", "tex.fit",
                                         "tex.reset", "-", "tex.justifyL", "tex.justifyR", "tex.justifyT",
                                         "tex.justifyB", "tex.justifyC", "-", "tex.shiftUm", "tex.shiftUp",
                                         "tex.shiftVm", "tex.shiftVp", "-", "view.uvEditor"},
                        mb));
  mb->addMenu(buildMenu(tr("&Help"), {"help.shortcuts", "view.palette", "-", "help.about"}, mb));
}

// The 3D view's menu bar is QML; its menus are these widgets, built from the same actions.
QMenu *MainWindow::viewportMenu(const QString &name) {
  if (auto it = m_viewportMenus.constFind(name); it != m_viewportMenus.constEnd())
    return *it;
  QMenu *menu = nullptr;
  if (name == QLatin1String("view")) {
    menu = buildMenu(QString(), {"view.focus", "-", "view.lit", "view.specular", "view.normals", "view.lut", "view.ao", "-",
                                 "view.collision", "view.triggers", "view.ioLines", "view.selectionTint", "-",
                                 "view.hide", "view.isolate", "view.showAll", "-"},
                     this);
    menu->addMenu(buildMenu(tr("Layout"), {"view.layoutSingle", "view.layoutQuad", "view.layoutSplit"}, menu));
    menu->addAction(ActionManager::icon(QStringLiteral("preferences")), tr("Rendering Preferences…"), this,
                    [this] { prefs::PreferencesDialog::showDialog(this, QStringLiteral("Rendering")); });
  } else if (name == QLatin1String("select")) {
    menu = buildMenu(QString(), {"edit.selectAll", "edit.selectNone", "edit.invert", "-", "edit.selectMaterial",
                                 "face.selectByGroup"},
                     this);
  } else if (name == QLatin1String("add")) {
    menu = buildMenu(QString(), {"brush.create", "-", "brush.shapeBox", "brush.shapeWedge", "brush.shapeCylinder",
                                 "brush.shapeCone", "brush.shapePyramid", "brush.shapeSphere", "brush.shapeStairs",
                                 "brush.shapeArch", "-", "add.pointLight", "add.spotLight", "add.playerStart",
                                 "add.camera", "-"},
                     this);
    menu->addMenu(buildMenu(tr("Logic"), {"add.logicAuto", "add.logicRelay", "add.logicTimer", "add.logicBranch",
                                          "add.mathCounter", "add.logicEvent"},
                            menu));
    menu->addMenu(buildMenu(tr("Path"), {"path.pen", "add.pathAnchor"}, menu));
    // Opened from the bar button: no cursor point, place in front of the camera.
    connect(menu, &QMenu::aboutToHide, this, [this] { QTimer::singleShot(0, this, [this] { m_addAt.reset(); }); });
  } else if (name == QLatin1String("object")) {
    menu = buildMenu(QString(), {"edit.duplicate", "edit.delete", "brush.cut", "-"}, this);
    menu->addMenu(buildMenu(tr("CSG"), {"csg.subtract", "csg.hollow", "csg.merge", "csg.intersect"}, menu));
    menu->addMenu(buildMenu(tr("Transform"), {"xform.rotYcw", "xform.rotYccw", "xform.mirrorX", "xform.mirrorZ", "-",
                                              "xform.snapToGrid", "xform.snap"},
                            menu));
    menu->addMenu(buildMenu(tr("Texture"), {"tex.apply", "tex.alignWorld", "tex.fit"}, menu));
  }
  if (menu)
    m_viewportMenus.insert(name, menu);
  else
    qDebug().noquote() << QStringLiteral("[viewport-menu] unknown menu '%1'").arg(name);
  return menu;
}

void MainWindow::showAddMenuAtCursor() {
  QMenu *menu = viewportMenu(QStringLiteral("add"));
  if (!menu)
    return;
  m_addAt = QCursor::pos();
  ActionManager::instance().refresh();
  menu->popup(*m_addAt);
}

void MainWindow::addEntityFromMenu(const QString &classname) {
  if (!g::engine3D)
    return;
  float px = -1.0f, py = -1.0f;
  if (m_addAt && quickContent_) {
    const QPointF p = quickContent_->toViewportPixels(QPointF(quickContent_->mapFromGlobal(*m_addAt)));
    px = float(p.x());
    py = float(p.y());
  }
  m_addAt.reset();
  const quint32 id = g::engine3D->placeEntityAt(classname, px, py);
  if (id == 0)
    return;
  g::engine3D->setSelected({}, {});
  g::engine3D->setSelectedEntities({id});
  if (const eng::EntityClass *c = eng::EntityRegistry::instance().find(classname))
    showToast(tr("Added %1").arg(c->label));
}

void MainWindow::showGridContextMenu(qreal x, qreal y) {
  if (!E() || E()->materialMode() || quickContent_ == nullptr)
    return;
  ActionManager::instance().refresh();
  auto *menu = new QMenu(this);
  menu->setAttribute(Qt::WA_DeleteOnClose);
  struct Item {
    const char *id;
    QString text;
  };
  const auto add = [](QMenu *m, const Item &it) {
    QAction *src = ui::act(QString::fromLatin1(it.id));
    if (!src)
      return;
    QAction *a = m->addAction(src->icon(), it.text, src, &QAction::trigger);
    a->setShortcut(src->shortcut()); // shown only; the menu is not in any widget's key path
    a->setEnabled(src->isEnabled());
    a->setToolTip(src->toolTip());
  };
  add(menu, {"xform.snapToGrid", tr("Snap to Closest Grid Cell")});
  menu->addSeparator();
  QMenu *brush = menu->addMenu(ActionManager::icon(QStringLiteral("mesh_cube")), tr("Brush"));
  brush->setEnabled(E()->hasMarquee());
  add(brush, {"brush.create", tr("Create Brush")});
  add(brush, {"brush.room", tr("Create Room")});
  for (const Item &it: {Item{"brush.stairs", tr("Create Stairs")}, Item{"brush.arch", tr("Create Arch…")},
                        Item{"brush.cylinder", tr("Create Cylinder…")}, Item{"brush.capsule", tr("Create Capsule…")},
                        Item{"brush.pyramid", tr("Create Pyramid…")}})
    add(menu, it);
  menu->popup(quickContent_->mapToGlobal(QPoint(int(x), int(y))));
}

void MainWindow::showViewportMenu(const QString &name, qreal x, qreal y) {
  m_addAt.reset();
  QMenu *menu = viewportMenu(name);
  if (menu == nullptr || quickContent_ == nullptr)
    return;
  ActionManager::instance().refresh();
  menu->popup(quickContent_->mapToGlobal(QPoint(int(x), int(y))));
}
