#include "preferences_dialog.h"
#include "client/ui/scale.h"

#include <QCheckBox>
#include <QComboBox>
#include <QButtonGroup>
#include <QHBoxLayout>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QToolButton>
#include <QVBoxLayout>

#include "preferences.h"
#include "client/ui/action_manager.h"
#include "client/ui/widgets.h"
#include "client/ui/external_apps.h"
#include "client/ui/file_dialog.h"
#include <cmath>
#include <QFrame>
#include <QTreeWidget>
#include <QApplication>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <algorithm>

namespace prefs {

QPointer<PreferencesDialog> PreferencesDialog::s_instance;

namespace {

QVector<QPair<QString, std::function<QWidget *(QWidget *)>>> &customPages() {
  static QVector<QPair<QString, std::function<QWidget *(QWidget *)>>> pages;
  return pages;
}

QString pageTitle(const QString &category) {
  const int slash = category.lastIndexOf(QLatin1Char('/'));
  return slash < 0 ? category : category.mid(slash + 1);
}


} // namespace

void PreferencesDialog::registerPage(const QString &name, std::function<QWidget *(QWidget *)> factory) {
  customPages().push_back({name, std::move(factory)});
}

void PreferencesDialog::showDialog(QWidget *parent, const QString &category) {
  if (!s_instance) {
    s_instance = new PreferencesDialog(parent);
    s_instance->setAttribute(Qt::WA_DeleteOnClose);
  }
  if (!category.isEmpty())
    s_instance->selectCategory(category);
  s_instance->show();
  s_instance->raise();
  s_instance->activateWindow();
}

namespace {

struct CategoryInfo {
  const char *icon;
};
const QHash<QString, CategoryInfo> &categoryInfo() {
  static const QHash<QString, CategoryInfo> info = {
      {QStringLiteral("General"), {"preferences"}},
      {QStringLiteral("Viewport"), {"view3d"}},
      {QStringLiteral("Rendering"), {"shading_rendered"}},
      {QStringLiteral("Rendering/Shadows"), {"light"}},
      {QStringLiteral("Rendering/Post"), {"shading_texture"}},
      {QStringLiteral("Grid & Snap"), {"snap_on"}},
      {QStringLiteral("Colors"), {"color"}},
      {QStringLiteral("Gizmos"), {"orientation_gimbal"}},
      {QStringLiteral("Brushes"), {"mesh_cube"}},
      {QStringLiteral("Entities"), {"decorate_linked"}},
      {QStringLiteral("Asset Server"), {"internet"}},
      {QStringLiteral("Paths"), {"file_folder"}},
      {QStringLiteral("Debug"), {"console"}},
      {QStringLiteral("Keyboard"), {"keyingset"}},
      {QStringLiteral("Project"), {"file_folder"}},
      {QStringLiteral("Level"), {"file_3d"}},
  };
  return info;
}

QString topCategory(const QString &category) {
  const int slash = category.indexOf(QLatin1Char('/'));
  return slash < 0 ? category : category.left(slash);
}

// Pages hold one scope each.
Scope scopeOfCategory(const QString &category) {
  for (const auto &d: Preferences::instance().defs())
    if (d.category == category)
      return d.scope;
  return Scope::App;
}

QString scopeHeading(Scope scope) {
  const auto &p = Preferences::instance();
  const QString what = scope == Scope::Project ? QObject::tr("Project") : QObject::tr("Level");
  const QString label = p.scopeLabel(scope);
  return p.available(scope) && !label.isEmpty() ? QStringLiteral("%1 · %2").arg(what, label) : what;
}

QString scopeMissing(Scope scope) {
  return scope == Scope::Project ? QObject::tr("No project open") : QObject::tr("No level open");
}

// Dialog style on top of the theme; sizes are scaled tokens (ui/scale.h).
const char *kStyle = R"(
#prefsSidebar { background: #0c1022; border-right: 1px solid #1f2747; }
#prefsSearch { background: #171C38; border: 1px solid #2a3360; border-radius: 6px; padding: 5px 8px; }
#prefsSearch:focus { border-color: #4866C5; }
#prefsEditorBox QLineEdit[readOnly="true"] { background: transparent; border: 1px solid #1f2747; border-radius: 4px;
  color: #8f9bbd; padding: 3px 6px; }
#prefsTree { background: transparent; border: none; outline: 0; show-decoration-selected: 0;
  selection-background-color: transparent; }
#prefsTree::item { padding: 5px 6px; border: none; border-radius: 5px; margin: 1px 4px; }
#prefsTree::item:hover { background: #171C38; }
#prefsTree::item:selected { background: #4866C5; color: white; }
#prefsTree::branch, #prefsTree::branch:selected, #prefsTree::branch:hover { image: none; border: none;
  background: transparent; }
