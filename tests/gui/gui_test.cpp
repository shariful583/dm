// SPDX-License-Identifier: GPL-3.0-or-later
// GUI tests: the list model against a real engine, and every QML window loading without
// warnings. Runs headless (QT_QPA_PLATFORM=offscreen).
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QQuickWindow>
#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include "../core/platform/temp_dir.hpp"
#include "gui/app_controller.hpp"
#include "gui/format.hpp"
#include "test_server.hpp"

using namespace std::chrono_literals;
using namespace lanes;

namespace {

std::mutex g_messages_mutex;
QStringList g_messages; // QML warnings and errors

void collect(QtMsgType type, const QMessageLogContext &, const QString &text) {
  if (type == QtFatalMsg) {
    std::fputs(qPrintable(text + '\n'), stderr);
  }
  // Problems in Lanes' QML only: not notices of the headless platform (fonts, raise()), nor Qt's
  // own Fluent ProgressBar, which warns about animation durations while it has no width.
  if (type != QtDebugMsg && type != QtInfoMsg &&
      text.contains(QLatin1StringView("qrc:/qt/qml/Lanes/"))) {
    std::lock_guard lock(g_messages_mutex);
    g_messages << text;
  }
}

struct Fixture {
  testserver::TestServer server;
  TempDir dir;
  std::unique_ptr<core::Engine> engine;
  std::unique_ptr<gui::AppController> app;

  Fixture() {
    server.add_file("/a.bin", {.size = 256 << 10, .seed = 7});
    server.start();
    engine = std::move(*core::Engine::open({.database = dir.path() / "lanes.db", .tick = 20ms}));
    REQUIRE(engine->set_setting("download_dir", (dir.path() / "dl").string()));
    app = std::make_unique<gui::AppController>(*engine);
    gui::AppController::setInstance(app.get());
  }
  ~Fixture() {
    app.reset();
    engine->shutdown();
  }
  // Runs the Qt event loop until `done` or the timeout.
  template <typename Pred> bool wait(Pred done, std::chrono::milliseconds timeout = 10s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
      if (done()) {
        return true;
      }
      std::this_thread::sleep_for(10ms);
    }
    return false;
  }
};

} // namespace

TEST_CASE("the list model follows the engine", "[gui]") {
  Fixture f;
  QStringList transitions;
  QObject::connect(
      f.app->model(), &gui::DownloadModel::stateChanged,
      [&](qint64, const QString &from, const QString &to) { transitions << from + ">" + to; });
  const QVariantMap added = f.app->add({{"url", QString::fromStdString(f.server.url("/a.bin"))}});
  REQUIRE(added.value("ok").toBool());
  auto *model = f.app->model();
  REQUIRE(model->rowCount() == 1); // add() refreshes at once
  const QModelIndex row = model->index(0);
  REQUIRE(
      f.wait([&] { return row.data(gui::DownloadModel::StateRole).toString() == "completed"; }));
  CHECK(row.data(gui::DownloadModel::FileNameRole).toString() == "a.bin");
  CHECK(row.data(gui::DownloadModel::SizeRole).toDouble() == 256 << 10);
  CHECK(row.data(gui::DownloadModel::ProgressRole).toDouble() == 1.0);
  CHECK(transitions.filter(">completed").size() == 1); // "running" may fall between two refreshes
  CHECK(model->counts().value("finished").toInt() == 1);

  auto *view = f.app->downloads();
  view->setView("unfinished");
  CHECK(view->count() == 0);
  view->setView("finished");
  view->setSearch("A.BIN");
  CHECK(view->count() == 1);
  view->setSearch("zzz");
  CHECK(view->count() == 0);

  // A duplicate is reported, not silently added.
  const QVariantMap again = f.app->add({{"url", QString::fromStdString(f.server.url("/a.bin"))}});
  CHECK_FALSE(again.value("ok").toBool());
  CHECK(again.value("duplicate").toBool());

  REQUIRE(f.app->remove(added.value("id").toLongLong(), true).isEmpty());
  REQUIRE(f.wait([&] { return model->rowCount() == 0; }));
}

TEST_CASE("every window loads without QML warnings", "[gui]") {
  Fixture f;
  const QVariantMap added = f.app->add({{"url", QString::fromStdString(f.server.url("/a.bin"))}});
  REQUIRE(added.value("ok").toBool());
  {
    std::lock_guard lock(g_messages_mutex);
    g_messages.clear();
  }
  {
    QQmlApplicationEngine qml;
    qml.setInitialProperties({{"startHidden", true}});
    qml.loadFromModule("Lanes", "Main");
    REQUIRE(qml.rootObjects().size() == 1);
    QObject *root = qml.rootObjects().first();
    for (const char *what : {"add:https://example.invalid/file.zip", "options", "queues"}) {
      QMetaObject::invokeMethod(root, "devShow", Q_ARG(QVariant, QString::fromLatin1(what)));
      f.wait([] { return false; }, 300ms);
    }
    QMetaObject::invokeMethod(root, "devShow",
                              Q_ARG(QVariant, "progress:" + added.value("id").toString()));
    f.wait([] { return false; }, 600ms);
  }
  std::lock_guard lock(g_messages_mutex);
  INFO(g_messages.join("\n").toStdString());
  CHECK(g_messages.isEmpty());
}

TEST_CASE("sizes and durations are formatted like the CLI", "[gui]") {
  CHECK(gui::format_bytes(512) == "512 B");
  CHECK(gui::format_bytes(1536) == "1.5 KiB");
  CHECK(gui::format_speed(2.0 * 1024 * 1024) == "2.0 MiB/s");
  CHECK(gui::format_duration(125) == "2:05");
  CHECK(gui::format_duration(-1).isEmpty());
  CHECK(gui::parse_size("2M") == 2u << 20);
  CHECK(gui::parse_size("1.5 MiB") == 3u << 19);
  CHECK(gui::parse_size("800k/s") == 800u << 10);
  CHECK_FALSE(gui::parse_size("fast").has_value());
}

int main(int argc, char *argv[]) {
  if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
  }
  qInstallMessageHandler(collect);
  QGuiApplication app(argc, argv);
  QQuickStyle::setStyle(QStringLiteral("FluentWinUI3"));
  return Catch::Session().run(argc, argv);
}
