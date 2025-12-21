#include "pack_installer.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QStorageInfo>
#include <QUrlQuery>
#include <QtConcurrent>

#include "asset_client.h"
#include "shared/lib/globals.h"

namespace net {

namespace {

constexpr int kBackoffSeconds[] = {2, 4, 8, 16, 30, 30};
constexpr qint64 kSpareBytes = qint64(64) << 20;

int backoff(int attempt) {
  return kBackoffSeconds[std::clamp(attempt - 1, 0, int(std::size(kBackoffSeconds)) - 1)];
}

int statusOf(QNetworkReply *r) {
  return r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
}

QString httpText(QNetworkReply *r) {
  const QString reason = r->attribute(QNetworkRequest::HttpReasonPhraseAttribute).toString();
  return reason.isEmpty() ? QStringLiteral("HTTP %1").arg(statusOf(r))
                          : QStringLiteral("HTTP %1 %2").arg(statusOf(r)).arg(reason);
}

bool retryableStatus(int status) {
  return status == 408 || status == 429 || status >= 500;
}

} // namespace

PackInstaller::PackInstaller(QObject *parent) : QObject(parent) {
  m_nam.setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy);
  m_stall.setSingleShot(true);
  m_stall.setInterval(kStallMs);
  connect(&m_stall, &QTimer::timeout, this, [this] {
    if (m_reply) {
      m_reply->setProperty("stalled", true);
      m_reply->abort();
    }
  });
  m_retry.setSingleShot(true);
  connect(&m_retry, &QTimer::timeout, this, [this] {
    if (m_phase == Phase::Downloading)
      startDownload();
    else if (m_phase == Phase::Uploading)
      startUpload();
  });
}

PackInstaller::~PackInstaller() {
  if (busy())
    cancel();
}

bool PackInstaller::busy() const {
  return m_phase == Phase::Downloading || m_phase == Phase::Checking || m_phase == Phase::Uploading ||
         m_phase == Phase::Installing;
}

void PackInstaller::setPhase(Phase p) {
  if (m_phase == p)
    return;
  m_phase = p;
  emit phaseChanged(p);
}

void PackInstaller::dropReply() {
  if (!m_reply)
    return;
  QNetworkReply *r = m_reply;
  m_reply = nullptr;
  disconnect(r, nullptr, this, nullptr);
  r->abort();
  r->deleteLater();
}

void PackInstaller::fail(const QString &message) {
  ++m_generation;
  m_stall.stop();
  m_retry.stop();
  dropReply();
  m_part.close();
  delete m_upload;
  m_upload = nullptr;
  m_message = message;
  setPhase(Phase::Failed);
  emit finished(false, message);
}

void PackInstaller::cancel() {
  if (!busy())
    return;
  const Phase was = m_phase;
  ++m_generation;
  m_phase = Phase::Cancelled;
  m_stall.stop();
  m_retry.stop();
  dropReply();
  if (m_part.isOpen()) {
    m_part.close();
    m_part.remove();
  } else if (was == Phase::Downloading) {
    QFile::remove(downloadPath() + QStringLiteral(".part"));
  }
  delete m_upload;
  m_upload = nullptr;
  if (m_ownsZip && !m_zip.isEmpty())
    QFile::remove(m_zip);
  m_message = tr("Cancelled");
  emit phaseChanged(m_phase);
  emit finished(false, m_message);
}

void PackInstaller::sample(qint64 done) {
  const qint64 now = m_clock.elapsed();
  if (now - m_rateAt < 1000)
    return;
  const double r = double(done - m_rateBytes) * 1000.0 / double(now - m_rateAt);
  m_rate = m_rate <= 0 ? r : 0.6 * m_rate + 0.4 * r;
  m_rateAt = now;
  m_rateBytes = done;
}

// ---- download ----

QString PackInstaller::downloadPath() const {
  const QByteArray id = QCryptographicHash::hash(m_url.toEncoded(), QCryptographicHash::Sha1).toHex().left(16);
  return QDir(gs::cacheDirectory).absoluteFilePath(QStringLiteral("downloads/%1.zip").arg(QString::fromLatin1(id)));
}

