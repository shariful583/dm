// SPDX-License-Identifier: GPL-3.0-or-later
// File categories (through the engine) and site logins (Windows Credential Manager).
#include <algorithm>
#include <iostream>

#include "cli/commands.hpp"
#include "core/category/category.hpp"
#include "core/net/request.hpp"
#include "core/platform/console.hpp"
#include "core/platform/credentials.hpp"
#include "core/platform/system.hpp"
#include "core/service/service.hpp"

namespace lanes::cli {

namespace {

using core::category::Category;

core::Result<core::service::EngineClient> connect_engine() {
  core::service::ClientOptions options;
  options.engine_executable = core::platform::current_executable();
  return core::service::EngineClient::connect(options);
}

int fail(const core::Error &error) {
  std::cerr << "error: " << error.message << '\n';
  return error.code == core::ErrorCode::InvalidArgument ? kExitUsage : kExitError;
}

std::string join(const std::vector<std::string> &items) {
  std::string out;
  for (const auto &item : items) {
    out += (out.empty() ? "" : " ") + item;
  }
  return out;
}

// Accepts "files.example.com" or a whole URL.
std::string normalize_host(const std::string &text) {
  if (text.find("://") != std::string::npos) {
    return core::net::host_of(text);
  }
  std::string host = text;
  for (char &c : host) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c + 32);
    }
  }
  return host;
}

} // namespace

int run_categories() {
  core::platform::use_utf8_console();
  auto client = connect_engine();
  if (!client) {
    return fail(client.error());
  }
  auto categories = client->categories();
  if (!categories) {
    return fail(categories.error());
  }
  for (const auto &c : *categories) {
    std::cout << c.name << "  ->  " << (c.folder.empty() ? "<download_dir>\\" + c.name : c.folder)
              << "\n    " << join(c.extensions) << '\n';
  }
  std::cout << core::category::kGeneral << "  ->  <download_dir>\n    (everything else)\n";
  return kExitOk;
}

int run_category_set(const std::string &name, const std::string &extensions,
                     const std::string &folder) {
  auto client = connect_engine();
  if (!client) {
    return fail(client.error());
  }
  auto categories = client->categories();
  if (!categories) {
    return fail(categories.error());
  }
  const auto exts = core::category::parse_extensions(extensions);
  auto it = std::find_if(categories->begin(), categories->end(),
                         [&](const Category &c) { return c.name == name; });
  if (it == categories->end()) {
    if (exts.empty()) {
      std::cerr << "error: a new category needs --ext\n";
      return kExitUsage;
    }
    categories->push_back({name, {}, ""});
    it = std::prev(categories->end());
  }
  if (!exts.empty()) {
    it->extensions = exts;
    // An extension belongs to one category: take it away from the others.
    for (auto &other : *categories) {
      if (&other != &*it) {
        std::erase_if(other.extensions, [&](const std::string &e) {
          return std::find(exts.begin(), exts.end(), e) != exts.end();
        });
      }
    }
  }
  if (!folder.empty()) {
    it->folder = folder == "default" ? std::string() : folder;
  }
  if (auto ok = client->set_categories(*categories); !ok) {
    return fail(ok.error());
  }
  return kExitOk;
}

int run_category_remove(const std::string &name) {
  auto client = connect_engine();
  if (!client) {
    return fail(client.error());
  }
  auto categories = client->categories();
  if (!categories) {
    return fail(categories.error());
  }
  const auto removed =
      std::erase_if(*categories, [&](const Category &c) { return c.name == name; });
  if (removed == 0) {
    std::cerr << "error: no category named '" << name << "'\n";
    return kExitError;
  }
  if (auto ok = client->set_categories(*categories); !ok) {
    return fail(ok.error());
  }
  return kExitOk;
}

int run_category_reset() {
  auto client = connect_engine();
  if (!client) {
    return fail(client.error());
  }
  if (auto ok = client->set_categories(core::category::default_categories()); !ok) {
    return fail(ok.error());
  }
  return kExitOk;
}

int run_login_add(const std::string &host_text, const std::string &user,
                  const std::string &password, bool password_given) {
  const std::string host = normalize_host(host_text);
  if (host.empty()) {
    std::cerr << "error: not a host name: " << host_text << '\n';
    return kExitUsage;
  }
  std::string secret = password;
  if (!password_given) {
    auto typed = core::platform::read_password("Password for " + user + "@" + host + ": ");
    if (!typed) {
      std::cerr << "error: no password entered\n";
      return kExitUsage;
    }
    secret = std::move(*typed);
  }
  if (auto ok = core::platform::save_login({host, user, secret}); !ok) {
    return fail(ok.error());
  }
  std::cout << "Saved the login for " << host << " in Windows Credential Manager.\n";
  return kExitOk;
}

int run_login_list() {
  auto logins = core::platform::list_logins();
  if (!logins) {
    return fail(logins.error());
  }
  if (logins->empty()) {
    std::cout << "No saved logins.\n";
  }
  for (const auto &l : *logins) {
    std::cout << l.host << "  " << l.username << '\n';
  }
  return kExitOk;
}

int run_login_remove(const std::string &host_text) {
  if (auto ok = core::platform::delete_login(normalize_host(host_text)); !ok) {
    return fail(ok.error());
  }
  return kExitOk;
}

} // namespace lanes::cli
