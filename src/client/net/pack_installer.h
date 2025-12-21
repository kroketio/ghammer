#pragma once

// Installs one asset pack on the asset server the editor uses (local or remote): downloads it (URL; resumes and retries
// through dropped connections and stalls), checks it here (shared/models/pack_archive.h: only the zip's directory and
// the pack JSON are read), uploads it (streamed from disk, with progress) and waits while the server installs it.

#include <QElapsedTimer>
#include <QFile>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QUrl>

#include "shared/models/pack_archive.h"

class QNetworkReply;

namespace net {

class PackInstaller final : public QObject {
  Q_OBJECT

public:
  enum class Phase { Idle, Downloading, Checking, Uploading, Installing, Done, Failed, Cancelled };
  Q_ENUM(Phase)

  explicit PackInstaller(QObject *parent = nullptr);
  ~PackInstaller() override;

  void installUrl(const QUrl &url);
  void installFile(const QString &path);
  void cancel();

  [[nodiscard]] Phase phase() const { return m_phase; }
  [[nodiscard]] bool busy() const;
  // The checked pack (valid from Uploading on).
  [[nodiscard]] const PackArchive &pack() const { return m_pack; }
  [[nodiscard]] QString message() const { return m_message; }
  // Bytes per second over the last few seconds of the transfer.
  [[nodiscard]] double rate() const { return m_rate; }

  static constexpr int kDownloadAttempts = 6;
  static constexpr int kUploadAttempts = 3;
  static constexpr int kStallMs = 30000;

signals:
  void phaseChanged(net::PackInstaller::Phase phase);
  void progress(qint64 done, qint64 total);  // total < 0: unknown
  void notice(const QString &text);           // retries, waiting
  void finished(bool ok, const QString &message);

private:
  void setPhase(Phase p);
  void fail(const QString &message);
  void startDownload();
  void onDownloadData();
  void onDownloadFinished();
  void retryDownload(const QString &why);
  void check(const QString &zipPath);
  void precheck();
  void startUpload();
  void onUploadFinished();
  void retryUpload(const QString &why);
  void sample(qint64 done);
  void dropReply();
  [[nodiscard]] QString downloadPath() const;

  QNetworkAccessManager m_nam;
  QPointer<QNetworkReply> m_reply;
  Phase m_phase = Phase::Idle;
  QString m_message;
  QUrl m_url;
  QString m_zip;          // the file being installed
  bool m_ownsZip = false; // downloaded: removed once installed
  QFile m_part;
  qint64 m_offset = 0;    // bytes already in the .part file when the request started
  qint64 m_total = -1;
  bool m_gotData = false;
  int m_attempt = 0;
  QTimer m_stall;
  QTimer m_retry;
  PackArchive m_pack;
  QFile *m_upload = nullptr;
  qint64 m_sent = 0;
  QElapsedTimer m_clock;
  qint64 m_rateBytes = 0;
  qint64 m_rateAt = 0;
  double m_rate = 0;
  quint64 m_generation = 0;
};

} // namespace net
