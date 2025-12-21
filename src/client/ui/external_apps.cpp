#include "external_apps.h"

#include <QDir>
#include <QFileInfo>
#include <QMessageBox>
#include <QObject>
#include <QProcess>
#include <QStandardPaths>

#include "client/prefs/preferences.h"
#include "client/ui/file_dialog.h"

namespace ui::external {

QString detectImageEditor() {
  // Distributions name the binary after the release series too.
  for (const char *name: {"gimp", "gimp-3.0", "gimp-3", "gimp-2.10"})
    if (const QString path = QStandardPaths::findExecutable(QString::fromLatin1(name)); !path.isEmpty())
      return path;
  return {};
}

bool openImage(const QString &path, QWidget *parent) {
  const QString program = prefs::s("External/ImageEditor").trimmed();
  if (program.isEmpty()) {
    QMessageBox::warning(parent, QObject::tr("Open image"),
                         QObject::tr("No image editor is set, and GIMP was not found on PATH.\n"
                                     "Pick one in Preferences › External applications.\n\nThe file is at:\n%1")
                             .arg(path));
    return false;
  }
  QString argTemplate = prefs::s("External/ImageEditorArgs").trimmed();
  if (argTemplate.isEmpty())
    argTemplate = QStringLiteral("{file}");
  QStringList args = QProcess::splitCommand(argTemplate);
  for (auto &a: args)
    a.replace(QStringLiteral("{file}"), path);
  if (!argTemplate.contains(QLatin1String("{file}")))
    args.append(path);
  if (!QProcess::startDetached(program, args)) {
    QMessageBox::warning(parent, QObject::tr("Open image"),
                         QObject::tr("Could not start the image editor '%1'.\n"
                                     "Set it in Preferences › External applications.\n\nThe file is at:\n%2")
                             .arg(program, path));
    return false;
  }
  return true;
}

QString programPath(const QString &path) {
  if (path.trimmed().isEmpty())
    return {};
  const QFileInfo fi(path.trimmed());
  // macOS .app: the program in Contents/MacOS (the one named after the bundle when there are several).
  if (fi.isDir() && fi.suffix().compare(QLatin1String("app"), Qt::CaseInsensitive) == 0) {
    const QDir macos(fi.absoluteFilePath() + QStringLiteral("/Contents/MacOS"));
    const QFileInfoList files = macos.entryInfoList(QDir::Files | QDir::Executable);
    for (const QFileInfo &f: files)
      if (f.completeBaseName() == fi.completeBaseName())
        return f.absoluteFilePath();
    return files.isEmpty() ? QString() : files.front().absoluteFilePath();
  }
  if (fi.isFile() && fi.isExecutable())
    return fi.absoluteFilePath();
  return {};
}

void warnNotProgram(QWidget *parent, const QString &path) {
  QMessageBox::warning(parent, QObject::tr("Not a program"),
                       QObject::tr("%1 is not a program that can be started.").arg(QDir::toNativeSeparators(path)));
}

QString pickProgram(QWidget *parent, const QString &title, const QString &start) {
  QString from = start.isEmpty() ? QDir::homePath() : start;
  for (;;) {
    const QString picked = ui::FileDialog::getOpenFileName(parent, title, from);
    if (picked.isEmpty())
      return {};
    if (const QString exe = programPath(picked); !exe.isEmpty())
      return exe;
    warnNotProgram(parent, picked);
    from = picked;
  }
}

bool startGodot(const QString &dir, QWidget *parent) {
  // A project setting; asked for when unset or missing.
  QString exe = programPath(prefs::s("Godot/Executable"));
  if (exe.isEmpty()) {
    exe = pickProgram(parent, QObject::tr("Godot executable"));
    if (exe.isEmpty())
      return false;
    prefs::get().setValue(QStringLiteral("Godot/Executable"), exe);
  }
  if (!QProcess::startDetached(exe, {QStringLiteral("--editor"), QStringLiteral("--path"), dir})) {
    QMessageBox::warning(parent, QObject::tr("Start in Godot"),
                         QObject::tr("Could not start Godot (%1).\nPick it again in Project Settings › Godot.")
                             .arg(QDir::toNativeSeparators(exe)));
    return false;
  }
  return true;
}

} // namespace ui::external