void PackInstaller::installUrl(const QUrl &url) {
  if (busy())
    return;
  if (url.isLocalFile()) {
    installFile(url.toLocalFile());
    return;
  }
  ++m_generation;
  m_url = url;
  m_pack = {};
  m_message.clear();
  m_zip.clear();
  m_ownsZip = true;
  m_attempt = 0;
  m_total = -1;
  if (url.scheme() != QLatin1String("http") && url.scheme() != QLatin1String("https")) {
    fail(tr("Not a web address: %1").arg(url.toString()));
    return;
  }
  if (QFileInfo::exists(downloadPath())) {  // downloaded before, not installed yet
    check(downloadPath());
    return;
  }
  startDownload();
}

void PackInstaller::startDownload() {
  setPhase(Phase::Downloading);
  const QString part = downloadPath() + QStringLiteral(".part");
  QDir().mkpath(QFileInfo(part).absolutePath());
  m_part.close();
  m_part.setFileName(part);
  if (!m_part.open(QIODevice::ReadWrite | QIODevice::Append)) {
    fail(tr("Cannot write %1").arg(part));
    return;
  }
  m_offset = m_part.size();
  m_gotData = false;
  QNetworkRequest req(m_url);
  req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("gHammer"));
  if (m_offset > 0)
    req.setRawHeader("Range", "bytes=" + QByteArray::number(m_offset) + '-');
  m_reply = m_nam.get(req);
  m_reply->setReadBufferSize(qint64(4) << 20);
  m_reply->setProperty("gen", m_generation);
  connect(m_reply, &QNetworkReply::readyRead, this, &PackInstaller::onDownloadData);
  connect(m_reply, &QNetworkReply::finished, this, &PackInstaller::onDownloadFinished);
  m_clock.restart();
  m_rateAt = 0;
  m_rateBytes = m_offset;
  m_rate = 0;
  m_stall.start();
  emit progress(m_offset, m_total);
}

void PackInstaller::onDownloadData() {
  if (!m_reply || m_phase != Phase::Downloading)
    return;
  const int status = statusOf(m_reply);
  if (status != 200 && status != 206)
    return;  // an error page: finished() decides
  if (!m_reply->property("seen").toBool()) {
    m_reply->setProperty("seen", true);
    if (status == 200) {
      if (m_offset > 0) {
        m_part.resize(0);
        m_offset = 0;
      }
      const qint64 len = m_reply->header(QNetworkRequest::ContentLengthHeader).toLongLong();
      m_total = len > 0 ? len : -1;
    } else {
      static const QRegularExpression re(QStringLiteral(R"(bytes\s+(\d+)-(\d+)/(\d+|\*))"));
      const auto m = re.match(QString::fromLatin1(m_reply->rawHeader("Content-Range")));
      if (!m.hasMatch() || m.captured(1).toLongLong() != m_offset) {
        m_part.resize(0);
        m_offset = 0;
        m_reply->setProperty("restart", true);
        m_reply->abort();
        return;
      }
      m_total = m.captured(3) == QLatin1String("*") ? -1 : m.captured(3).toLongLong();
    }
    if (m_total > 0) {
      const QStorageInfo disk(QFileInfo(m_part.fileName()).absolutePath());
      if (disk.isValid() && disk.bytesAvailable() < m_total - m_offset + kSpareBytes) {
        fail(tr("Not enough disk space to download %1").arg(QLocale().formattedDataSize(m_total)));
        return;
      }
    }
  }
  const QByteArray chunk = m_reply->readAll();
  if (chunk.isEmpty())
    return;
  if (m_part.write(chunk) != chunk.size()) {
    fail(tr("Cannot write the download (disk full?)"));
    return;
  }
  m_gotData = true;
  m_stall.start();
  const qint64 have = m_part.size();
  sample(have);
  emit progress(have, m_total);
}

