#include "client/ui/scale.h"

#include <QApplication>
#include <QBoxLayout>
#include <QFormLayout>
#include <QGridLayout>
#include <QImage>
#include <QPixmap>
#include <QProxyStyle>
#include <QRegularExpression>
#include <QScreen>
#include <QStyleFactory>
#include <QTimer>
#include <QWidget>

#include <cmath>

#include "client/prefs/preferences.h"

namespace ui::scale {
namespace {

constexpr double kBodyPx = 13.0;

const char *kUiScaleKey = "General/UiScale";
const char *kFontScaleKey = "General/FontScale";

// Read once per change (pixelMetric runs for every widget).
double g_factor = 1.0;
double g_textFactor = 1.0;
QString g_themeTemplate;

void readPrefs() {
  g_factor = prefs::d(kUiScaleKey);
  if (!(g_factor > 0.0))
    g_factor = 1.0;
  const double fs = prefs::d(kFontScaleKey);
  g_textFactor = g_factor * (fs > 0.0 ? fs : 1.0);
}

double roleFactor(Font role) {
  switch (role) {
  case Font::Small: return 0.75;
  case Font::Caption: return 0.85;
  case Font::Body: return 1.0;
  case Font::Mono: return 0.95;
  case Font::Heading: return 1.15;
  case Font::Title: return 1.3;
  case Font::PageTitle: return 1.6;
  }
  return 1.0;
}

const char *roleName(Font role) {
  switch (role) {
  case Font::Small: return "small";
  case Font::Caption: return "caption";
  case Font::Body: return "body";
  case Font::Mono: return "mono";
  case Font::Heading: return "heading";
  case Font::Title: return "title";
  case Font::PageTitle: return "pageTitle";
  }
  return "body";
}

int scaled(double design, double k) {
  if (std::fabs(design) <= 1.0)
    return int(design);
  const int v = int(std::lround(design * k));
  // A size that was more than a hairline stays more than one.
  return design > 0 ? std::max(v, 2) : std::min(v, -2);
}

// Scales Qt's own metrics. Layout margins / spacing are left to the layout tracker so they are not scaled twice.
class ScaledStyle final : public QProxyStyle {
public:
  using QProxyStyle::QProxyStyle;

  // Disabled icons grey and faint: Qt's disabled look barely changes light icons on the dark theme.
  QPixmap generatedIconPixmap(QIcon::Mode mode, const QPixmap &pm, const QStyleOption *opt) const override {
    if (mode != QIcon::Disabled || pm.isNull())
      return QProxyStyle::generatedIconPixmap(mode, pm, opt);
    QImage img = pm.toImage().convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < img.height(); ++y) {
      auto *line = reinterpret_cast<QRgb *>(img.scanLine(y));
      for (int x = 0; x < img.width(); ++x) {
        const QRgb c = line[x];
        const int g = qGray(c);
        line[x] = qRgba(g, g, g, qAlpha(c) * 30 / 100);
      }
    }
    QPixmap out = QPixmap::fromImage(img);
    out.setDevicePixelRatio(pm.devicePixelRatio());
    return out;
  }

