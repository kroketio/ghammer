#include "tag_picker.h"

#include <algorithm>

#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QPointer>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include "client/net/texture_catalog.h"
#include "client/ui/flow_layout.h"
#include "client/ui/scale.h"
#include "client/ui/texture_ui.h"

namespace ui {

TagChip *addFilterChip(FlowLayout *flow, QWidget *box, const QString &label, const bool exclude,
                       std::function<void()> onRemove) {
  auto *c = new TagChip(label, -1, box);
  c->setFont(scale::font(scale::Font::Caption, box->font()));
  c->setRemovable(true);
  if (exclude)
    c->setState(TagChip::State::Exclude);
  QObject::connect(c, &TagChip::removeRequested, box, [onRemove = std::move(onRemove)](const QString &) { onRemove(); });
  flow->addWidget(c);
  return c;
}

TagPicker::TagPicker(QWidget *parent) : QWidget(parent) {
  auto *v = new QVBoxLayout(this);
  v->setContentsMargins(0, 0, 0, 0);
  v->setSpacing(8);

  v->addWidget(sectionLabel(tr("Popular"), this));
  m_popularBox = new QWidget(this);
  m_popularFlow = new FlowLayout(m_popularBox, 5, 5);
  v->addWidget(m_popularBox);

  v->addSpacing(6);
  auto *head = new QHBoxLayout();
  head->addWidget(sectionLabel(tr("Tags in results"), this));
  head->addStretch(1);
  m_summary = new QLabel(this);
  m_summary->setObjectName(QStringLiteral("dlgDesc"));
  head->addWidget(m_summary);
  v->addLayout(head);

  m_debounce = new QTimer(this);
  m_debounce->setSingleShot(true);
  m_debounce->setInterval(200);
  connect(m_debounce, &QTimer::timeout, this, &TagPicker::rebuildList);

  m_filter = new QLineEdit(this);
  m_filter->setPlaceholderText(tr("Find a tag…"));
  m_filter->setClearButtonEnabled(true);
  connect(m_filter, &QLineEdit::textChanged, this, [this] {
    m_limit = 25;
    m_debounce->start();
  });
  v->addWidget(m_filter);

  m_list = sidebarList(this);
  auto *delegate = new CountListDelegate(m_list);
  delegate->setBars(true);
  m_list->setItemDelegate(delegate);
  scale::bind(m_list, [this] { m_list->doItemsLayout(); });
  m_list->setSelectionMode(QAbstractItemView::NoSelection);
  m_list->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(m_list, &QListWidget::itemClicked, this,
          [this](QListWidgetItem *it) { addTag(it->data(Qt::UserRole).toString(), false); });
  connect(m_list, &QListWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
    if (QListWidgetItem *it = m_list->itemAt(pos))
      addTag(it->data(Qt::UserRole).toString(), true);
  });
  v->addWidget(m_list);

  m_more = new QToolButton(this);
  m_more->setObjectName(QStringLiteral("tbLink"));
  m_more->setText(tr("Show 25 more"));
  connect(m_more, &QToolButton::clicked, this, [this] {
    m_limit += 25;
    rebuildList();
  });
  v->addWidget(m_more, 0, Qt::AlignLeft);
  m_more->hide();
}

void TagPicker::addTag(const QString &tag, const bool exclude) {
  if (tag.isEmpty())
    return;
  m_tags.removeAll(tag);
  m_notTags.removeAll(tag);
  (exclude ? m_notTags : m_tags).append(tag);
  {
    QSignalBlocker b(m_filter);
    m_filter->clear();
  }
  rebuildPopular(m_popular);
  emit changed();
}

void TagPicker::removeTag(const QString &tag) {
  m_tags.removeAll(tag);
  m_notTags.removeAll(tag);
  rebuildPopular(m_popular);
  emit changed();
}

void TagPicker::clear(const bool notify) {
  m_tags.clear();
  m_notTags.clear();
  {
    QSignalBlocker b(m_filter);
    m_filter->clear();
  }
  m_limit = 25;
  rebuildPopular(m_popular);
  if (notify)
    emit changed();
}

void TagPicker::setPopularFilter(const QJsonObject &filter) {
  m_popularFilter = filter;
  refreshPopular();
}

void TagPicker::refreshPopular() {
  QPointer<TagPicker> self(this);
  net::TextureCatalog::instance().popularTags(m_popularFilter, this, [self](const QVector<QPair<QString, int>> &tags) {
    if (self)
      self->rebuildPopular(tags);
  });
}

void TagPicker::rebuildPopular(const QVector<QPair<QString, int>> &tags) {
  m_popular = tags;
  m_popularFlow->clear();
  const QFont f = scale::font(scale::Font::Caption, font());
  for (const auto &[tag, count]: tags) {
    if (count <= 0)
      continue;
    auto *chip = new TagChip(tag, count, m_popularBox);
    chip->setFont(f);
    if (m_tags.contains(tag))
      chip->setState(TagChip::State::Include);
    else if (m_notTags.contains(tag))
      chip->setState(TagChip::State::Exclude);
    connect(chip, &TagChip::toggledInclude, this,
            [this](const QString &t, bool on) { on ? addTag(t, false) : removeTag(t); });
    connect(chip, &TagChip::excludeRequested, this,
            [this](const QString &t) { m_notTags.contains(t) ? removeTag(t) : addTag(t, true); });
    m_popularFlow->addWidget(chip);
  }
  m_popularBox->updateGeometry();
}

void TagPicker::setResult(const QVector<QPair<QString, int>> &facets, const int distinctTags, const QJsonObject &query) {
  m_facets = facets;
  m_distinct = distinctTags;
  m_query = query;
  rebuildList();
}

void TagPicker::rebuildList() {
  auto fill = [this](const QVector<QPair<QString, int>> &tags, int distinct) {
    m_list->clear();
    int maxCount = 1;
    for (const auto &t: tags)
      maxCount = std::max(maxCount, t.second);
    int shown = 0;
    for (const auto &[tag, count]: tags) {
      if (shown >= m_limit)
        break;
      auto *it = new QListWidgetItem(tag);
      it->setData(Qt::UserRole, tag);
      it->setData(Qt::UserRole + 1, QLocale().toString(count));
      it->setData(Qt::UserRole + 2, double(count) / maxCount);
      m_list->addItem(it);
      ++shown;
    }
    m_more->setVisible(tags.size() > m_limit);
    m_summary->setText(distinct > 0 ? tr("%1 tags").arg(QLocale().toString(distinct)) : QString());
    fitListHeight(m_list, std::max(10, std::min(shown, 60)));
  };
  const QString needle = m_filter->text().trimmed();
  if (needle.isEmpty()) {
    fill(m_facets, m_distinct);
    return;
  }
  QJsonObject filter = m_query;
  filter.remove(QStringLiteral("facets"));
  QPointer<TagPicker> self(this);
  net::TextureCatalog::instance().tagSearch(needle, filter, 200, this,
                                            [self, fill](const QVector<QPair<QString, int>> &tags) {
                                              if (self)
                                                fill(tags, int(tags.size()));
                                            });
}

void TagPicker::addActiveChips(FlowLayout *flow, QWidget *box) {
  for (const QString &t: m_tags)
    addFilterChip(flow, box, t, false, [this, t] { removeTag(t); });
  for (const QString &t: m_notTags)
    addFilterChip(flow, box, tr("not %1").arg(t), true, [this, t] { removeTag(t); });
}

} // namespace ui
