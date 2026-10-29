// SPDX-License-Identifier: GPL-3.0-or-later
// lanes-gui.exe: owns the engine, serves the CLI and the browser bridge over the
// pipe, and shows the Qt Quick UI.
//
//   lanes-gui.exe [--tray] [URL...]
//     --tray   start hidden in the notification area (start with Windows)
//     URL...   open the Add URL window for them (also forwarded to a running Lanes window)

#include <array>
#include <chrono>
#include <future>
#include <memory>
#include <thread>

#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QScreen>
#include <QStyleHints>
#include <QTimer>

#include "core/engine/engine.hpp"
#include "core/platform/ipc.hpp"
#include "core/platform/system.hpp"
#include "core/service/service.hpp"
#include "core/version.hpp"
#include "gui/app_controller.hpp"
#include "gui/win/shell.hpp"

namespace {

using namespace lanes;
using namespace std::chrono_literals;

struct Arguments {
  bool tray = false;
  QStringList urls;
  // Development: open a window ("add", "options", "queues", "progress:<id>"), save the focused
  // window as PNG after `screenshot_delay` ms and exit.
  QString show;
  QString screenshot;
  int screenshot_delay = 1500;
};

Arguments parse(const QStringList &args) {
  Arguments a;
  for (qsizetype i = 1; i < args.size(); ++i) {
    if (args[i] == QLatin1StringView("--tray")) {
      a.tray = true;
    } else if (args[i] == QLatin1StringView("--screenshot") && i + 1 < args.size()) {
      a.screenshot = args[++i];
    } else if (args[i] == QLatin1StringView("--screenshot-delay") && i + 1 < args.size()) {
      a.screenshot_delay = args[++i].toInt();
    } else if (args[i] == QLatin1StringView("--show") && i + 1 < args.size()) {
      a.show = args[++i];
    } else if (!args[i].startsWith(QLatin1StringView("--"))) {
      a.urls << args[i];
    }
  }
  return a;
}

enum class Ownership { Owner, HandedOff, Failed };

// Takes the per-user pipe so this process runs the engine. A running Lanes window gets our URLs
// instead; a windowless `lanes engine` is asked to stop so the GUI can take over.
Ownership take_engine(const QStringList &urls, core::platform::PipeServer &server, QString &error) {
  const std::string name = core::platform::default_pipe_name();
  for (int attempt = 0; attempt < 100; ++attempt) {
    auto created = core::platform::PipeServer::create(name);
    if (created) {
      server = std::move(*created);
      return Ownership::Owner;
    }
    if (created.error().code != core::ErrorCode::AlreadyExists) {
      error = QString::fromStdString(created.error().message);
      return Ownership::Failed;
    }
    if (attempt == 0) {
      core::service::ClientOptions options;
      options.start_engine = false;
      if (auto client = core::service::EngineClient::connect(options)) {
        std::vector<std::string> args;
        for (const QString &u : urls) {
          args.push_back(u.toStdString());
        }
        gui::win::allow_foreground_handoff();
        auto activated = client->activate(args);
        if (activated) {
          return Ownership::HandedOff;
        }
        if (activated.error().code == core::ErrorCode::Unsupported) {
          (void)client->shutdown_engine(); // the background engine; its downloads resume here
        }
      }
    }
    std::this_thread::sleep_for(100ms); // until the old owner has released the pipe
  }
  error = QObject::tr("Another Lanes engine is running and did not stop.");
  return Ownership::Failed;
}

// Tray icon, notifications and taskbar progress, driven by the controller and the model.
class Shell : public QObject {
public:
  Shell(gui::AppController &app, QQmlApplicationEngine &qml)
      : app_(app), qml_(qml), tray_(QStringLiteral("Lanes")) {
    connect(&tray_, &gui::win::TrayIcon::activated, &app_, &gui::AppController::requestMainWindow);
    connect(&tray_, &gui::win::TrayIcon::menuTriggered, this, &Shell::on_menu);
    connect(&tray_, &gui::win::TrayIcon::messageClicked, this,
            [this](qint64 id) { app_.openFolder(id); });
    connect(&app_, &gui::AppController::settingsChanged, this, &Shell::build_menu);
    connect(app_.model(), &gui::DownloadModel::statsChanged, this, &Shell::on_stats);
    connect(app_.model(), &gui::DownloadModel::stateChanged, this, &Shell::on_state);
    build_menu();
    on_stats();
    // Re-sent periodically: the taskbar button may not exist yet when progress first changes.
    taskbar_timer_.setInterval(1000);
    connect(&taskbar_timer_, &QTimer::timeout, this, &Shell::update_taskbar);
    taskbar_timer_.start();
  }