#prefsScroll, #prefsPage { background: #101428; border: none; }
#prefsTitle { font-size: $font_page; font-weight: 600; }
#prefsSubtitle { color: #8f9bbd; }
#prefsCard { background: #151a33; border: 1px solid #232A4E; border-radius: 8px; }
#prefsRow { background: transparent; border: none; }
#prefsRow[first="false"] { border-top: 1px solid #222949; }
#prefsDesc { color: #8a95b5; font-size: $font_caption; }
#prefsDot { color: #6A84D9; }
#prefsFooter { background: #0c1022; border-top: 1px solid #1f2747; }
QPushButton#prefsPrimary { background: #4866C5; color: white; border: none; border-radius: 5px; padding: 6px 22px; }
QPushButton#prefsPrimary:hover { background: #6A84D9; }
QPushButton#prefsFlat { background: transparent; border: 1px solid #346077; border-radius: 5px; padding: 6px 14px; }
QPushButton#prefsFlat:hover { background: #171C38; }
QToolButton#prefsReset { border: none; background: transparent; padding: 2px; }
QToolButton#prefsSegment { background: #0f1328; border: 1px solid #2a3360; border-left: none; padding: 4px 6px; }
QToolButton#prefsSegment[pos="first"] { border-left: 1px solid #2a3360; border-top-left-radius: 5px;
  border-bottom-left-radius: 5px; }
QToolButton#prefsSegment[pos="last"] { border-top-right-radius: 5px; border-bottom-right-radius: 5px; }
QToolButton#prefsSegment:hover { background: #171C38; }
QToolButton#prefsSegment:checked { background: #4866C5; border-color: #6A84D9; color: white; }
QToolButton#prefsReset:hover { background: #232A4E; border-radius: 4px; }
#prefsRow QLabel, #prefsRow QCheckBox, #prefsRow QSlider, #prefsEditorBox, #prefsPage QLabel,
#prefsSidebar QLabel { background: transparent; }
#prefsDot { font-size: $font_caption; }
#prefsRow QCheckBox::indicator { width: 18px; height: 18px; border-radius: 4px; border: 1px solid #3a4580;
  background: #0f1328; }
#prefsRow QCheckBox::indicator:hover { border-color: #6A84D9; }
#prefsRow QCheckBox::indicator:checked { background: #4866C5; border-color: #6A84D9;
  image: url(:/theme/blue/checkbox_checked.png); }
QScrollBar:vertical { background: transparent; width: 10px; margin: 2px; border: none; }
QScrollBar::handle:vertical { background: #2a3360; border-radius: 4px; min-height: 30px; }
QScrollBar::handle:vertical:hover { background: #3a4580; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; border: none; background: none; }
QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: none; }
)";

constexpr int kEditorWidth = 300;

} // namespace

