#pragma once

#include <QDialog>

class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QStackedWidget;
class QQuickWidget;

namespace vk {
class Engine3D;
}

namespace ui {

// The project chooser (qml/ProjectSplash.qml for the scene). run() returns the root of the project to
// open (existing or just created), or empty when cancelled.
class ProjectDialog final : public QDialog {
  Q_OBJECT

public:
  enum class Mode {
    Startup, // closing quits the editor (the caller's choice)
    Open,
    Create,  // the New Project page
  };

  static QString run(QWidget *parent, Mode mode);

private:
  ProjectDialog(QWidget *parent, Mode mode);

  QWidget *buildRecentPage();
  QWidget *buildNewPage();
  QWidget *buildScaleControls(QWidget *parent);
  QWidget *buildServerControls(QWidget *parent);
  void showPage(int page);
  void rebuildRecent();
  void openSelected();
  void openOther();
  void validateNew();
  void createProject();
  void accept(const QString &root);

  Mode m_mode;
  QString m_result;
  QStackedWidget *m_pages = nullptr;
  // Recent
  QListWidget *m_recent = nullptr;
  QLabel *m_recentEmpty = nullptr;
  QLabel *m_openProblem = nullptr;
  QPushButton *m_open = nullptr;
  // New
  QLineEdit *m_name = nullptr;
  QLineEdit *m_location = nullptr;
  QLabel *m_resultPath = nullptr;
  QLabel *m_problem = nullptr;
  QPushButton *m_openExisting = nullptr;
  QPushButton *m_create = nullptr;
  QQuickWidget *m_scene = nullptr;
  vk::Engine3D *m_splash = nullptr;

private slots:
  void updateSplashCamera();
};

} // namespace ui
