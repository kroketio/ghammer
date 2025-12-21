#include "pref_rows.h"

#include <QApplication>
#include <QClipboard>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QToolButton>

#include "client/ui/action_manager.h"
#include "client/ui/scale.h"

namespace ui {

namespace {

constexpr int kEditorWidth = 300;  // as the generated pages' editor column

} // namespace

QWidget *prefValueField(QWidget *parent, QLineEdit *&line, bool secret, bool copy) {
  auto *box = new QWidget(parent);
  box->setObjectName(QStringLiteral("prefsEditorBox"));
  scale::bind(box, [box] { box->setFixedWidth(scale::tpx(kEditorWidth)); });
  auto *h = new QHBoxLayout(box);
  h->setContentsMargins(0, 0, 0, 0);
  h->setSpacing(4);
  line = new QLineEdit(box);
  line->setReadOnly(true);
  if (secret)
    line->setEchoMode(QLineEdit::Password);
  h->addWidget(line, 1);
  if (secret) {
    auto *eye = new QToolButton(box);
    eye->setObjectName(QStringLiteral("prefsReset"));
    eye->setCheckable(true);
    eye->setIcon(ActionManager::icon(QStringLiteral("hide_on")));
    eye->setToolTip(QObject::tr("Show the password"));
    QLineEdit *l = line;
    QObject::connect(eye, &QToolButton::toggled, eye, [eye, l](bool on) {
      l->setEchoMode(on ? QLineEdit::Normal : QLineEdit::Password);
      eye->setIcon(ActionManager::icon(on ? QStringLiteral("hide_off") : QStringLiteral("hide_on")));
    });
    h->addWidget(eye);
  }
  if (copy) {
    auto *btn = new QToolButton(box);
    btn->setObjectName(QStringLiteral("prefsReset"));
    btn->setIcon(ActionManager::icon(QStringLiteral("duplicate")));
    btn->setToolTip(QObject::tr("Copy"));
    QLineEdit *l = line;
    QObject::connect(btn, &QToolButton::clicked, btn, [l] { QApplication::clipboard()->setText(l->text()); });
    h->addWidget(btn);
  }
  return box;
}

QHBoxLayout *prefFieldButtons(QWidget *field) {
  return static_cast<QHBoxLayout *>(field->layout());
}

QFrame *prefRow(QWidget *card, const QString &label, const QString &tooltip, QWidget *editor, bool first) {
  auto *r = new QFrame(card);
  r->setObjectName(QStringLiteral("prefsRow"));
  r->setProperty("first", first);
  r->setToolTip(tooltip);
  auto *h = new QHBoxLayout(r);
  h->setContentsMargins(16, 10, 10, 10);
  h->setSpacing(12);
  h->addWidget(new QLabel(label, r), 1);
  editor->setParent(r);
  h->addWidget(editor, 0, Qt::AlignVCenter);
  return r;
}

} // namespace ui