  QQuickWindow *main_window() const {
    const auto roots = qml_.rootObjects();
    return roots.isEmpty() ? nullptr : qobject_cast<QQuickWindow *>(roots.first());
  }

private:
  enum MenuId { Open = 1, AddUrl, PauseAll, ResumeAll, Exit, LimitBase = 100 };
  static constexpr std::array<std::uint64_t, 7> kLimits{0,       256 << 10, 512 << 10, 1 << 20,
                                                        2 << 20, 5 << 20,   10 << 20};

  void build_menu() {
    const std::uint64_t limit = app_.settings().value("speed_limit").toULongLong();
    std::vector<gui::win::TrayIcon::MenuItem> speeds;
    for (std::size_t i = 0; i < kLimits.size(); ++i) {
      speeds.push_back({static_cast<int>(LimitBase + i),
                        kLimits[i] == 0 ? tr("No limit") : app_.formatSpeed(double(kLimits[i])),
                        kLimits[i] == limit});
    }
    tray_.setMenu({
        {Open, tr("Open Lanes")},
        {AddUrl, tr("Add URL...")},
        {},
        {PauseAll, tr("Pause all")},
        {ResumeAll, tr("Resume all")},
        {0, tr("Speed limit"), false, true, std::move(speeds)},
        {},
        {Exit, tr("Exit")},
    });
  }

  void on_menu(int id) {
    if (id >= LimitBase && id < LimitBase + static_cast<int>(kLimits.size())) {
      app_.setSetting(QStringLiteral("speed_limit"),
                      QString::number(kLimits[static_cast<std::size_t>(id - LimitBase)]));
      return;
    }
    switch (id) {
    case Open:
      app_.requestMainWindow();
      break;
    case AddUrl:
      app_.requestAdd({});
      break;
    case PauseAll:
      app_.pauseAll();
      break;
    case ResumeAll:
      app_.resumeAll();
      break;
    case Exit:
      app_.quit();
      break;
    default:
      break;
    }
  }

  void on_stats() {
    const auto *m = app_.model();
    tray_.setTooltip(m->activeCount() > 0
                         ? tr("Lanes: %n downloading, %1", nullptr, m->activeCount())
                               .arg(app_.formatSpeed(m->totalSpeed()))
                         : tr("Lanes Download Manager"));
    update_taskbar();
  }

  void update_taskbar() {
    const auto *m = app_.model();
    taskbar_.set(main_window(), m->activeCount() > 0 ? std::max(0.0, m->overallProgress()) : -1,
                 false);
  }

  void on_state(qint64 id, const QString &, const QString &to) {
    if (!app_.settings().value("gui/notify").toBool()) {
      return;
    }
    const QVariantMap d = app_.details(id);
    const QString name = d.value("fileName").toString();
    if (to == QLatin1StringView("completed")) {
      tray_.showMessage(tr("Download complete"), name, id);
    } else if (to == QLatin1StringView("failed") || to == QLatin1StringView("needs_address")) {
      tray_.showMessage(tr("Download failed"), name + "\n" + d.value("error").toString(), id);
    }
  }

  gui::AppController &app_;
  QQmlApplicationEngine &qml_;
  gui::win::TrayIcon tray_;
  gui::win::TaskbarProgress taskbar_;
  QTimer taskbar_timer_;
};

} // namespace

