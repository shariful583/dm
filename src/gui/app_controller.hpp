// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <optional>

#include <QDateTime>
#include <QObject>
#include <QSettings>
#include <QThreadPool>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include "core/engine/engine.hpp"
#include "gui/download_model.hpp"

class QQmlEngine;
class QJSEngine;

namespace lanes::gui {

// What QML calls: the engine's actions, settings, queues, categories and site logins, plus the
// requests that open windows (tray, second launch, notifications). Methods that can fail return
// an error message, "" on success. GUI thread only; probes run on a worker pool.
//
// GUI-only preferences live in QSettings (HKCU\Software\<organization>\<application>: Lanes\Lanes;
// tests set their own names) under "gui/":
// gui/minimizeToTray, gui/showProgress, gui/showComplete, gui/notify, gui/confirmCaptured (show
// the Add window for downloads the browser hands over; all default true).
class AppController : public QObject {
  Q_OBJECT
  QML_NAMED_ELEMENT(App)
  QML_SINGLETON
  Q_PROPERTY(DownloadModel *model READ model CONSTANT)
  Q_PROPERTY(DownloadFilterModel *downloads READ downloads CONSTANT)
  // [{name, extensions: "mp4, mkv", folder}]
  Q_PROPERTY(QVariantList categories READ categories NOTIFY categoriesChanged)
  // [{name, maxActive, enabled, startTime: "07:00" or "", stopTime, days: bit 0 = Monday}]
  Q_PROPERTY(QVariantList queues READ queues NOTIFY queuesChanged)
  // Engine settings (download_dir, connections, ...) and the gui/ preferences.
  Q_PROPERTY(QVariantMap settings READ settings NOTIFY settingsChanged)
  Q_PROPERTY(QString version READ version CONSTANT)
  Q_PROPERTY(QString iconFont READ iconFont CONSTANT) // Segoe Fluent Icons or MDL2 Assets
  // What to do once every download has finished: "nothing", "exit", "sleep", "hibernate",
  // "shutdown". One-shot: back to "nothing" after it fired (whenDoneReady).
  Q_PROPERTY(QString whenDone READ whenDone WRITE setWhenDone NOTIFY whenDoneChanged)
  // Lanes starts (in the tray) when the user signs in to Windows (HKCU ...\CurrentVersion\Run).
  Q_PROPERTY(bool startWithWindows READ startWithWindows WRITE setStartWithWindows NOTIFY
                 startWithWindowsChanged)
  Q_PROPERTY(bool keepingAwake READ keepingAwake NOTIFY keepingAwakeChanged)

public:
  explicit AppController(core::Engine &engine, QObject *parent = nullptr);
  ~AppController() override;

  // The QML singleton is the instance made in main() (or by a test).
  static void setInstance(AppController *instance);
  static AppController *create(QQmlEngine *, QJSEngine *);

  DownloadModel *model() { return &model_; }
  DownloadFilterModel *downloads() { return &filter_; }
  QVariantList categories() const;
  QVariantList queues() const;
  QVariantMap settings() const;
  QString version() const;
  QString iconFont() const;
  QString whenDone() const { return when_done_; }
  void setWhenDone(const QString &action);
  bool startWithWindows() const;
  void setStartWithWindows(bool on);
  bool keepingAwake() const;

  // Carries out a "when done" action after the countdown: "exit" quits Lanes; the others ask
  // Windows (Lanes then exits too for "shutdown"). Returns an error message or "".
  Q_INVOKABLE QString runWhenDone(const QString &action);

  // --- downloads ---
  // Checks `url` in the background; probeFinished(url, {ok, error, fileName, size (-1 unknown),
  // resumable, type, finalUrl, category, folder}). `request`: referer, cookies, userAgent,
  // username, password, headers ("Name: value" lines).
  Q_INVOKABLE void probe(const QString &url, const QVariantMap &request);
  // `download`: url, request fields as for probe, fileName, folder, queue ("" = start now),
  // startAt (QDateTime), connections, speedLimit (bytes/s), checksum ("sha256:<hex>"),
  // ifDuplicate ("ask", "rename", "overwrite", "resume"), overwriteFile.
  // → {ok, id} or {ok: false, error, duplicate (bool)}.
  Q_INVOKABLE QVariantMap add(const QVariantMap &download);
  Q_INVOKABLE QString pause(qint64 id);
  Q_INVOKABLE QString resume(qint64 id, const QString &newUrl = {});
  Q_INVOKABLE QString remove(qint64 id, bool deleteFiles);
  Q_INVOKABLE void pauseAll();
  Q_INVOKABLE void resumeAll();
  Q_INVOKABLE QString setSpeedLimit(qint64 id, double bytesPerSecond);
  // Everything about one download, with its connections: {exists, id, fileName, url, state,
  // size, downloaded, progress, speed, timeLeft, error, folder, filePath, category, queue,
  // connections, speedLimit, segments: [{start, end, received}], resumable}.
  Q_INVOKABLE QVariantMap details(qint64 id) const;
  Q_INVOKABLE void openFile(qint64 id);
  Q_INVOKABLE void openFolder(qint64 id);
  Q_INVOKABLE void openPath(const QString &path);
  Q_INVOKABLE void copyText(const QString &text);
  Q_INVOKABLE QString clipboardText() const;
  // URL list (one per line, '#' comments) and batch patterns ("img[001-120].jpg") → {urls, error}.
  Q_INVOKABLE QVariantMap expandUrls(const QString &text) const;
  Q_INVOKABLE bool isUrl(const QString &text) const;
  // Where a file of this name is saved by default, and its category.
  Q_INVOKABLE QString folderFor(const QString &fileName) const;
  Q_INVOKABLE QString categoryOf(const QString &fileName) const;

