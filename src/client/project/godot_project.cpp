#include "godot_project.h"

#include <QDir>
#include <QFile>
#include <QSaveFile>

#include "client/engine/godot_export.h"
#include "client/engine/godot_text.h"

namespace proj {

namespace {

// Godot's default project icon (Godot logo by Andrea Calabró, CC BY 4.0).
constexpr const char *kIconSvg =
    R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="128" height="128"><rect width="124" height="124" x="2" y="2" fill="#363d52" stroke="#212532" stroke-width="4" rx="14"/><g fill="#fff" transform="translate(12.322 12.322)scale(.101)"><path d="M105 673v33q407 354 814 0v-33z"/><path fill="#478cbf" d="m105 673 152 14q12 1 15 14l4 67 132 10 8-61q2-11 15-15h162q13 4 15 15l8 61 132-10 4-67q3-13 15-14l152-14V427q30-39 56-81-35-59-83-108-43 20-82 47-40-37-88-64 7-51 8-102-59-28-123-42-26 43-46 89-49-7-98 0-20-46-46-89-64 14-123 42 1 51 8 102-48 27-88 64-39-27-82-47-48 49-83 108 26 42 56 81zm0 33v39c0 276 813 276 814 0v-39l-134 12-5 69q-2 10-14 13l-162 11q-12 0-16-11l-10-65H446l-10 65q-4 11-16 11l-162-11q-12-3-14-13l-5-69z"/><path d="M483 600c0 34 58 34 58 0v-86c0-34-58-34-58 0z"/><circle cx="725" cy="526" r="90"/><circle cx="299" cy="526" r="90"/></g><g fill="#414042" transform="translate(12.322 12.322)scale(.101)"><circle cx="307" cy="532" r="60"/><circle cx="717" cy="532" r="60"/></g></svg>
)svg";

constexpr const char *kEditorConfig = "root = true\n\n[*]\ncharset = utf-8\n";

// A Godot string literal: quotes and backslashes escaped.
QString godotString(const QString &s) {
  QString out = s;
  out.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
  out.replace(QLatin1Char('"'), QStringLiteral("\\\""));
  out.replace(QLatin1Char('\n'), QStringLiteral("\\n"));
  return QLatin1Char('"') + out + QLatin1Char('"');
}

bool writeFile(const QString &path, const QByteArray &bytes, QString *error) {
  QSaveFile f(path);
  if (f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size() && f.commit())
    return true;
  if (error)
    *error = QStringLiteral("Cannot write %1").arg(QDir::toNativeSeparators(path));
  return false;
}

} // namespace

QString newProjectGodotText(const QString &name, const NewGodotSettings &settings) {
  // What Godot 4.8's project manager writes for a new Forward+ project, section for section.
  const QString version = QStringLiteral("%1.%2").arg(eng::godot::kMinGodotMajor).arg(eng::godot::kMinGodotMinor);
  return QStringLiteral(
             "; Engine configuration file.\n"
             "; It's best edited using the editor UI and not directly,\n"
             "; since the parameters that go here are not all obvious.\n"
             ";\n"
             "; Format:\n"
             ";   [section] ; section goes between []\n"
             ";   param=value ; assign values to parameters\n"
             "\n"
             "config_version=5\n"
             "\n"
             "[application]\n"
             "\n"
             "config/name=%1\n"
             "config/features=PackedStringArray(\"%2\", \"Forward Plus\")\n"
             "%3"
             "config/icon=\"res://icon.svg\"\n"
             "\n"
             "[display]\n"
             "\n"
             "%4"
             "window/stretch/mode=\"canvas_items\"\n"
             "window/stretch/aspect=\"expand\"\n"
             "\n"
             "[gui]\n"
             "\n"
             "common/auto_focus_strategy=1\n"
             "\n"
             "[input_devices]\n"
             "\n"
             "joypads/ignore_joypad_on_unfocused_application=true\n"
             "\n"
             "[physics]\n"
             "\n"
             "3d/physics_engine=\"Jolt Physics\"\n"
             "\n"
             "[rendering]\n"
             "\n"
             "rendering_device/driver.windows=\"d3d12\"\n"
             "lights_and_shadows/multi_bounce_occlusion/enabled=true\n")
      .arg(godotString(name), version,
           settings.maxFps > 0 ? QStringLiteral("run/max_fps=%1\n").arg(settings.maxFps) : QString(),
           settings.viewportWidth > 0 && settings.viewportHeight > 0
               ? QStringLiteral("window/size/viewport_width=%1\nwindow/size/viewport_height=%2\n")
                     .arg(settings.viewportWidth)
                     .arg(settings.viewportHeight)
               : QString());
}

bool writeNewGodotProject(const QString &dir, const QString &name, QString *error, const NewGodotSettings &settings) {
  QDir d(dir);
  if (!d.exists() && !QDir().mkpath(dir)) {
    if (error)
      *error = QStringLiteral("Cannot create %1").arg(QDir::toNativeSeparators(dir));
    return false;
  }
  if (d.exists(QStringLiteral("project.godot"))) {
    if (error)
      *error = QStringLiteral("%1 already holds a Godot project").arg(QDir::toNativeSeparators(dir));
    return false;
  }
  if (!writeFile(d.filePath(QStringLiteral("project.godot")), newProjectGodotText(name, settings).toUtf8(), error))
    return false;
  if (!d.exists(QStringLiteral("icon.svg"))
      && !writeFile(d.filePath(QStringLiteral("icon.svg")), QByteArray(kIconSvg), error))
    return false;
  if (!d.exists(QStringLiteral(".editorconfig"))
      && !writeFile(d.filePath(QStringLiteral(".editorconfig")), QByteArray(kEditorConfig), error))
    return false;
  return true;
}

QString godotProjectName(const QString &dir) {
  QFile f(QDir(dir).filePath(QStringLiteral("project.godot")));
  if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
    return {};
  const eng::godot::GdTextFile p = eng::godot::GdTextFile::parse(QString::fromUtf8(f.readAll()));
  for (int s = 0; s < p.sections.size(); ++s)
    if (p.sections[s].tag == QLatin1String("application") && p.findProp(s, QStringLiteral("config/name")) >= 0)
      return p.prop(s, QStringLiteral("config/name")).toString();
  return {};
}

} // namespace proj
