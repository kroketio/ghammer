#pragma once

// File › Asset Server › Install Asset Packs: installs packs on the asset server the editor uses, from the list of
// published packs, a web address or a zip on disk (net::PackInstaller). One install at a time; it keeps running while
// the window is closed.

#include <QDialog>
#include <QPointer>

class QFrame;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QVBoxLayout;

namespace net {
class PackInstaller;
}

namespace ui {

class PackInstallWindow final : public QDialog {
  Q_OBJECT

public:
  // The one window, opened (or raised). `firstRun`: the editor's first start, which says why it opened.
  static PackInstallWindow *open(QWidget *parent, bool firstRun = false);
  void installFile(const QString &path);
  void installUrl(const QUrl &url);
  [[nodiscard]] net::PackInstaller *job() const { return m_job; }

private:
  explicit PackInstallWindow(QWidget *parent);
  void refreshServer();
  void refreshButtons();
  void startUrl(const QUrl &url, const QString &label);
  void chooseFile();
  void showPhase();
  void onProgress(qint64 done, qint64 total);
  void onFinished(bool ok, const QString &message);

  static QPointer<PackInstallWindow> s_window;

  net::PackInstaller *m_job = nullptr;
  QLabel *m_intro = nullptr;
  QLabel *m_server = nullptr;
  QList<QPushButton *> m_installButtons;
  QLineEdit *m_url = nullptr;
  QPushButton *m_urlInstall = nullptr;
  QPushButton *m_file = nullptr;
  QFrame *m_progressCard = nullptr;
  QLabel *m_what = nullptr;
  QLabel *m_phase = nullptr;
  QProgressBar *m_bar = nullptr;
  QLabel *m_detail = nullptr;
  QLabel *m_notice = nullptr;
  QPushButton *m_cancel = nullptr;
  QLabel *m_result = nullptr;
};

} // namespace ui