PreferencesDialog::PreferencesDialog(QWidget *parent) : QDialog(parent) {
  setWindowTitle(tr("Preferences"));
  ui::scale::setScaledStyleSheet(this, QString::fromLatin1(kStyle));
  resize(ui::scale::initialSize(this, {1240, 780}));

  auto *root = new QHBoxLayout(this);
  root->setContentsMargins(0, 0, 0, 0);
  root->setSpacing(0);

  // sidebar: search + category tree
  auto *sidebar = new QFrame(this);
  sidebar->setObjectName(QStringLiteral("prefsSidebar"));
  ui::scale::bind(sidebar, [sidebar] { sidebar->setFixedWidth(ui::scale::tpx(250)); });
  auto *side = new QVBoxLayout(sidebar);
  side->setContentsMargins(10, 12, 6, 12);
  side->setSpacing(8);
  m_search = new QLineEdit(sidebar);
  m_search->setObjectName(QStringLiteral("prefsSearch"));
  m_search->setPlaceholderText(tr("Search settings"));
  m_search->setClearButtonEnabled(true);
  m_search->addAction(ui::ActionManager::icon(QStringLiteral("viewzoom")), QLineEdit::LeadingPosition);
  side->addWidget(m_search);
  m_categories = new QTreeWidget(sidebar);
  m_categories->setObjectName(QStringLiteral("prefsTree"));
  m_categories->setHeaderHidden(true);
  m_categories->setRootIsDecorated(false);
  ui::scale::bind(m_categories, [this] {
    m_categories->setIndentation(ui::scale::px(14));
    m_categories->setIconSize(ui::scale::icon(ui::scale::Icon::Small));
  });
  m_categories->setFocusPolicy(Qt::NoFocus);
  side->addWidget(m_categories, 1);
  root->addWidget(sidebar);

  // right: pages + footer
  auto *right = new QVBoxLayout;
  right->setContentsMargins(0, 0, 0, 0);
  right->setSpacing(0);
  m_pages = new QStackedWidget(this);
  right->addWidget(m_pages, 1);

  auto *footer = new QFrame(this);
  footer->setObjectName(QStringLiteral("prefsFooter"));
  auto *foot = new QHBoxLayout(footer);
  foot->setContentsMargins(16, 10, 16, 10);
  m_restartNote = new QLabel(tr("● Some changes take effect after a restart."), footer);
  m_restartNote->setStyleSheet(QStringLiteral("color:#f0b45a;"));
  m_restartNote->hide();
  auto *resetPage = new QPushButton(tr("Reset Page to Defaults"), footer);
  resetPage->setObjectName(QStringLiteral("prefsFlat"));
  auto *close = new QPushButton(tr("Close"), footer);
  close->setObjectName(QStringLiteral("prefsPrimary"));
  close->setDefault(true);
  foot->addWidget(m_restartNote);
  foot->addStretch(1);
  foot->addWidget(resetPage);
  foot->addWidget(close);
  right->addWidget(footer);
  root->addLayout(right, 1);

  // categories: one page each; "A/B" nests under "A" (created when "A" has no settings).
  auto &p = Preferences::instance();
  QStringList cats = p.categories();
  cats.removeAll(QStringLiteral("Hidden")); // internal UI state, not settings
  // Hand-made top-level pages go before Debug, which stays last.
  for (const auto &[name, factory]: customPages()) {
    const qsizetype debug = cats.indexOf(QStringLiteral("Debug"));
    if (!name.contains(QLatin1Char('/')) && debug >= 0)
      cats.insert(debug, name);
    else
      cats.push_back(name);
  }

  auto addPage = [this](const QString &cat, QWidget *page) {
    m_categoryNames.push_back(cat);
    m_pages->addWidget(page);
  };
  auto itemFor = [this](const QString &cat) -> QTreeWidgetItem * {
    if (auto it = m_categoryItems.constFind(cat); it != m_categoryItems.constEnd())
      return *it;
    QTreeWidgetItem *item = nullptr;
    const int slash = cat.lastIndexOf(QLatin1Char('/'));
    if (slash < 0) {
      item = new QTreeWidgetItem(m_categories);
      const auto info = categoryInfo().value(cat);
      item->setIcon(0, ui::ActionManager::icon(QString::fromLatin1(info.icon ? info.icon : "preferences")));
    } else {
      QTreeWidgetItem *parentItem = m_categoryItems.value(cat.left(slash));
      item = new QTreeWidgetItem(parentItem);
    }
    item->setText(0, pageTitle(cat));
    item->setData(0, Qt::UserRole, cat);
    m_categoryItems.insert(cat, item);
    return item;
  };
  // Parents first, in registry order.
  for (const QString &cat: cats) {
    const QString top = topCategory(cat);
    if (!m_categoryItems.contains(top))
      itemFor(top);
  }
  for (const QString &cat: cats) {
    itemFor(cat);
    QWidget *page = nullptr;
    for (const auto &[name, factory]: customPages())
      if (name == cat)
        page = factory(m_pages);
    addPage(cat, page ? page : buildPage(cat));
  }
  ui::scale::bind(m_categories, [this] {
    const QIcon glyph = ui::subItemGlyph(ui::scale::icon(ui::scale::Icon::Small).height());
    for (QTreeWidgetItem *item: std::as_const(m_categoryItems))
      if (item->parent())
        item->setIcon(0, glyph);
  });
  m_categories->expandAll();
  // A heading without settings of its own collapses by default; the project and level groups stay open.
  for (auto it = m_categoryItems.begin(); it != m_categoryItems.end(); ++it)
    if (!m_categoryNames.contains(it.key()) && it.value()->childCount() > 0
        && it.key() != QLatin1String("Project") && it.key() != QLatin1String("Level"))
      it.value()->setExpanded(false);
  const auto refreshScopeHeadings = [this] {
    for (const Scope sc: {Scope::Project, Scope::Level}) {
      const QString top = sc == Scope::Project ? QStringLiteral("Project") : QStringLiteral("Level");
      if (QTreeWidgetItem *item = m_categoryItems.value(top)) {
        item->setText(0, scopeHeading(sc));
        item->setToolTip(0, Preferences::instance().available(sc) ? QString() : scopeMissing(sc));
      }
    }
  };
  refreshScopeHeadings();
  connect(&p, &Preferences::storeChanged, this, refreshScopeHeadings);

  connect(m_categories, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *cur) {
    if (!cur)
      return;
    const QString cat = cur->data(0, Qt::UserRole).toString();
    int idx = int(m_categoryNames.indexOf(cat));
    if (idx < 0 && cur->childCount() > 0) { // heading only: show its first child
      cur->setExpanded(true);
      idx = int(m_categoryNames.indexOf(cur->child(0)->data(0, Qt::UserRole).toString()));
    }
    if (idx >= 0)
      m_pages->setCurrentIndex(idx);
  });
  if (m_categories->topLevelItemCount() > 0)
    m_categories->setCurrentItem(m_categories->topLevelItem(0));

  connect(resetPage, &QPushButton::clicked, this, [this] {
    const int idx = m_pages->currentIndex();
    if (idx >= 0 && idx < m_categoryNames.size())
      Preferences::instance().resetCategory(m_categoryNames[idx]);
  });
  connect(close, &QPushButton::clicked, this, &QDialog::close);
  connect(m_search, &QLineEdit::textChanged, this, &PreferencesDialog::applyFilter);
  connect(&p, &Preferences::changed, this, [this](const QString &key, const QVariant &) {
    if (const Def *d = Preferences::instance().def(key); d && d->restartRequired)
      m_restartNote->show();
  });
}

