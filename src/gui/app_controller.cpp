// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/app_controller.hpp"

#include <QClipboard>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QQmlEngine>

#include "core/batch/batch.hpp"
#include "core/category/category.hpp"
#include "core/net/probe.hpp"
#include "core/platform/credentials.hpp"
#include "core/platform/path.hpp"
#include "core/version.hpp"
#include "gui/format.hpp"

#ifdef _WIN32
#include "gui/win/shell.hpp"
#endif

namespace lanes::gui {

namespace {

AppController *g_instance = nullptr;

std::string utf8(const QString &s) { return s.toStdString(); }
QString qstr(const std::string &s) { return QString::fromStdString(s); }

QString native(const std::filesystem::path &p) {
  return QDir::toNativeSeparators(qstr(core::platform::path_to_utf8(p)));
}

std::filesystem::path to_path(const QString &s) {
  return core::platform::path_from_utf8(utf8(QDir::fromNativeSeparators(s)));
}

QString error_text(const core::Error &e) { return qstr(e.message); }

template <typename T> QString status(const core::Result<T> &r) {
  return r ? QString() : error_text(r.error());
}

// Request fields shared by probe() and add().
core::net::RequestOptions request_from(const QString &url, const QVariantMap &m) {
  core::net::RequestOptions r;
  r.url = utf8(url.trimmed());
  r.referer = utf8(m.value("referer").toString().trimmed());
  r.cookies = utf8(m.value("cookies").toString().trimmed());
  r.user_agent = utf8(m.value("userAgent").toString().trimmed());
  const QString user = m.value("username").toString();
  if (!user.isEmpty()) {
    r.auth = core::net::Credentials{utf8(user), utf8(m.value("password").toString())};
  }
  for (const QString &line : m.value("headers").toString().split('\n', Qt::SkipEmptyParts)) {
    const qsizetype colon = line.indexOf(':');
    if (colon > 0) {
      r.headers.emplace_back(utf8(line.left(colon).trimmed()), utf8(line.mid(colon + 1).trimmed()));
    }
  }
  return r;
}

} // namespace

AppController::AppController(core::Engine &engine, QObject *parent)
    : QObject(parent), engine_(engine), model_(engine), filter_(&model_),
      prefs_(QStringLiteral("Lanes"), QStringLiteral("Lanes")) {
  pool_.setMaxThreadCount(4);
}

AppController::~AppController() {
  pool_.clear();
  pool_.waitForDone(); // probes end within their connect timeout
  if (g_instance == this) {
    g_instance = nullptr;
  }
}

void AppController::setInstance(AppController *instance) { g_instance = instance; }

AppController *AppController::create(QQmlEngine *, QJSEngine *) {
  QQmlEngine::setObjectOwnership(g_instance, QQmlEngine::CppOwnership);
  return g_instance;
}

QVariantList AppController::categories() const {
  QVariantList out;
  for (const auto &c : engine_.categories()) {
    QStringList exts;
    for (const auto &e : c.extensions) {
      exts << qstr(e);
    }
    out.push_back(QVariantMap{{"name", qstr(c.name)},
                              {"extensions", exts.join(QStringLiteral(", "))},
                              {"folder", QDir::toNativeSeparators(qstr(c.folder))}});
  }
  return out;
}

QVariantList AppController::queues() const {
  QVariantList out;
  for (const auto &q : engine_.queues()) {
    out.push_back(QVariantMap{
        {"name", qstr(q.name)},
        {"maxActive", q.max_active},
        {"enabled", q.enabled},
        {"startTime", q.start_time ? qstr(core::queue::to_string(*q.start_time)) : QString()},
        {"stopTime", q.stop_time ? qstr(core::queue::to_string(*q.stop_time)) : QString()},
        {"days", static_cast<int>(q.days)},
    });
  }
  return out;
}

QVariantMap AppController::settings() const {
  QVariantMap out;
  for (const auto &[key, value] : engine_.settings()) {
    out[qstr(key)] = qstr(value);
  }
  out["download_dir"] = QDir::toNativeSeparators(out.value("download_dir").toString());
  for (const char *key :
       {"gui/minimizeToTray", "gui/showProgress", "gui/showComplete", "gui/notify"}) {
    out[QString::fromLatin1(key)] = prefs_.value(QString::fromLatin1(key), true).toBool();
  }
  return out;
}

QString AppController::version() const { return qstr(std::string(core::kVersionString)); }

QString AppController::iconFont() const {
  static const QString font =
      QFontDatabase::families().contains(QStringLiteral("Segoe Fluent Icons"))
          ? QStringLiteral("Segoe Fluent Icons")
          : QStringLiteral("Segoe MDL2 Assets");
  return font;
}

// --- downloads ---

void AppController::probe(const QString &url, const QVariantMap &request) {
  core::net::RequestOptions r = request_from(url, request);
  r.connect_timeout = std::chrono::seconds(15);
  pool_.start([this, url, r = std::move(r)] {
    auto probed = core::net::probe(r);
    QVariantMap result;
    if (probed) {
      result["ok"] = true;
      result["fileName"] = qstr(probed->filename);
      result["size"] = probed->size ? static_cast<double>(*probed->size) : -1.0;
      result["resumable"] = probed->accepts_ranges && probed->size.has_value();
      result["type"] = qstr(probed->content_type);
      result["finalUrl"] = qstr(core::net::strip_credentials(probed->final_url));
    } else {
      result["ok"] = false;
      result["error"] = error_text(probed.error());
    }
    // The pool is drained before the controller is destroyed, so `this` is still valid here.
    QMetaObject::invokeMethod(
        this,
        [this, url, result]() mutable {
          if (result.value("ok").toBool()) {
            const QString name = result.value("fileName").toString();
            result["category"] = categoryOf(name);
            result["folder"] = folderFor(name);
          }
          emit probeFinished(url, result);
        },
        Qt::QueuedConnection);
  });
}

QVariantMap AppController::add(const QVariantMap &d) {
  core::NewDownload n;
  n.request = request_from(d.value("url").toString(), d);
  n.filename = utf8(d.value("fileName").toString().trimmed());
  const QString folder = d.value("folder").toString().trimmed();
  if (!folder.isEmpty()) {
    n.directory = to_path(folder);
  }
  n.queue = utf8(d.value("queue").toString());
  if (const QDateTime at = d.value("startAt").toDateTime(); at.isValid()) {
    n.start_at = at.toSecsSinceEpoch();
  }
  n.connections = d.value("connections").toInt();
  n.speed_limit = static_cast<std::uint64_t>(std::max(0.0, d.value("speedLimit").toDouble()));
  if (const QString sum = d.value("checksum").toString().trimmed(); !sum.isEmpty()) {
    const qsizetype colon = sum.indexOf(':');
    const bool md5 = colon > 0 && sum.left(colon).compare("md5", Qt::CaseInsensitive) == 0;
    auto c = core::verify::make_checksum(md5 ? core::verify::HashAlgorithm::Md5
                                             : core::verify::HashAlgorithm::Sha256,
                                         utf8(colon > 0 ? sum.mid(colon + 1).trimmed() : sum));
    if (!c) {
      return {{"ok", false}, {"error", error_text(c.error())}};
    }
    n.checksum = *c;
  }
  const QString dup = d.value("ifDuplicate").toString();
  n.if_duplicate = dup == "rename"      ? core::DuplicateAction::Rename
                   : dup == "overwrite" ? core::DuplicateAction::Overwrite
                   : dup == "resume"    ? core::DuplicateAction::Resume
                                        : core::DuplicateAction::Ask;
  n.overwrite_file = d.value("overwriteFile").toBool();
  auto id = engine_.add(std::move(n));
  if (!id) {
    return {{"ok", false},
            {"error", error_text(id.error())},
            {"duplicate", id.error().code == core::ErrorCode::AlreadyExists}};
  }
  model_.refresh(); // the row is there when the caller opens its window
  return {{"ok", true}, {"id", static_cast<qint64>(*id)}};
}

QString AppController::pause(qint64 id) { return status(engine_.pause(id)); }

QString AppController::resume(qint64 id, const QString &newUrl) {
  return status(engine_.resume(id, newUrl.trimmed().isEmpty()
                                       ? std::nullopt
                                       : std::optional<std::string>(utf8(newUrl.trimmed()))));
}

QString AppController::remove(qint64 id, bool deleteFiles) {
  return status(engine_.remove(id, deleteFiles));
}

void AppController::pauseAll() { engine_.pause_all(); }
void AppController::resumeAll() { engine_.resume_all(); }

QString AppController::setSpeedLimit(qint64 id, double bytesPerSecond) {
  return status(
      engine_.set_speed_limit(id, static_cast<std::uint64_t>(std::max(0.0, bytesPerSecond))));
}

QVariantMap AppController::details(qint64 id) const {
  const auto info = engine_.get(id);
  if (!info) {
    return {{"exists", false}};
  }
  QVariantList segments;
  bool ranged = false;
  for (const auto &s : info->segments) {
    segments.push_back(QVariantMap{{"start", static_cast<double>(s.start)},
                                   {"end", s.end ? static_cast<double>(*s.end) : -1.0},
                                   {"received", static_cast<double>(s.received)}});
    ranged = ranged || (s.end.has_value() && s.start > 0);
  }
  const bool done = info->state == core::DownloadState::Completed;
  const double size = info->size ? static_cast<double>(*info->size) : -1.0;
  double left = -1;
  if (info->state == core::DownloadState::Running && info->size && info->bytes_per_second > 0) {
    left = static_cast<double>(*info->size - std::min(*info->size, info->downloaded)) /
           info->bytes_per_second;
  }
  return {
      {"exists", true},
      {"id", static_cast<qint64>(info->id)},
      {"fileName", qstr(info->filename)},
      {"url", qstr(info->url)},
      {"state", QString::fromUtf8(core::to_string(info->state))},
      {"size", size},
      {"downloaded", static_cast<double>(done && info->size ? *info->size : info->downloaded)},
      {"progress", done ? 1.0 : (size > 0 ? static_cast<double>(info->downloaded) / size : -1.0)},
      {"speed", info->state == core::DownloadState::Running ? info->bytes_per_second : 0.0},
      {"timeLeft", left},
      {"error", qstr(info->error)},
      {"folder", info->directory.empty() ? QString() : native(info->directory)},
      {"filePath", info->directory.empty() || info->filename.empty()
                       ? QString()
                       : native(info->directory / core::platform::path_from_utf8(info->filename))},
      {"category", qstr(info->category)},
      {"queue", qstr(info->queue)},
      {"connections", info->connections},
      {"speedLimit", static_cast<double>(info->speed_limit)},
      {"segments", segments},
      {"resumable", ranged || segments.size() > 1},
  };
}

void AppController::openFile(qint64 id) {
  const QString path = details(id).value("filePath").toString();
  if (!path.isEmpty()) {
    openPath(path);
  }
}

void AppController::openFolder(qint64 id) {
  const QVariantMap d = details(id);
  QString path = d.value("filePath").toString();
  if (path.isEmpty()) {
    path = d.value("folder").toString();
  }
  if (path.isEmpty()) {
    path = settings().value("download_dir").toString();
  }
#ifdef _WIN32
  win::show_in_folder(path);
#endif
}

void AppController::openPath(const QString &path) {
#ifdef _WIN32
  win::open_with_default_app(path);
#else
  Q_UNUSED(path)
#endif
}

void AppController::copyText(const QString &text) { QGuiApplication::clipboard()->setText(text); }

QString AppController::clipboardText() const { return QGuiApplication::clipboard()->text(); }

QVariantMap AppController::expandUrls(const QString &text) const {
  QStringList urls;
  for (const std::string &line : core::batch::parse_url_list(utf8(text))) {
    if (core::batch::has_pattern(line)) {
      auto expanded = core::batch::expand_pattern(line);
      if (!expanded) {
        return {{"urls", QStringList()}, {"error", error_text(expanded.error())}};
      }
      for (const auto &u : *expanded) {
        urls << qstr(u);
      }
    } else {
      urls << qstr(line);
    }
  }
  return {{"urls", urls}, {"error", QString()}};
}

bool AppController::isUrl(const QString &text) const {
  return core::net::validate_url(utf8(text.trimmed())).has_value();
}

QString AppController::folderFor(const QString &fileName) const {
  const QVariantMap s = settings();
  const std::filesystem::path base = to_path(s.value("download_dir").toString());
  if (s.value("category_folders").toString() != "1") {
    return native(base);
  }
  return native(core::category::folder_for(engine_.categories(), utf8(fileName), base));
}

QString AppController::categoryOf(const QString &fileName) const {
  return qstr(core::category::category_name(engine_.categories(), utf8(fileName)));
}

// --- settings, queues, categories, logins ---

QString AppController::setSetting(const QString &key, const QVariant &value) {
  if (key.startsWith(QStringLiteral("gui/"))) {
    prefs_.setValue(key, value.toBool());
    emit settingsChanged();
    return {};
  }
  QString text = value.toString();
  if (key == QStringLiteral("download_dir")) {
    text = QDir::fromNativeSeparators(text);
  }
  const QString error = status(engine_.set_setting(utf8(key), utf8(text)));
  emit settingsChanged();
  return error;
}

QString AppController::saveQueue(const QVariantMap &m) {
  core::queue::QueueSettings q;
  q.name = utf8(m.value("name").toString().trimmed());
  if (q.name.empty()) {
    return tr("A queue needs a name.");
  }
  for (const auto &existing : engine_.queues()) {
    if (existing.name == q.name) {
      q = existing; // keep what the map leaves out
    }
  }
  if (m.contains("maxActive")) {
    q.max_active = std::clamp(m.value("maxActive").toInt(), 1, 64);
  }
  if (m.contains("days")) {
    q.days = static_cast<std::uint8_t>(m.value("days").toInt() & core::queue::kEveryDay);
  }
  for (const auto &[field, slot] :
       {std::pair{"startTime", &q.start_time}, std::pair{"stopTime", &q.stop_time}}) {
    if (!m.contains(field)) {
      continue;
    }
    const QString text = m.value(field).toString().trimmed();
    if (text.isEmpty()) {
      slot->reset();
    } else if (auto t = core::queue::parse_time_of_day(utf8(text))) {
      *slot = *t;
    } else {
      return error_text(t.error());
    }
  }
  const QString error = status(engine_.save_queue(q));
  emit queuesChanged();
  return error;
}

QString AppController::startQueue(const QString &name) {
  const QString error = status(engine_.start_queue(utf8(name)));
  emit queuesChanged();
  return error;
}

QString AppController::stopQueue(const QString &name) {
  const QString error = status(engine_.stop_queue(utf8(name)));
  emit queuesChanged();
  return error;
}

QString AppController::setCategories(const QVariantList &list) {
  std::vector<core::category::Category> cats;
  for (const QVariant &v : list) {
    const QVariantMap m = v.toMap();
    core::category::Category c;
    c.name = utf8(m.value("name").toString().trimmed());
    if (c.name.empty()) {
      continue;
    }
    c.extensions = core::category::parse_extensions(utf8(m.value("extensions").toString()));
    c.folder = utf8(QDir::fromNativeSeparators(m.value("folder").toString().trimmed()));
    cats.push_back(std::move(c));
  }
  const QString error = status(engine_.set_categories(std::move(cats)));
  emit categoriesChanged();
  return error;
}

QVariantList AppController::defaultCategories() const {
  QVariantList out;
  for (const auto &c : core::category::default_categories()) {
    QStringList exts;
    for (const auto &e : c.extensions) {
      exts << qstr(e);
    }
    out.push_back(QVariantMap{{"name", qstr(c.name)},
                              {"extensions", exts.join(QStringLiteral(", "))},
                              {"folder", qstr(c.folder)}});
  }
  return out;
}

QVariantList AppController::logins() const {
  QVariantList out;
  if (auto list = core::platform::list_logins()) {
    for (const auto &l : *list) {
      out.push_back(QVariantMap{{"host", qstr(l.host)}, {"username", qstr(l.username)}});
    }
  }
  return out;
}

QString AppController::saveLogin(const QString &host, const QString &username,
                                 const QString &password) {
  QString h = host.trimmed().toLower();
  if (isUrl(h)) {
    h = qstr(core::net::host_of(utf8(h))); // a pasted URL: keep its host
  }
  if (h.isEmpty() || username.isEmpty()) {
    return tr("Enter a host and a user name.");
  }
  return status(core::platform::save_login({utf8(h), utf8(username), utf8(password)}));
}

QString AppController::removeLogin(const QString &host) {
  return status(core::platform::delete_login(utf8(host)));
}

// --- formatting ---

QString AppController::formatBytes(double bytes) const {
  return format_bytes(static_cast<std::uint64_t>(std::max(0.0, bytes)));
}
QString AppController::formatSpeed(double bytesPerSecond) const {
  return format_speed(bytesPerSecond);
}
QString AppController::formatDuration(double seconds) const { return format_duration(seconds); }

double AppController::parseSize(const QString &text) const {
  const auto bytes = parse_size(text);
  return bytes ? static_cast<double>(*bytes) : -1.0;
}

QString AppController::licenseText(const QString &name) const {
  QFile file(QStringLiteral(":/qt/qml/Lanes/") + name);
  if (!file.open(QIODevice::ReadOnly)) {
    return {};
  }
  return QString::fromUtf8(file.readAll());
}

} // namespace lanes::gui