  int pixelMetric(PixelMetric m, const QStyleOption *opt, const QWidget *w) const override {
    const int v = QProxyStyle::pixelMetric(m, opt, w);
    switch (m) {
    case PM_SmallIconSize:
    case PM_LargeIconSize:
    case PM_ToolBarIconSize:
    case PM_ButtonIconSize:
    case PM_ListViewIconSize:
    case PM_IconViewIconSize:
    case PM_TabBarIconSize:
    case PM_MessageBoxIconSize:
    case PM_ToolBarItemSpacing:
    case PM_ToolBarItemMargin:
    case PM_ToolBarSeparatorExtent:
    case PM_ToolBarHandleExtent:
    case PM_ToolBarExtensionExtent:
    case PM_DockWidgetTitleMargin:
    case PM_DockWidgetTitleBarButtonMargin:
    case PM_DockWidgetSeparatorExtent:
    case PM_MenuHMargin:
    case PM_MenuVMargin:
    case PM_MenuBarItemSpacing:
    case PM_MenuBarHMargin:
    case PM_MenuBarVMargin:
    case PM_ScrollBarExtent:
    case PM_ScrollBarSliderMin:
    case PM_SliderThickness:
    case PM_SliderLength:
    case PM_SliderControlThickness:
    case PM_IndicatorWidth:
    case PM_IndicatorHeight:
    case PM_ExclusiveIndicatorWidth:
    case PM_ExclusiveIndicatorHeight:
    case PM_SplitterWidth:
    case PM_HeaderMargin:
    case PM_HeaderMarkSize:
    case PM_ButtonMargin:
    case PM_TreeViewIndentation:
    case PM_TabBarTabHSpace:
    case PM_TabBarTabVSpace:
    case PM_TitleBarHeight:
      return scaled(v, g_factor);
    default:
      return v;
    }
  }
};

//
// Qt never scales layout margins / spacing. A layout's first values are recorded as design values and
// replaced by scaled ones; a value that differs from the last applied one is a new design value.

const char *kDesignMargins = "_uiDesignMargins";
const char *kAppliedMargins = "_uiAppliedMargins";
const char *kDesignSpacing = "_uiDesignSpacing";   // QPoint(horizontal, vertical)
const char *kAppliedSpacing = "_uiAppliedSpacing";
const char *kInheritSpacing = "_uiInheritSpacing";  // a sub-layout using its parent's spacing

QPoint spacingOf(QLayout *l) {
  if (auto *g = qobject_cast<QGridLayout *>(l))
    return {g->horizontalSpacing(), g->verticalSpacing()};
  if (auto *f = qobject_cast<QFormLayout *>(l))
    return {f->horizontalSpacing(), f->verticalSpacing()};
  if (auto *b = qobject_cast<QBoxLayout *>(l))
    return {b->spacing(), b->spacing()};
  return {-1, -1};
}

void setSpacingOf(QLayout *l, QPoint s) {
  if (auto *g = qobject_cast<QGridLayout *>(l)) {
    g->setHorizontalSpacing(s.x());
    g->setVerticalSpacing(s.y());
  } else if (auto *f = qobject_cast<QFormLayout *>(l)) {
    f->setHorizontalSpacing(s.x());
    f->setVerticalSpacing(s.y());
  } else if (auto *b = qobject_cast<QBoxLayout *>(l)) {
    b->setSpacing(s.x());
  }
}

bool isScalable(QLayout *l) {
  // QMainWindow, QDockWidget, QToolBar and QStackedLayout manage their own geometry.
  return qobject_cast<QBoxLayout *>(l) || qobject_cast<QGridLayout *>(l) || qobject_cast<QFormLayout *>(l)
         || l->inherits("ui::FlowLayout");
}

void scaleLayout(QLayout *l) {
  if (!l)
    return;
  if (isScalable(l)) {
    const QMargins cur = l->contentsMargins();
    const QVariant applied = l->property(kAppliedMargins);
    if (!applied.isValid() || applied.value<QMargins>() != cur)
      l->setProperty(kDesignMargins, QVariant::fromValue(cur));
    const auto d = l->property(kDesignMargins).value<QMargins>();
    const QMargins want(px(d.left()), px(d.top()), px(d.right()), px(d.bottom()));
    if (want != cur)
      l->setContentsMargins(want);
    l->setProperty(kAppliedMargins, QVariant::fromValue(want));

    if (!l->inherits("ui::FlowLayout")) {
      const QPoint s = spacingOf(l);
      const QVariant appliedS = l->property(kAppliedSpacing);
      if (!appliedS.isValid() || appliedS.toPoint() != s) {
        // A sub-layout without its own spacing reports its parent's: don't scale it twice.
        auto *parent = qobject_cast<QLayout *>(l->parent());
        const bool inherits = !appliedS.isValid() && parent && s == spacingOf(parent);
        l->setProperty(kInheritSpacing, inherits);
        l->setProperty(kDesignSpacing, s);
      }
      if (!l->property(kInheritSpacing).toBool()) {
        const QPoint d = l->property(kDesignSpacing).toPoint();
        const QPoint want(d.x() < 0 ? d.x() : px(d.x()), d.y() < 0 ? d.y() : px(d.y()));
        if (want != s)
          setSpacingOf(l, want);
        l->setProperty(kAppliedSpacing, want);
      }
    }
  }
  // Parents before children: the inherit test compares with the parent's applied spacing.
  for (int i = 0; i < l->count(); ++i)
    if (QLayoutItem *it = l->itemAt(i); it && it->layout())
      scaleLayout(it->layout());
}

class LayoutTracker final : public QObject {
public:
  using QObject::QObject;
  bool eventFilter(QObject *o, QEvent *e) override {
    if (e->type() == QEvent::Polish && o->isWidgetType()) {
      auto *w = static_cast<QWidget *>(o);
      scaleLayout(w->layout());
      if (QWidget *p = w->parentWidget())
        scaleLayout(p->layout());
    }
    return false;
  }
};

void applyAll() {
  readPrefs();
  auto *app = qApp;
  QFont f = app->font();
  f.setPixelSize(fontPx(Font::Body));
  app->setFont(f);
  // Re-polishes every widget; the StyleChange makes them re-read ScaledStyle's metrics.
  app->setStyleSheet(expandStyleSheet(g_themeTemplate));
  // Setting the style refills Qt's per-class fonts at the desktop's size, which win over the stylesheet
  // (e.g. "QComboMenuItem" in combo popups). Setting the default font again empties that table.
  app->setFont(f);
  for (QWidget *w: QApplication::allWidgets())
    scaleLayout(w->layout());
  qDebug().noquote() << QStringLiteral("[ui] scale %1, text %2: body %3 px, heading %4 px, icons %5 px")
                            .arg(g_factor, 0, 'f', 2)
                            .arg(g_textFactor, 0, 'f', 2)
                            .arg(fontPx(Font::Body))
                            .arg(fontPx(Font::Heading))
                            .arg(icon(Icon::Small).width());
  emit Notifier::instance().changed();
}

} // namespace