void PreferencesDialog::selectCategory(const QString &category) {
  QTreeWidgetItem *item = m_categoryItems.value(category);
  if (item && !m_categoryNames.contains(category) && item->childCount() > 0)
    item = item->child(0); // a heading (Project, Level): its first page
  if (item)
    m_categories->setCurrentItem(item);
}

QWidget *PreferencesDialog::buildPage(const QString &category) {
  auto *scroll = new QScrollArea(m_pages);
  scroll->setObjectName(QStringLiteral("prefsScroll"));
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  auto *inner = new QWidget(scroll);
  inner->setObjectName(QStringLiteral("prefsPage"));
  auto *v = new QVBoxLayout(inner);
  v->setContentsMargins(28, 22, 28, 22);
  v->setSpacing(4);

  const QString heading = category.contains(QLatin1Char('/'))
      ? QStringLiteral("%1  ›  %2").arg(topCategory(category), pageTitle(category))
      : category;
  auto *title = new QLabel(heading, inner);
  title->setObjectName(QStringLiteral("prefsTitle"));
  v->addWidget(title);
  // Project / level pages: whose settings, or that nothing is open.
  const Scope scope = scopeOfCategory(category);
  QLabel *owner = nullptr;
  if (scope != Scope::App) {
    owner = new QLabel(inner);
    owner->setObjectName(QStringLiteral("prefsSubtitle"));
    v->addWidget(owner);
  }
  v->addSpacing(12);

  auto *card = new QFrame(inner);
  card->setObjectName(QStringLiteral("prefsCard"));
  if (owner) {
    const auto refreshOwner = [owner, card, scope] {
      const auto &pr = Preferences::instance();
      const bool on = pr.available(scope);
      owner->setText(on ? scopeHeading(scope) : scopeMissing(scope));
      card->setEnabled(on);
    };
    refreshOwner();
    connect(&Preferences::instance(), &Preferences::storeChanged, owner, refreshOwner);
  }
  auto *cardLayout = new QVBoxLayout(card);
  cardLayout->setContentsMargins(0, 0, 0, 0);
  cardLayout->setSpacing(0);

  auto &p = Preferences::instance();
  bool first = true;
  for (const auto &d: p.defs()) {
    if (d.category != category)
      continue;
    auto *row = new QFrame(card);
    row->setObjectName(QStringLiteral("prefsRow"));
    row->setProperty("first", first);
    first = false;
    auto *h = new QHBoxLayout(row);
    h->setContentsMargins(16, 10, 10, 10);
    h->setSpacing(12);

    // Label with a dot while off its default; the description is the tooltip.
    auto *texts = new QVBoxLayout;
    texts->setSpacing(1);
    auto *labelRow = new QHBoxLayout;
    labelRow->setSpacing(6);
    auto *label = new QLabel(d.label + (d.restartRequired ? tr("  (restart)") : QString()), row);
    auto *dot = new QLabel(QStringLiteral("●"), row);
    dot->setObjectName(QStringLiteral("prefsDot"));
    dot->setToolTip(tr("Changed from the default"));
    labelRow->addWidget(label);
    labelRow->addWidget(dot);
    labelRow->addStretch(1);
    texts->addLayout(labelRow);
    h->addLayout(texts, 1);

    QWidget *editor = buildEditor(d, row);
    auto *editorBox = new QWidget(row);
    editorBox->setObjectName(QStringLiteral("prefsEditorBox"));
    ui::scale::bind(editorBox, [editorBox] { editorBox->setFixedWidth(ui::scale::tpx(kEditorWidth)); });
    auto *eb = new QHBoxLayout(editorBox);
    eb->setContentsMargins(0, 0, 0, 0);
    const bool compact = d.type == Type::Bool || d.type == Type::Color;
    if (compact)
      eb->addStretch(1); // small editors sit at the right edge of the column
    eb->addWidget(editor, compact ? 0 : 1);
    h->addWidget(editorBox, 0, Qt::AlignVCenter);

    auto *reset = new QToolButton(row);
    reset->setObjectName(QStringLiteral("prefsReset"));
    reset->setIcon(ui::ActionManager::icon(QStringLiteral("loop_back")));
    reset->setToolTip(tr("Reset to default"));
    QSizePolicy keep = reset->sizePolicy();
    keep.setRetainSizeWhenHidden(true);
    reset->setSizePolicy(keep);
    h->addWidget(reset, 0, Qt::AlignVCenter);

    const QString key = d.key;
    connect(reset, &QToolButton::clicked, this, [key] { Preferences::instance().reset(key); });
    const auto refreshChanged = [reset, dot, key] {
      const bool changed = !Preferences::instance().isDefault(key);
      reset->setVisible(changed);
      dot->setVisible(changed);
    };
    refreshChanged();
    connect(&p, &Preferences::changed, row, [refreshChanged, key](const QString &k, const QVariant &) {
      if (k == key)
        refreshChanged();
    });
    row->setToolTip(d.tooltip);
    if (!d.enabledKey.isEmpty()) {
      const QVariant want = d.enabledValue;
      p.watch(d.enabledKey, row, [row, want](const QVariant &v) { row->setEnabled(v == want || v.toString() == want.toString()); });
    }
    cardLayout->addWidget(row);
    m_rows.insert(d.key, row);
  }
  v->addWidget(card);
  v->addStretch(1);
  scroll->setWidget(inner);
  return scroll;
}

