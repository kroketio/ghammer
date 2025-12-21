#pragma once

#include <QDebug>
#include <QDir>
#include <QObject>

#include "client/lib/globals.h"
#include "shared/lib/config.h"

class Ctx;
extern Ctx* g_instance;

class Ctx final : public QObject {
Q_PROPERTY(g::EditorMode editorMode MEMBER editorMode NOTIFY editorModeChanged)
Q_PROPERTY(double scaleFactor MEMBER scaleFactor NOTIFY scaleFactorChanged)
  // UI scale × font scale (ui::scale::textFactor), like widget text.
  Q_PROPERTY(double textScaleFactor MEMBER textScaleFactor NOTIFY textScaleFactorChanged)
  // Bumped on shortcut changes; QML passes it to shortcutSuffix() so tooltips re-read.
  Q_PROPERTY(int shortcutRevision MEMBER shortcutRevision NOTIFY shortcutsChanged)
Q_OBJECT

public:
  explicit Ctx();

  g::EditorMode editorMode = g::EditorMode::OBJECT;
  // The mode Edit mode was entered from: Esc there goes back to it.
  g::EditorMode modeBeforeEdit = g::EditorMode::OBJECT;

  // General/UiScale mirrored for QML; setScaleFactor() changes the preference.
  double scaleFactor = 1.0;
  double textScaleFactor = 1.0;
  Q_INVOKABLE void setScaleFactor(double value);

  static Ctx* instance() {
    return g_instance;
  }

  // " (G)": an action's current shortcut, empty when none. `revision` only makes the binding follow changes.
  Q_INVOKABLE QString shortcutSuffix(const QString &actionId, int revision = 0) const;
  int shortcutRevision = 0;
  void noteShortcutsChanged() {
    ++shortcutRevision;
    emit shortcutsChanged();
  }

signals:
  void editorModeChanged(g::EditorMode mode);
  void shortcutsChanged();
  void scaleFactorChanged(double scaleFactor);
  void textScaleFactorChanged(double textScaleFactor);

public slots:
  void switchMode(g::EditorMode mode);
  // The engine is already in `mode` (another level became active): only the window follows.
  void syncMode(g::EditorMode mode);

private:
  static void createConfigDirectory(const QStringList &lst);
};
