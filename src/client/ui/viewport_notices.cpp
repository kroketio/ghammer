#include "viewport_notices.h"

#include <QTimer>

namespace ui {

namespace {
const QString kFlashKey = QStringLiteral("flash");
}

ViewportNotices::ViewportNotices(QObject *parent) : QAbstractListModel(parent) {
  m_flashTimer = new QTimer(this);
  m_flashTimer->setSingleShot(true);
  connect(m_flashTimer, &QTimer::timeout, this, [this] { set(kFlashKey, {}); });
}

void ViewportNotices::flash(const QString &text, const int ms) {
  if (text.isEmpty())
    return;
  // Always the last row.
  set(kFlashKey, {});
  set(kFlashKey, text);
  m_flashTimer->start(ms);
}

void ViewportNotices::addSource(const QString &key, Source text) {
  m_sources.emplace_back(key, std::move(text));
  scheduleRefresh();
}

void ViewportNotices::scheduleRefresh() {
  if (m_pending)
    return;
  m_pending = true;
  QTimer::singleShot(0, this, [this] { refresh(); });
}

void ViewportNotices::refresh() {
  m_pending = false;
  std::vector<QString> texts;
  texts.reserve(m_sources.size());
  for (const auto &[key, fn]: m_sources)
    texts.push_back(fn ? fn() : QString());
  for (size_t i = 0; i < m_sources.size(); ++i)
    if (texts[i].isEmpty())
      set(m_sources[i].first, {});
  // Shown in source order.
  for (size_t i = 0; i < m_sources.size(); ++i) {
    if (texts[i].isEmpty())
      continue;
    const QString &key = m_sources[i].first;
    int row = -1, before = 0;
    for (int r = 0; r < m_notices.size(); ++r) {
      if (m_notices[r].key == key)
        row = r;
      for (size_t j = 0; j < i; ++j)
        if (m_sources[j].first == m_notices[r].key)
          ++before;
    }
    if (row >= 0) {
      set(key, texts[i]);
      continue;
    }
    beginInsertRows({}, before, before);
    m_notices.insert(before, {key, texts[i]});
    endInsertRows();
  }
}

void ViewportNotices::set(const QString &key, const QString &text) {
  int row = -1;
  for (int i = 0; i < m_notices.size(); ++i)
    if (m_notices[i].key == key)
      row = i;
  if (text.isEmpty()) {
    if (row < 0)
      return;
    beginRemoveRows({}, row, row);
    m_notices.removeAt(row);
    endRemoveRows();
    return;
  }
  if (row >= 0) {
    if (m_notices[row].text == text)
      return;
    m_notices[row].text = text;
    emit dataChanged(index(row), index(row), {TextRole});
    return;
  }
  beginInsertRows({}, int(m_notices.size()), int(m_notices.size()));
  m_notices.append({key, text});
  endInsertRows();
}

int ViewportNotices::rowCount(const QModelIndex &parent) const {
  return parent.isValid() ? 0 : int(m_notices.size());
}

QVariant ViewportNotices::data(const QModelIndex &index, int role) const {
  if (!index.isValid() || index.row() >= m_notices.size())
    return {};
  const Notice &n = m_notices[index.row()];
  switch (role) {
    case KeyRole: return n.key;
    case TextRole:
    case Qt::DisplayRole: return n.text;
    default: return {};
  }
}

QHash<int, QByteArray> ViewportNotices::roleNames() const {
  return {{KeyRole, "key"}, {TextRole, "text"}};
}

} // namespace ui