QWidget *PreferencesDialog::buildEditor(const Def &d, QWidget *parent) {
  auto &p = Preferences::instance();
  const QString key = d.key;

  // Editors follow the stored value both ways (reset, presets and the engine change it too).
  const auto follow = [&p, key](QWidget *w, const std::function<void(const QVariant &)> &apply) {
    QObject::connect(&p, &Preferences::changed, w, [key, apply](const QString &k, const QVariant &v) {
      if (k == key)
        apply(v);
    });
  };

  switch (d.type) {
    case Type::Bool: {
      auto *cb = new QCheckBox(parent);
      cb->setChecked(p.boolValue(key));
      cb->setToolTip(d.tooltip);
      connect(cb, &QCheckBox::toggled, this, [key](bool on) { Preferences::instance().setValue(key, on); });
      follow(cb, [cb](const QVariant &v) {
        QSignalBlocker b(cb);
        cb->setChecked(v.toBool());
      });
      return cb;
    }
    case Type::Int:
    case Type::Double: {
      // Wide ranges give the low end more slider travel.
      const bool isInt = d.type == Type::Int;
      const double lo = d.minValue, hi = d.maxValue;
      const double curve = lo > 0.0 && hi / lo > 40.0 ? 2.5 : (lo >= 0.0 && hi - lo > 1000.0 ? 2.5 : 1.0);
      auto *sf = new ui::SliderField(lo, hi, isInt ? 0 : d.decimals, d.suffix, curve, parent);
      if (!isInt && d.step > 0.0)
        sf->setStep(d.step);
      else if (isInt)
        sf->setStep(1.0);
      // One width for every value box, so sliders line up. Bound after SliderField's own width so it wins.
      ui::scale::bind(sf, [sf] {
        const QFontMetrics fm(ui::scale::font(ui::scale::Font::Body, sf->font()));
        sf->field()->setFixedWidth(fm.horizontalAdvance(QStringLiteral("00000.000 lines")) + ui::scale::tpx(16));
      });
      sf->setValueQuiet(isInt ? double(p.intValue(key)) : p.doubleValue(key));
      connect(sf, &ui::SliderField::committed, this, [key, isInt](double v) {
        Preferences::instance().setValue(key, isInt ? QVariant(int(std::lround(v))) : QVariant(v));
      });
      follow(sf, [sf](const QVariant &v) { sf->setValueQuiet(v.toDouble()); });
      return sf;
    }
    case Type::Choice: {
      if (d.buttons) {
        auto *row = new QWidget(parent);
        auto *h = new QHBoxLayout(row);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(0);
        auto *group = new QButtonGroup(row);
        for (int i = 0; i < d.choices.size(); ++i) {
          auto *b = new QToolButton(row);
          b->setObjectName(QStringLiteral("prefsSegment"));
          b->setProperty("pos", i == 0 ? "first" : i == d.choices.size() - 1 ? "last" : "middle");
          b->setText(d.choices[i]);
          b->setCheckable(true);
          b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
          group->addButton(b, i);
          h->addWidget(b);
        }
        const auto indexOf = [d](const QVariant &v) {
          for (int i = 0; i < d.choiceValues.size(); ++i)
            if (d.choiceValues[i] == v || d.choiceValues[i].toString() == v.toString())
              return i;
          return d.choiceValues.isEmpty() ? v.toInt() : -1;
        };
        connect(group, &QButtonGroup::idClicked, this, [key, d](int idx) {
          Preferences::instance().setValue(key, d.choiceValues.isEmpty() ? QVariant(idx) : d.choiceValues.value(idx));
        });
        if (QAbstractButton *b = group->button(indexOf(p.value(key))))
          b->setChecked(true);
        follow(row, [group, indexOf](const QVariant &v) {
          if (QAbstractButton *b = group->button(indexOf(v)))
            b->setChecked(true);
        });
        return row;
      }
      auto *combo = new QComboBox(parent);
      combo->addItems(d.choices);
      const auto indexOf = [d](const QVariant &v) {
        if (d.choiceValues.isEmpty())
          return v.toInt();
        for (int i = 0; i < d.choiceValues.size(); ++i)
          if (d.choiceValues[i] == v || d.choiceValues[i].toString() == v.toString())
            return i;
        return 0;
      };
      combo->setCurrentIndex(indexOf(p.value(key)));
      connect(combo, &QComboBox::currentIndexChanged, this, [key, d](int idx) {
        Preferences::instance().setValue(key, d.choiceValues.isEmpty() ? QVariant(idx) : d.choiceValues.value(idx));
      });
      follow(combo, [combo, indexOf](const QVariant &v) {
        QSignalBlocker b(combo);
        combo->setCurrentIndex(indexOf(v));
      });
      return combo;
    }
    case Type::Color: {
      auto *btn = new ui::ColorButton(parent);
      ui::scale::bind(btn, [btn] { btn->setFixedWidth(ui::scale::tpx(130)); });
      btn->setToolTip(d.label);
      btn->setColor(p.colorValue(key));
      connect(btn, &ui::ColorButton::colorPicked, this,
              [key](const QColor &c) { Preferences::instance().setValue(key, c); });
      follow(btn, [btn](const QVariant &v) { btn->setColor(QColor(v.toString())); });
      return btn;
    }
    case Type::Key: {
      auto *edit = new QKeySequenceEdit(p.keyValue(key), parent);
      edit->setClearButtonEnabled(true);
      connect(edit, &QKeySequenceEdit::editingFinished, this,
              [edit, key] { Preferences::instance().setValue(key, edit->keySequence()); });
      follow(edit, [edit](const QVariant &v) {
        QSignalBlocker b(edit);
        edit->setKeySequence(QKeySequence::fromString(v.toString(), QKeySequence::PortableText));
      });
      return edit;
    }
    case Type::Path:
    case Type::File:
    case Type::Program: {
      auto *w = new QWidget(parent);
      auto *h = new QHBoxLayout(w);
      h->setContentsMargins(0, 0, 0, 0);
      auto *line = new QLineEdit(p.stringValue(key), w);
      auto *browse = new QToolButton(w);
      browse->setText(QStringLiteral("…"));
      h->addWidget(line, 1);
      h->addWidget(browse);
      const bool program = d.type == Type::Program;
      connect(line, &QLineEdit::editingFinished, this, [this, line, key, program] {
        QString text = line->text().trimmed();
        // Must be runnable (a macOS .app counts as its program).
        if (program && !text.isEmpty()) {
          const QString exe = ui::external::programPath(text);
          if (exe.isEmpty()) {
            ui::external::warnNotProgram(this, text);
            QSignalBlocker b(line);
            line->setText(Preferences::instance().stringValue(key));
            return;
          }
          text = exe;
        }
        Preferences::instance().setValue(key, text);
      });
      const bool dir = d.type == Type::Path;
      connect(browse, &QToolButton::clicked, this, [this, line, key, dir, program, label = d.label] {
        const QString start = line->text();
        const QString picked = dir       ? ui::FileDialog::getExistingDirectory(this, label, start)
                               : program ? ui::external::pickProgram(this, label, start)
                                         : ui::FileDialog::getOpenFileName(this, label, start);
        if (!picked.isEmpty())
          Preferences::instance().setValue(key, picked);
      });
      follow(line, [line](const QVariant &v) {
        QSignalBlocker b(line);
        line->setText(v.toString());
      });
      return w;
    }
    case Type::String:
    default: {
      auto *line = new QLineEdit(p.stringValue(key), parent);
      if (d.secret)
        line->setEchoMode(QLineEdit::PasswordEchoOnEdit);
      connect(line, &QLineEdit::editingFinished, this,
              [line, key] { Preferences::instance().setValue(key, line->text()); });
      follow(line, [line](const QVariant &v) {
        QSignalBlocker b(line);
        line->setText(v.toString());
      });
      return line;
    }
  }
}

