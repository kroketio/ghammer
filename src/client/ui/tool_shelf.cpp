#include "tool_shelf.h"

#include <QAction>
#include <QGridLayout>
#include <QLabel>
#include <QRegularExpression>
#include <QScrollArea>
#include <QScrollBar>
#include <QStyle>
#include <QStyleOptionToolBar>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

#include "action_manager.h"
#include "client/ui/scale.h"

namespace ui {

namespace {

QString cleanText(QString t) {
  t.remove(QLatin1Char('&'));
  if (t.endsWith(QStringLiteral("…")))
    t.chop(1);
  return t;
}
} // namespace

ToolShelf::ToolShelf(QWidget *parent) : QToolBar(tr("Tools"), parent) {
  setObjectName(QStringLiteral("toolbar.shelf"));
  setOrientation(Qt::Vertical);
  setMovable(false);
  setFloatable(false);
  // Label fonts come from the theme (#shelfButton / #shelfTitle); the toolbar wraps the scroll area.
  scale::bind(this, [this] { applyScale(); });
  setContextMenuPolicy(Qt::PreventContextMenu);

  m_scroll = new QScrollArea(this);
  m_scroll->setObjectName(QStringLiteral("toolShelfScroll"));
  m_scroll->setWidgetResizable(true);
  m_scroll->setFrameShape(QFrame::NoFrame);
  m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  m_scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff); // a bar would eat the narrow width
  m_scroll->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
  addWidget(m_scroll);
}

QWidget *ToolShelf::makeButton(const Entry &e, QWidget *parent) {
  QAction *a = act(e.id);
  if (!a)
    return nullptr;
  auto *b = new QToolButton(parent);
  b->setObjectName(QStringLiteral("shelfButton"));
  b->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
  b->setIconSize(scale::icon(scale::Icon::Shelf));
  b->setAutoRaise(true);
  const QString label = e.label.isEmpty() ? cleanText(a->text()) : e.label;
  b->setText(label);
  b->setProperty("shelfLabel", label);  // applyScale() wraps it to the shelf width
  // Not setDefaultAction(): it would overwrite the short label with the action's text.
  auto sync = [b, a, label] {
    b->setIcon(a->icon());
    b->setEnabled(a->isEnabled());
    b->setCheckable(a->isCheckable());
    b->setChecked(a->isChecked());
    QString tip = QStringLiteral("<b>%1</b>").arg(cleanText(a->text()).toHtmlEscaped());
    if (!a->shortcut().isEmpty())
      tip += QStringLiteral("  <span style='color:#8a9ab8'>%1</span>")
                 .arg(a->shortcut().toString(QKeySequence::NativeText).toHtmlEscaped());
    if (!a->toolTip().isEmpty() && cleanText(a->toolTip()) != cleanText(a->text()))
      tip += QStringLiteral("<br>%1").arg(a->toolTip().toHtmlEscaped());
    b->setToolTip(tip);
  };
  sync();
  connect(a, &QAction::changed, b, sync);
  connect(b, &QToolButton::clicked, a, [a, b] {
    a->trigger();
    // The action's checked state may be refreshed later.
    b->setChecked(a->isChecked());
  });
  return b;
}

bool ToolShelf::event(QEvent *e) {
  // The first measurement runs before the toolbar is polished (metrics not final): measure again on
  // show and on style / font changes.
  switch (e->type()) {
    case QEvent::Show:
    case QEvent::Polish:
    case QEvent::StyleChange:
    case QEvent::FontChange:
      scheduleScale();
      break;
    default:
      break;
  }
  return QToolBar::event(e);
}

void ToolShelf::scheduleScale() {
  if (m_scalePending)
    return;
  m_scalePending = true;
  QTimer::singleShot(0, this, [this] {
    m_scalePending = false;
    applyScale();
  });
}