void PackInstaller::onDownloadFinished() {
  QNetworkReply *reply = m_reply;
  if (!reply || reply->property("gen").toULongLong() != m_generation || m_phase != Phase::Downloading)
    return;
  m_stall.stop();
  if (reply->error() == QNetworkReply::NoError)
    onDownloadData();
  m_reply = nullptr;
  reply->deleteLater();
  if (m_phase != Phase::Downloading)
    return;
  m_part.flush();
  const int status = statusOf(reply);
  const qint64 have = m_part.size();

  if (reply->property("restart").toBool()) {
    retryDownload(tr("The server sent another part of the file"));
    return;
  }
  if (reply->error() == QNetworkReply::NoError && (status == 200 || status == 206)) {
    if (m_total >= 0 && have != m_total) {
      retryDownload(tr("The download ended early"));
      return;
    }
    if (have == 0) {
      fail(tr("The download is empty"));
      return;
    }
    m_part.close();
    const QString zip = downloadPath();
    QFile::remove(zip);
    if (!QFile::rename(m_part.fileName(), zip)) {
      fail(tr("Cannot write %1").arg(zip));
      return;
    }
    check(zip);
    return;
  }
  if (status == 416) {  // the range asked for is past the end: start over
    m_part.resize(0);
    retryDownload(httpText(reply));
    return;
  }
  if (status >= 400 && !retryableStatus(status)) {
    m_part.close();
    m_part.remove();
    fail(tr("Download failed: %1").arg(httpText(reply)));
    return;
  }
  if (reply->property("stalled").toBool())
    retryDownload(tr("No data for %1 s").arg(kStallMs / 1000));
  else
    retryDownload(status >= 400 ? httpText(reply) : reply->errorString());
}

void PackInstaller::retryDownload(const QString &why) {
  if (m_gotData)
    m_attempt = 0;  // it was moving: count again from here
  if (++m_attempt > kDownloadAttempts) {
    m_part.close();
    fail(tr("Download failed: %1").arg(why));
    return;
  }
  const int s = backoff(m_attempt);
  emit notice(tr("%1 - retrying in %2 s (%3 of %4)").arg(why).arg(s).arg(m_attempt).arg(kDownloadAttempts));
  m_retry.start(s * 1000);
}

// ---- check ----

void PackInstaller::installFile(const QString &path) {
  if (busy())
    return;
  ++m_generation;
  m_url = QUrl::fromLocalFile(path);
  m_pack = {};
  m_message.clear();
  m_ownsZip = false;
  check(path);
}

void PackInstaller::check(const QString &zipPath) {
  m_zip = zipPath;
  setPhase(Phase::Checking);
  const quint64 gen = m_generation;
  auto *watch = new QFutureWatcher<PackArchive>(this);
  connect(watch, &QFutureWatcher<PackArchive>::finished, this, [this, watch, gen] {
    watch->deleteLater();
    if (gen != m_generation || m_phase != Phase::Checking)
      return;
    m_pack = watch->result();
    if (!m_pack.ok) {
      if (m_ownsZip)
        QFile::remove(m_zip);
      fail(tr("Not a valid asset pack: %1").arg(m_pack.error));
      return;
    }
    precheck();
  });
  watch->setFuture(QtConcurrent::run([zipPath] { return inspectPackArchive(zipPath); }));
}

void PackInstaller::precheck() {
  if (!AssetClient::instance().isOnline()) {
    fail(tr("Asset server offline"));
    return;
  }
  QUrlQuery q;
  q.addQueryItem(QStringLiteral("name"), m_pack.name);
  q.addQueryItem(QStringLiteral("type"), m_pack.type);
  q.addQueryItem(QStringLiteral("size"), QString::number(QFileInfo(m_zip).size()));
  const quint64 gen = m_generation;
  AssetClient::instance().httpGet(
      QStringLiteral("/api/2/packs/install?") + q.toString(QUrl::FullyEncoded), this,
      [this, gen](QNetworkReply *reply) {
        if (gen != m_generation || m_phase != Phase::Checking)
          return;
        if (statusOf(reply) == 404 || statusOf(reply) == 405) {
          fail(tr("This asset server cannot install packs (update it)"));
          return;
        }
        const Result r = AssetClient::resultFromReply(reply);
        if (!r.ok) {
          fail(r.message.isEmpty() ? tr("Asset server offline") : r.message);
          return;
        }
        m_attempt = 0;
        startUpload();
      },
      15000);
}