int main(int argc, char *argv[]) {
  QGuiApplication::setOrganizationName(QStringLiteral("Lanes"));
  QGuiApplication::setApplicationName(QStringLiteral("Lanes"));
  QGuiApplication::setApplicationVersion(
      QString::fromUtf8(core::kVersionString.data(), core::kVersionString.size()));
  QGuiApplication app(argc, argv);
  QGuiApplication::setWindowIcon(QIcon(QStringLiteral(":/qt/qml/Lanes/resources/lanes.ico")));
  app.setQuitOnLastWindowClosed(false); // closing the window keeps Lanes in the tray
  const Arguments args = parse(QGuiApplication::arguments());

  core::platform::PipeServer server;
  QString error;
  switch (take_engine(args.urls, server, error)) {
  case Ownership::HandedOff:
    return 0;
  case Ownership::Failed:
    gui::win::error_box(QStringLiteral("Lanes"), error);
    return 1;
  case Ownership::Owner:
    break;
  }

  auto engine = core::Engine::open({.database = core::platform::app_data_dir() / "lanes.db"});
  if (!engine) {
    gui::win::error_box(QStringLiteral("Lanes"),
                        QObject::tr("Lanes could not open its database:\n%1")
                            .arg(QString::fromStdString(engine.error().message)));
    return 1;
  }

  gui::AppController controller(**engine);
  gui::AppController::setInstance(&controller);

  // The CLI and the browser bridge talk to this engine; "activate" comes from a second launch.
  core::service::EngineService service(**engine, [&controller] {
    QMetaObject::invokeMethod(&controller, &gui::AppController::quit, Qt::QueuedConnection);
  });
  service.set_on_activate([&controller](std::vector<std::string> received) {
    QStringList urls;
    for (const auto &u : received) {
      urls << QString::fromStdString(u);
    }
    QMetaObject::invokeMethod(
        &controller,
        [&controller, urls] {
          controller.requestMainWindow();
          if (!urls.isEmpty()) {
            controller.requestAdd(urls);
          }
        },
        Qt::QueuedConnection);
  });
  // Browser messages (from lanes-bridge) are answered on the GUI thread. The pipe thread waits a
  // few seconds at most, so shutting down never deadlocks on it.
  service.set_on_browser([&controller](std::string message) -> core::Result<std::string> {
    auto reply = std::make_shared<std::promise<QString>>();
    auto answered = reply->get_future();
    QMetaObject::invokeMethod(
        &controller,
        [&controller, reply, text = QString::fromStdString(message)] {
          reply->set_value(controller.handleBrowserMessage(text));
        },
        Qt::QueuedConnection);
    if (answered.wait_for(5s) != std::future_status::ready) {
      return core::make_error(core::ErrorCode::Timeout, "the Lanes window did not answer");
    }
    return answered.get().toStdString();
  });
  std::jthread serving([&server, &service](std::stop_token stop) {
    core::service::serve(std::move(server), service, stop);
  });

  QQuickStyle::setStyle(QStringLiteral("FluentWinUI3"));
  QQmlApplicationEngine qml;
  qml.setInitialProperties({{"startHidden", args.tray}});
  qml.loadFromModule("Lanes", "Main");
  if (qml.rootObjects().isEmpty()) {
    gui::win::error_box(QStringLiteral("Lanes"), QObject::tr("The Lanes window could not load."));
    return 1;
  }
  Shell shell(controller, qml);
  if (args.screenshot.isEmpty()) {
    controller.checkExtractorUpdate(); // yt-dlp: at most once a day, in the background
  }
  // exit(), not quit(): quit() first asks the windows to close, and the main window refuses
  // (it hides to the tray instead).
  QObject::connect(&controller, &gui::AppController::quitRequested, &app,
                   [] { QCoreApplication::exit(0); });

  // Title bar colors and Mica follow the Windows theme.
  const auto restyle = [&shell] {
    if (QQuickWindow *w = shell.main_window()) {
      const bool dark = QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
      w->setProperty("mica", gui::win::style_window(w, dark, true));
    }
  };
  restyle();
  QObject::connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, &app, restyle);

  if (!args.urls.isEmpty()) {
    controller.requestAdd(args.urls);
  }
  if (!args.show.isEmpty() || !args.screenshot.isEmpty()) {
    gui::win::bring_to_front(shell.main_window());
  }
  if (!args.show.isEmpty()) {
    QMetaObject::invokeMethod(shell.main_window(), "devShow", Q_ARG(QVariant, args.show));
  }
  if (!args.screenshot.isEmpty()) {
    QTimer::singleShot(args.screenshot_delay, &app, [&shell, &args] {
      QWindow *w = QGuiApplication::focusWindow();
      if (w == nullptr) {
        w = shell.main_window();
      }
      if (w != nullptr) {
        // From the screen, so the Windows-drawn parts (Mica, title bar) are in the picture.
        if (QScreen *screen = w->screen()) {
          const QRect r = w->frameGeometry();
          screen->grabWindow(0, r.x(), r.y(), r.width(), r.height()).save(args.screenshot);
        }
      }
      QCoreApplication::exit(0);
    });
  }

  const int code = QGuiApplication::exec();
  serving.request_stop();
  serving.join();
  (*engine)->shutdown(); // running downloads stop and resume on the next start
  return code;
}
