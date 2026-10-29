// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include "../platform/temp_dir.hpp"
#include "core/store/store.hpp"

using namespace lanes::core;
using namespace lanes::core::store;

TEST_CASE("downloads round-trip through the database", "[store]") {
  TempDir dir;
  const auto path = dir.path() / "lanes.db";
  DownloadId id = 0;
  {
    auto s = Store::open(path);
    REQUIRE(s);
    DownloadRecord r;
    r.url = "https://example.com/a.iso";
    r.headers = {{"X-One", "1"}, {"Accept", "*/*"}};
    r.directory = dir.path() / L"déjà";
    r.filename = "a.iso";
    r.queue = "night";
    r.state = DownloadState::Scheduled;
    r.start_at = 1'800'000'000;
    r.size = 123;
    r.downloaded = 45;
    r.connections = 4;
    r.speed_limit = 1000;
    r.checksum = "sha256:abc";
    r.added_at = 1'700'000'000;
    auto inserted = s->insert(r);
    REQUIRE(inserted);
    id = *inserted;

    r.id = id;
    r.state = DownloadState::Completed;
    r.completed_at = 1'700'000'100;
    REQUIRE(s->update(r));
  }
  auto s = Store::open(path); // persisted across reopen
  REQUIRE(s);
  auto all = s->downloads();
  REQUIRE(all);
  REQUIRE(all->size() == 1);
  const auto &r = all->front();
  CHECK(r.id == id);
  CHECK(r.url == "https://example.com/a.iso");
  CHECK(r.headers.size() == 2);
  CHECK(r.directory == dir.path() / L"déjà");
  CHECK(r.queue == "night");
  CHECK(r.state == DownloadState::Completed);
  CHECK(r.start_at == 1'800'000'000);
  CHECK(r.size == 123u);
  CHECK(r.downloaded == 45u);
  CHECK(r.checksum == "sha256:abc");
  CHECK(r.completed_at == 1'700'000'100);

  REQUIRE(s->remove(id));
  CHECK(s->downloads()->empty());
}

TEST_CASE("queues and settings persist", "[store]") {
  TempDir dir;
  auto s = Store::open(dir.path() / "lanes.db");
  REQUIRE(s);
  queue::QueueSettings q{.name = "night", .max_active = 2, .enabled = true};
  q.start_time = queue::TimeOfDay{1, 30};
  q.days = 0x1F;
  REQUIRE(s->save_queue(q));
  q.max_active = 5; // upsert
  REQUIRE(s->save_queue(q));
  auto queues = s->queues();
  REQUIRE(queues);
  REQUIRE(queues->size() == 1);
  CHECK(queues->front().max_active == 5);
  CHECK(queues->front().start_time == queue::TimeOfDay{1, 30});
  CHECK_FALSE(queues->front().stop_time);
  CHECK(queues->front().days == 0x1F);

  REQUIRE(s->set_setting("connections", "4"));
  REQUIRE(s->set_setting("connections", "6"));
  auto settings = s->settings();
  REQUIRE(settings);
  REQUIRE(settings->size() == 1);
  CHECK(settings->front().second == "6");
}
