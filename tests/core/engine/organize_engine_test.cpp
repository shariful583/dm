// SPDX-License-Identifier: GPL-3.0-or-later
// Existing files, category folders, duplicates and saved logins, through the download engine
// and the queue engine.
#include <chrono>
#include <fstream>
#include <random>
#include <sstream>
#include <thread>

#include <catch2/catch_test_macros.hpp>
#include <windows.h>

#include "../platform/temp_dir.hpp"
#include "content.hpp"
#include "core/engine/download.hpp"
#include "core/engine/engine.hpp"
#include "core/platform/credentials.hpp"
#include "core/platform/fs.hpp"
#include "core/platform/path.hpp"
#include "test_server.hpp"

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using namespace lanes::core;
using lanes::testserver::content_sha256;
using lanes::testserver::make_content;
using lanes::testserver::sha256_hex;
using lanes::testserver::TestServer;

namespace {

constexpr std::uint64_t kSize = 100'000;

std::string read_file(const fs::path &path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

struct Server {
  TestServer server;
  TempDir dir;
  Server() {
    server.add_file("/a.bin", {.size = kSize, .seed = 110});
    server.add_file(
        "/get",
        {.size = kSize, .seed = 111, .content_disposition = "attachment; filename=\"movie.mkv\""});
    server.start();
  }
};

// Test logins live under their own credential namespace, never touching real ones.
struct LoginNamespace {
  LoginNamespace() {
    std::random_device rd;
    ::SetEnvironmentVariableW(L"LANES_CREDENTIAL_NAMESPACE",
                              (L"LanesTest" + std::to_wstring(rd())).c_str());
  }
  ~LoginNamespace() {
    if (auto logins = platform::list_logins()) {
      for (const auto &l : *logins) {
        (void)platform::delete_login(l.host);
      }
    }
    ::SetEnvironmentVariableW(L"LANES_CREDENTIAL_NAMESPACE", nullptr);
  }
};

} // namespace

TEST_CASE("an existing file is renamed, overwritten or refused", "[engine][organize]") {
  Server s;
  REQUIRE(platform::write_file_atomic(s.dir.path() / "a.bin", "old content"));
  engine::DownloadOptions o;
  o.request.url = s.server.url("/a.bin");
  o.directory = s.dir.path();

  o.if_file_exists = engine::ExistingFile::Rename;
  const auto renamed = engine::download_file(o);
  REQUIRE(renamed);
  CHECK(renamed->path.filename() == "a (2).bin");
  CHECK(read_file(s.dir.path() / "a.bin") == "old content");

  o.if_file_exists = engine::ExistingFile::Fail;
  const auto refused = engine::download_file(o);
  REQUIRE_FALSE(refused);
  CHECK(refused.error().code == ErrorCode::AlreadyExists);

  o.if_file_exists = engine::ExistingFile::Overwrite;
  const auto replaced = engine::download_file(o);
  REQUIRE(replaced);
  CHECK(replaced->path.filename() == "a.bin");
  CHECK(sha256_hex(read_file(s.dir.path() / "a.bin")) == content_sha256(110, kSize));
}

TEST_CASE("the real file name picks the category folder", "[engine][organize]") {
  Server s;
  engine::DownloadOptions o;
  o.request.url = s.server.url("/get"); // the URL says nothing; the server says movie.mkv
  o.choose_directory = [&](const std::string &name) {
    return category::folder_for(category::default_categories(), name, s.dir.path());
  };
  const auto result = engine::download_file(o);
  REQUIRE(result);
  CHECK(result->path == s.dir.path() / "Video" / "movie.mkv");
}

TEST_CASE("a saved site login answers the server's challenge", "[engine][organize]") {
  LoginNamespace logins;
  Server s;
  engine::DownloadOptions o;
  o.request.url = s.server.url("/a.bin?basic_auth=alice:s3cret");
  o.directory = s.dir.path();
  const auto denied = engine::download_file(o);
  REQUIRE_FALSE(denied);
  CHECK(denied.error().http_status == 401);

  REQUIRE(platform::save_login({"127.0.0.1", "alice", "s3cret"}));
  const auto found = platform::find_login("127.0.0.1");
  REQUIRE(found);
  REQUIRE(*found);
  CHECK((*found)->username == "alice");
  CHECK(platform::list_logins()->size() == 1);

  const auto allowed = engine::download_file(o);
  REQUIRE(allowed);

  REQUIRE(platform::delete_login("127.0.0.1"));
  CHECK_FALSE(*platform::find_login("127.0.0.1"));
  CHECK(platform::delete_login("127.0.0.1")); // already gone: fine
}

namespace {

struct EngineFixture : Server {
  std::unique_ptr<Engine> engine;
  EngineFixture() {
    auto e = Engine::open({.database = dir.path() / "lanes.db", .tick = 20ms});
    REQUIRE(e);
    engine = std::move(*e);
    REQUIRE(engine->set_setting("download_dir", platform::path_to_utf8(dir.path() / "dl")));
  }
  NewDownload download(const std::string &path) const {
    NewDownload n;
    n.request.url = server.url(path);
    return n;
  }
  bool completed(DownloadId id, std::chrono::milliseconds timeout = 10s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      if (engine->get(id)->state == DownloadState::Completed) {
        return true;
      }
      std::this_thread::sleep_for(10ms);
    }
    return false;
  }
};

} // namespace