Notifier &Notifier::instance() {
  static Notifier n;
  return n;
}

double factor() { return g_factor; }
double textFactor() { return g_textFactor; }
int px(double design) { return scaled(design, g_factor); }
int tpx(double design) { return scaled(design, g_textFactor); }

int fontPx(Font role) {
  return std::max(7, int(std::lround(kBodyPx * roleFactor(role) * g_textFactor)));
}

QFont font(Font role, QFont base) {
  base.setPixelSize(fontPx(role));
  if (role == Font::Heading || role == Font::Title || role == Font::PageTitle)
    base.setWeight(QFont::DemiBold);
  if (role == Font::Mono) {
    base.setFamily(QStringLiteral("monospace"));
    base.setStyleHint(QFont::Monospace);
  }
  return base;
}

QSize icon(Icon role) {
  int d = 16;
  switch (role) {
  case Icon::Small: d = 16; break;
  case Icon::Medium: d = 20; break;
  case Icon::Toolbar: d = 28; break;
  case Icon::Shelf: d = 24; break;
  case Icon::Large: d = 32; break;
  }
  return {px(d), px(d)};
}

void setRole(QWidget *w, Font role) {
  w->setProperty("uiRole", QString::fromLatin1(roleName(role)));
  // Already polished: the selector only matches after a re-polish.
  if (w->testAttribute(Qt::WA_WState_Polished)) {
    w->style()->unpolish(w);
    w->style()->polish(w);
  }
}

void bind(QObject *context, const std::function<void()> &fn) {
  fn();
  QObject::connect(&Notifier::instance(), &Notifier::changed, context, fn);
}

QString expandStyleSheet(const QString &qss) {
  // Pixel values first (the font tokens below produce px of their own).
  static const QRegularExpression pxRe(QStringLiteral(R"((?<![\w.])(-?)(\d+(?:\.\d+)?)px\b)"));
  QString out;
  out.reserve(qss.size() + 64);
  qsizetype last = 0;
  auto it = pxRe.globalMatch(qss);
  while (it.hasNext()) {
    const auto m = it.next();
    out += QStringView(qss).mid(last, m.capturedStart() - last);
    double v = m.captured(2).toDouble();
    if (!m.captured(1).isEmpty())
      v = -v;
    out += QString::number(px(v)) + QStringLiteral("px");
    last = m.capturedEnd();
  }
  out += QStringView(qss).mid(last);

  // Longest token first.
  static const struct {
    const char *token;
    Font role;
  } kTokens[] = {
      {"$font_caption", Font::Caption}, {"$font_heading", Font::Heading}, {"$font_title", Font::Title},
      {"$font_small", Font::Small},
      {"$font_body", Font::Body},       {"$font_mono", Font::Mono},       {"$font_page", Font::PageTitle},
  };
  for (const auto &t: kTokens)
    out.replace(QLatin1String(t.token), QString::number(fontPx(t.role)) + QStringLiteral("px"));
  return out;
}

void setScaledStyleSheet(QWidget *w, const QString &qss) {
  bind(w, [w, qss] { w->setStyleSheet(expandStyleSheet(qss)); });
}

QSize initialSize(QWidget *w, QSize design) {
  QSize s(px(design.width()), px(design.height()));
  QScreen *screen = w && w->screen() ? w->screen() : QGuiApplication::primaryScreen();
  if (screen) {
    const QSize avail = screen->availableGeometry().size() * 0.95;
    s = s.boundedTo(avail);
  }
  return s;
}

void install(QApplication *app, const QString &themeTemplate) {
  g_themeTemplate = themeTemplate;
  readPrefs();
  app->setStyle(new ScaledStyle(QStyleFactory::create(QStringLiteral("Fusion"))));
  app->installEventFilter(new LayoutTracker(app));

  // Debounced: restyling the whole app per slider step is too slow.
  auto *debounce = new QTimer(app);
  debounce->setSingleShot(true);
  debounce->setInterval(150);
  QObject::connect(debounce, &QTimer::timeout, app, [] { applyAll(); });
  QObject::connect(&prefs::get(), &prefs::Preferences::changed, debounce,
                   [debounce](const QString &key, const QVariant &) {
                     if (key == QLatin1String(kUiScaleKey) || key == QLatin1String(kFontScaleKey))
                       debounce->start();
                   });
  applyAll();
}

} // namespace ui::scale
