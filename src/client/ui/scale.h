#pragma once

// Live UI sizing from General/UiScale (everything) and General/FontScale (text, on top). 1.0 = normal
// density (13 px body text). On change: stylesheet re-expanded, ScaledStyle metrics re-read, layouts
// re-scaled, bind() callbacks run.
//
// Writing UI code:
//   - never hard-code a font size: give a widget a role (setRole) or, in paint
//     code, derive from the widget's font; QSS "font-size: 85%" is not
//     supported by Qt and is silently ignored
//   - write layout margins and spacing as normal-density literals; they are
//     scaled automatically
//   - any other pixel size: px(n) (or tpx(n) when it holds text), applied
//     inside bind() so it follows changes
//   - widget stylesheets with sizes: setScaledStyleSheet(), which expands the
//     same tokens as the app theme ($font_body..., Npx scaled)

#include <QFont>
#include <QObject>
#include <QSize>
#include <QString>

#include <functional>

class QApplication;
class QWidget;

namespace ui::scale {

// The type scale. Body is 13 px × textFactor(); the others are relative to it.
enum class Font {
  Small,      // 0.75: tiny uppercase labels (tool shelf section titles)
  Caption,    // 0.85: hints, badges, legends, tool shelf labels
  Body,       // 1.0: everything else
  Mono,       // 0.95, monospace family: console, code views
  Heading,    // 1.15 semibold: section headers, dock titles
  Title,      // 1.3 semibold: the inspector's object name, editor titles
  PageTitle,  // 1.6 semibold: dialog page titles
};

enum class Icon {
  Small,    // 16: trees, lists, menus, section headers
  Medium,   // 20: small tool buttons
  Toolbar,  // 28: the main toolbars
  Shelf,    // 24: the tool shelf
  Large,    // 32: entity icons next to a title
};

[[nodiscard]] double factor();      // General/UiScale
[[nodiscard]] double textFactor();  // UiScale × FontScale

// A normal-density pixel size at the current scale; 0 and ±1 are kept (hairlines).
[[nodiscard]] int px(double design);
// With the text factor, for sizes that hold text.
[[nodiscard]] int tpx(double design);

[[nodiscard]] int fontPx(Font role);
// `base` with the role's pixel size and weight (family and style kept).
[[nodiscard]] QFont font(Font role, QFont base = {});
[[nodiscard]] QSize icon(Icon role);

// Styles `w` as `role` through the app stylesheet (property uiRole).
void setRole(QWidget *w, Font role);

// Calls `fn` now and after every scale change, until `context` is destroyed.
void bind(QObject *context, const std::function<void()> &fn);

// Expands the theme tokens in a stylesheet: $font_small / $font_caption / $font_body /
// $font_mono / $font_heading / $font_title / $font_page become pixel sizes, and
// every other "Npx" is multiplied by factor() (0 and 1 px are kept).
[[nodiscard]] QString expandStyleSheet(const QString &qss);
// w->setStyleSheet(expandStyleSheet(qss)), redone after every scale change.
void setScaledStyleSheet(QWidget *w, const QString &qss);

// A first-show window size: `design` scaled, clamped to the screen.
[[nodiscard]] QSize initialSize(QWidget *w, QSize design);

// `themeTemplate` has its colours injected. Call once, after the QApplication exists and before any window.
void install(QApplication *app, const QString &themeTemplate);

// changed() after the scale was applied (debounced).
class Notifier final : public QObject {
  Q_OBJECT
public:
  static Notifier &instance();
signals:
  void changed();
};

} // namespace ui::scale