void ToolShelf::applyScale() {
  if (!m_content)
    return;
  // One column, as narrow as the icons and the longest word allow; measured in the theme's fonts.
  const auto buttons = m_content->findChildren<QToolButton *>(QStringLiteral("shelfButton"));
  const int pad = scale::px(6);
  int w = 0;
  for (QToolButton *b: buttons) {
    b->ensurePolished();
    b->setIconSize(scale::icon(scale::Icon::Shelf));
    w = std::max(w, b->iconSize().width() + scale::px(12));
    const QString label = b->property("shelfLabel").toString();
    for (const QString &word: label.split(QRegularExpression(QStringLiteral("[ \n]")), Qt::SkipEmptyParts))
      w = std::max(w, b->fontMetrics().horizontalAdvance(word) + pad);
  }
  for (QToolButton *b: buttons) {
    // Greedy wrap at spaces within the button width; explicit breaks stay.
    const QFontMetrics fm = b->fontMetrics();
    QStringList lines;
    for (const QString &part: b->property("shelfLabel").toString().split(QLatin1Char('\n'))) {
      QString line;
      for (const QString &word: part.split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
        const QString joined = line.isEmpty() ? word : line + QLatin1Char(' ') + word;
        if (!line.isEmpty() && fm.horizontalAdvance(joined) + pad > w) {
          lines << line;
          line = word;
        } else {
          line = joined;
        }
      }
      lines << line;
    }
    b->setText(lines.join(QLatin1Char('\n')));
    b->setFixedWidth(w);
  }
  // Titles that do not fit are drawn smaller (their own font-size beats the theme's #shelfTitle rule).
  for (QLabel *t: m_content->findChildren<QLabel *>(QStringLiteral("shelfTitle"))) {
    t->setStyleSheet(QString());
    t->ensurePolished();
    const int need = t->fontMetrics().horizontalAdvance(t->text()) + pad;
    if (need > w) {
      const int px = std::max(6, t->fontInfo().pixelSize() * w / need);
      t->setStyleSheet(QStringLiteral("font-size: %1px;").arg(px));
    }
  }
  // The toolbar gets the exact width (a fixed-width scroll area would leave spare width on its right).
  const QMargins m = m_content->layout()->contentsMargins();
  const int contentWidth = w + m.left() + m.right();
  m_scroll->setMinimumWidth(contentWidth);
  m_scroll->setMaximumWidth(QWIDGETSIZE_MAX);
  ensurePolished(); // the QSS frame of #toolbar.shelf counts in the metrics below
  QStyleOptionToolBar opt;
  initStyleOption(&opt);
  const int chrome = style()->pixelMetric(QStyle::PM_ToolBarItemMargin, &opt, this)
                     + style()->pixelMetric(QStyle::PM_ToolBarFrameWidth, &opt, this);
  setFixedWidth(contentWidth + 2 * chrome);
}

void ToolShelf::setSections(const QList<Section> &sections) {
  auto *content = new QWidget;
  content->setObjectName(QStringLiteral("toolShelfContent"));
  auto *col = new QVBoxLayout(content);
  col->setContentsMargins(3, 4, 3, 4);
  col->setSpacing(4);
  for (const Section &s: sections) {
    QList<QWidget *> buttons;
    for (const Entry &e: s.entries)
      if (QWidget *b = makeButton(e, content))
        buttons.push_back(b);
    if (buttons.isEmpty())
      continue;
    auto *title = new QLabel(s.title.toUpper(), content);
    title->setObjectName(QStringLiteral("shelfTitle"));
    col->addWidget(title);
    auto *grid = new QGridLayout;
    grid->setContentsMargins(0, 0, 0, 6);
    grid->setHorizontalSpacing(2);
    grid->setVerticalSpacing(2);
    for (int i = 0; i < buttons.size(); ++i)
      grid->addWidget(buttons[i], i, 0, Qt::AlignTop | Qt::AlignHCenter);
    col->addLayout(grid);
  }
  col->addStretch(1);
  m_scroll->setWidget(content); // deletes the previous content
  m_content = content;
  // Polished now so its layout margins are scaled when measured.
  content->ensurePolished();
  applyScale();
}

} // namespace ui