// ---- upload ----

void PackInstaller::startUpload() {
  setPhase(Phase::Uploading);
  delete m_upload;
  m_upload = new QFile(m_zip, this);
  if (!m_upload->open(QIODevice::ReadOnly)) {
    fail(tr("Cannot read %1").arg(m_zip));
    return;
  }
  const qint64 size = m_upload->size();
  QNetworkRequest req = AssetClient::instance().request(QStringLiteral("/api/2/packs/install"));
  req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/zip"));
  req.setHeader(QNetworkRequest::ContentLengthHeader, size);
  req.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
  m_sent = 0;
  m_reply = m_nam.post(req, m_upload);
  m_reply->setProperty("gen", m_generation);
  connect(m_reply, &QNetworkReply::uploadProgress, this, [this](qint64 sent, qint64 total) {
    if (m_phase != Phase::Uploading)
      return;
    if (sent > m_sent) {
      m_sent = sent;
      m_stall.start();
    }
    sample(sent);
    emit progress(sent, total);
    if (total > 0 && sent >= total) {
      m_stall.stop();  // the server is installing: as long as it takes
      setPhase(Phase::Installing);
    }
  });
  connect(m_reply, &QNetworkReply::finished, this, &PackInstaller::onUploadFinished);
  m_clock.restart();
  m_rateAt = 0;
  m_rateBytes = 0;
  m_rate = 0;
  m_stall.start();
  emit progress(0, size);
}

void PackInstaller::onUploadFinished() {
  QNetworkReply *reply = m_reply;
  if (!reply || reply->property("gen").toULongLong() != m_generation ||
      (m_phase != Phase::Uploading && m_phase != Phase::Installing))
    return;
  m_reply = nullptr;
  reply->deleteLater();
  m_stall.stop();
  delete m_upload;
  m_upload = nullptr;
  const int status = statusOf(reply);
  const Result r = AssetClient::resultFromReply(reply);
  if (reply->error() == QNetworkReply::NoError && r.ok) {
    const int count = r.data.value(QStringLiteral("pack")).toObject().value(QStringLiteral("count")).toInt();
    if (m_ownsZip)
      QFile::remove(m_zip);
    const QString assets = count == 1 ? tr("1 asset") : tr("%1 assets").arg(count);
    m_message = tr("Installed %1 (%2, %3)").arg(m_pack.name, packTypeLabel(m_pack.type), assets);
    m_phase = Phase::Done;
    emit phaseChanged(m_phase);
    emit finished(true, m_message);
    return;
  }
  if (status >= 400 && !retryableStatus(status)) {
    fail(r.message.isEmpty() ? httpText(reply) : r.message);
    return;
  }
  if (m_phase == Phase::Installing) {
    fail(tr("Lost the connection while the asset server was installing the pack; it may still finish"));
    return;
  }
  retryUpload(reply->property("stalled").toBool() ? tr("The upload stalled")
              : status >= 400                     ? (r.message.isEmpty() ? httpText(reply) : r.message)
                                                  : reply->errorString());
}

void PackInstaller::retryUpload(const QString &why) {
  if (++m_attempt > kUploadAttempts) {
    fail(tr("Upload failed: %1").arg(why));
    return;
  }
  if (!AssetClient::instance().isOnline() && AssetClient::instance().state() == AssetClient::State::Offline) {
    fail(tr("Upload failed: asset server offline"));
    return;
  }
  const int s = backoff(m_attempt);
  emit notice(tr("%1 - retrying in %2 s (%3 of %4)").arg(why).arg(s).arg(m_attempt).arg(kUploadAttempts));
  m_retry.start(s * 1000);
}

} // namespace net