  // --- settings, queues, categories, logins ---
  Q_INVOKABLE QString setSetting(const QString &key, const QVariant &value);
  Q_INVOKABLE QString saveQueue(const QVariantMap &queue);
  Q_INVOKABLE QString startQueue(const QString &name);
  Q_INVOKABLE QString stopQueue(const QString &name);
  Q_INVOKABLE QString setCategories(const QVariantList &categories);
  Q_INVOKABLE QVariantList defaultCategories() const;
  Q_INVOKABLE QVariantList logins() const; // [{host, username}]
  Q_INVOKABLE QString saveLogin(const QString &host, const QString &username,
                                const QString &password);
  Q_INVOKABLE QString removeLogin(const QString &host);

  // --- video sites ---
  // Reads a video page with yt-dlp in the background; videoInfoReady(url, {ok, error, title,
  // uploader, duration, thumbnail, isPlaylist, entries: [{url, title, duration}], choices:
  // [{key, label, detail}], subtitles: [language]}). `request`: cookies, userAgent, referer.
  Q_INVOKABLE void extractVideo(const QString &url, const QVariantMap &request);
  // Fetches a new yt-dlp now (sites change often); extractorUpdated(ok, message).
  Q_INVOKABLE void updateExtractor();
  Q_INVOKABLE void requestVideo(const QString &url, const QVariantMap &request = {}) {
    emit videoPageRequested(url, request);
  }
  // At most daily, in the background: keeps yt-dlp current without asking.
  void checkExtractorUpdate();

  // --- browser extension ---
  // Answers a message the bridge validated (JSON): ping, open, capture_download (opens the Add
  // window with the browser's cookies, referrer, user agent and POST body, or continues a
  // download waiting for a new address), capture_media (Add window with the links), video_page
  // (not supported until yt-dlp is in). Returns the reply JSON: {"result": "accepted" |
  // "rejected" | "error", "message"}.
  Q_INVOKABLE QString handleBrowserMessage(const QString &message);
  // IDM's automatic "Refresh download address": opens the download's page in the browser; the
  // next captured download with the same file name (and size, if known) within 10 minutes
  // continues this download instead of starting a new one. Returns an error message or "".
  Q_INVOKABLE QString startAutoRefresh(qint64 id);

  // --- formatting (same units as the CLI) ---
  Q_INVOKABLE QString formatBytes(double bytes) const;
  Q_INVOKABLE QString formatSpeed(double bytesPerSecond) const;
  Q_INVOKABLE QString formatDuration(double seconds) const;
  Q_INVOKABLE double parseSize(const QString &text) const;    // bytes, -1 if invalid
  Q_INVOKABLE QString licenseText(const QString &name) const; // "LICENSE", "THIRD_PARTY_NOTICES.md"

  // Asks the UI to open windows (tray menu, second launch, notification clicks).
  Q_INVOKABLE void requestMainWindow() { emit showMainWindow(); }
  // `request`: fields for the Add window (referer, cookies, userAgent, method, body, fileName).
  Q_INVOKABLE void requestAdd(const QStringList &urls, const QVariantMap &request = {}) {
    emit addUrlsRequested(urls, request);
  }
  Q_INVOKABLE void requestProgress(qint64 id) { emit progressWindowRequested(id); }
  Q_INVOKABLE void quit() { emit quitRequested(); }

signals:
  void probeFinished(const QString &url, const QVariantMap &result);
  void categoriesChanged();
  void queuesChanged();
  void settingsChanged();
  void showMainWindow();
  void addUrlsRequested(const QStringList &urls, const QVariantMap &request);
  void progressWindowRequested(qint64 id);
  void videoPageRequested(const QString &url, const QVariantMap &request);
  void videoInfoReady(const QString &url, const QVariantMap &info);
  void extractorUpdated(bool ok, const QString &message);
  void whenDoneChanged();
  // Everything finished and an action was chosen: the UI counts down, then runWhenDone().
  void whenDoneReady(const QString &action);
  void startWithWindowsChanged();
  void keepingAwakeChanged();
  void quitRequested();

private:
  core::Engine &engine_;
  DownloadModel model_;
  DownloadFilterModel filter_;
  QThreadPool pool_; // probes
  mutable QSettings prefs_;

  struct PendingRefresh {
    qint64 id = 0;
    QString filename;
    double size = -1; // -1 = unknown
    QDateTime deadline;
  };
  std::optional<PendingRefresh> pending_refresh_;

  void on_stats();
  void on_clipboard();
  QString when_done_ = QStringLiteral("nothing");
  bool had_active_ = false;
  QTimer done_check_; // the engine may become idle between two list refreshes
  bool keeping_awake_ = false;
  QString last_clipboard_;
};

} // namespace lanes::gui