void PreferencesDialog::applyFilter(const QString &text) {
  const QString needle = text.trimmed();
  auto &p = Preferences::instance();
  QSet<QString> categoriesWithHits;
  for (auto it = m_rows.begin(); it != m_rows.end(); ++it) {
    const Def *d = p.def(it.key());
    const bool hit = needle.isEmpty() || (d && (d->label.contains(needle, Qt::CaseInsensitive)
                                                || d->tooltip.contains(needle, Qt::CaseInsensitive)
                                                || d->key.contains(needle, Qt::CaseInsensitive)));
    it.value()->setVisible(hit);
    if (hit && d)
      categoriesWithHits.insert(d->category);
  }
  QTreeWidgetItem *firstHit = nullptr;
  for (auto it = m_categoryItems.begin(); it != m_categoryItems.end(); ++it) {
    const QString cat = it.key();
    const bool custom = std::any_of(customPages().begin(), customPages().end(),
                                    [&cat](const auto &pg) { return pg.first == cat; });
    bool show = needle.isEmpty() || custom || categoriesWithHits.contains(cat);
    // A heading stays while any child has hits.
    for (const QString &hitCat: categoriesWithHits)
      show = show || hitCat.startsWith(cat + QLatin1Char('/'));
    it.value()->setHidden(!show);
    if (!needle.isEmpty() && show && categoriesWithHits.contains(cat) && !firstHit)
      firstHit = it.value();
  }
  if (!needle.isEmpty()) {
    m_categories->expandAll();
    if (firstHit)
      m_categories->setCurrentItem(firstHit);
  }
}

} // namespace prefs