TEST_CASE("engine downloads go to their category folder", "[engine][organize]") {
  EngineFixture f;
  const auto id = *f.engine->add(f.download("/get"));
  REQUIRE(f.completed(id));
  const auto info = f.engine->get(id);
  CHECK(info->filename == "movie.mkv");
  CHECK(info->category == "Video");
  CHECK(info->directory == f.dir.path() / "dl" / "Video");
  CHECK(fs::exists(f.dir.path() / "dl" / "Video" / "movie.mkv"));

  REQUIRE(f.engine->set_setting("category_folders", "0"));
  const auto flat = *f.engine->add(f.download("/a.bin"));
  REQUIRE(f.completed(flat));
  CHECK(fs::exists(f.dir.path() / "dl" / "a.bin"));

  auto cats = f.engine->categories();
  cats.push_back({"Binaries", {"bin"}, "Bins"});
  REQUIRE(f.engine->set_categories(cats));
  CHECK(f.engine->categories().back().name == "Binaries");
  CHECK(f.engine->set_categories({{"General", {"x"}, ""}}).error().code ==
        ErrorCode::InvalidArgument);
}

TEST_CASE("duplicate URLs: ask, resume, rename, overwrite", "[engine][organize]") {
  EngineFixture f;
  const auto first = *f.engine->add(f.download("/a.bin"));
  REQUIRE(f.completed(first));

  const auto asked = f.engine->add(f.download("/a.bin"));
  REQUIRE_FALSE(asked);
  CHECK(asked.error().code == ErrorCode::AlreadyExists);
  CHECK(asked.error().message.find("#" + std::to_string(first)) != std::string::npos);

  auto resume = f.download("/a.bin");
  resume.if_duplicate = DuplicateAction::Resume;
  CHECK(*f.engine->add(resume) == first);
  CHECK(f.engine->list().size() == 1);

  auto rename = f.download("/a.bin");
  rename.if_duplicate = DuplicateAction::Rename;
  const auto second = *f.engine->add(rename);
  REQUIRE(f.completed(second));
  CHECK(f.engine->get(second)->filename == "a (2).bin");
  CHECK(fs::exists(f.dir.path() / "dl" / "a (2).bin"));

  auto overwrite = f.download("/a.bin");
  overwrite.if_duplicate = DuplicateAction::Overwrite;
  const auto third = *f.engine->add(overwrite);
  REQUIRE(f.completed(third));
  CHECK_FALSE(f.engine->get(first)); // replaced
  CHECK(f.engine->get(third)->filename == "a.bin");
  CHECK(sha256_hex(read_file(f.dir.path() / "dl" / "a.bin")) == content_sha256(110, kSize));
}

TEST_CASE("an engine download never silently replaces a file", "[engine][organize]") {
  EngineFixture f;
  REQUIRE(platform::create_directories(f.dir.path() / "dl"));
  REQUIRE(platform::write_file_atomic(f.dir.path() / "dl" / "a.bin", "mine"));
  REQUIRE(f.engine->set_setting("category_folders", "0"));
  const auto id = *f.engine->add(f.download("/a.bin"));
  REQUIRE(f.completed(id));
  CHECK(f.engine->get(id)->filename == "a (2).bin");
  CHECK(read_file(f.dir.path() / "dl" / "a.bin") == "mine");
}
