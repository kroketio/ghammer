#include "flow_layout.h"

#include <QLocale>
#include <QMouseEvent>
#include <QPainter>
#include <QWidget>

namespace ui {

FlowLayout::FlowLayout(QWidget *parent, int hSpacing, int vSpacing) : QLayout(parent), m_h(hSpacing), m_v(vSpacing) {
  setContentsMargins(0, 0, 0, 0);
}

FlowLayout::~FlowLayout() {
  while (QLayoutItem *item = takeAt(0))
    delete item;
}

void FlowLayout::addItem(QLayoutItem *item) { m_items.append(item); }
int FlowLayout::count() const { return int(m_items.size()); }
QLayoutItem *FlowLayout::itemAt(int index) const { return m_items.value(index); }
QLayoutItem *FlowLayout::takeAt(int index) {
  return index >= 0 && index < m_items.size() ? m_items.takeAt(index) : nullptr;
}

void FlowLayout::clear() {
  while (QLayoutItem *item = takeAt(0)) {
    if (QWidget *w = item->widget())
      w->deleteLater();
    delete item;
  }
  invalidate();
}

int FlowLayout::heightForWidth(int width) const { return doLayout(QRect(0, 0, width, 0), true); }

QSize FlowLayout::minimumSize() const {
  QSize size;
  for (const QLayoutItem *item: m_items)
    size = size.expandedTo(item->minimumSize());
  const QMargins m = contentsMargins();
  return size + QSize(m.left() + m.right(), m.top() + m.bottom());
}

void FlowLayout::setGeometry(const QRect &rect) {
  QLayout::setGeometry(rect);
  doLayout(rect, false);
}

int FlowLayout::doLayout(const QRect &rect, bool testOnly) const {
  const QMargins m = contentsMargins();
  const QRect r = rect.adjusted(m.left(), m.top(), -m.right(), -m.bottom());
  int x = r.x();
  int y = r.y();
  int lineHeight = 0;
  for (QLayoutItem *item: m_items) {
    if (item->widget() && !item->widget()->isVisibleTo(item->widget()->parentWidget()))
      continue;
    const QSize hint = item->sizeHint();
    int nextX = x + hint.width() + m_h;
    if (nextX - m_h > r.right() + 1 && lineHeight > 0) {
      x = r.x();
      y += lineHeight + m_v;
      nextX = x + hint.width() + m_h;
      lineHeight = 0;
    }
    if (!testOnly)
      item->setGeometry(QRect(QPoint(x, y), hint));
    x = nextX;
    lineHeight = std::max(lineHeight, hint.height());
  }
  return y + lineHeight - rect.y() + m.bottom();
}

TagChip::TagChip(const QString &tag, int count, QWidget *parent) : QToolButton(parent), m_tag(tag), m_count(count) {
  setCursor(Qt::PointingHandCursor);
  setFocusPolicy(Qt::NoFocus);
  connect(this, &QToolButton::clicked, this, [this] {
    if (m_removable) {
      emit removeRequested(m_tag);
      return;
    }
    const bool on = m_state != State::Include;
    setState(on ? State::Include : State::Off);
    emit toggledInclude(m_tag, on);
  });
}

void TagChip::setCount(int count) {
  m_count = count;
  updateGeometry();
  update();
}

void TagChip::setState(State s) {
  m_state = s;
  update();
}

void TagChip::setRemovable(bool on) {
  m_removable = on;
  setToolTip(on ? tr("Remove this filter") : QString());
  updateGeometry();
}

void TagChip::mousePressEvent(QMouseEvent *e) {
  if (e->button() == Qt::RightButton && !m_removable) {
    setState(m_state == State::Exclude ? State::Off : State::Exclude);
    emit excludeRequested(m_tag);
    return;
  }
  QToolButton::mousePressEvent(e);
}

QString TagChip::label() const {
  QString s = m_state == State::Exclude ? QStringLiteral("−") + m_tag : m_tag;
  return s;
}

QSize TagChip::sizeHint() const {
  const QFontMetrics fm(font());
  int w = fm.horizontalAdvance(label()) + 20;
  if (m_count >= 0)
    w += fm.horizontalAdvance(QLocale().toString(m_count)) + 8;
  if (m_removable)
    w += 14;
  return {w, fm.height() + 8};
}

void TagChip::paintEvent(QPaintEvent *) {
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);
  const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
  const bool hover = underMouse();
  QColor bg(44, 50, 68), border(66, 74, 98), text(196, 204, 226), countColor(132, 144, 172);
  if (m_state == State::Include || m_removable) {
    bg = QColor(34, 118, 200);
    border = QColor(70, 150, 230);
    text = Qt::white;
    countColor = QColor(200, 225, 255);
  } else if (m_state == State::Exclude) {
    bg = QColor(120, 44, 52);
    border = QColor(170, 70, 80);
    text = QColor(255, 220, 224);
    countColor = QColor(230, 170, 176);
  }
  if (hover)
    bg = bg.lighter(118);
  p.setPen(QPen(border, 1));
  p.setBrush(bg);
  p.drawRoundedRect(r, r.height() / 2, r.height() / 2);
  const QFontMetrics fm(font());
  int x = 10;
  p.setPen(text);
  const QString l = label();
  p.drawText(QRect(x, 0, fm.horizontalAdvance(l) + 1, height()), Qt::AlignVCenter | Qt::AlignLeft, l);
  x += fm.horizontalAdvance(l) + 6;
  if (m_count >= 0) {
    p.setPen(countColor);
    const QString c = QLocale().toString(m_count);
    p.drawText(QRect(x, 0, fm.horizontalAdvance(c) + 1, height()), Qt::AlignVCenter | Qt::AlignLeft, c);
    x += fm.horizontalAdvance(c) + 4;
  }
  if (m_removable) {
    p.setPen(QPen(text, 1.4));
    const QPointF c(width() - 11.0, height() / 2.0);
    p.drawLine(c + QPointF(-3, -3), c + QPointF(3, 3));
    p.drawLine(c + QPointF(-3, 3), c + QPointF(3, -3));
  }
}

} // namespace ui
