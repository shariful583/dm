// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/platform/credentials.hpp"

#include <memory>

#include "core/platform/system.hpp"
#include "core/platform/win/win32.hpp"

#include <wincred.h>

namespace lanes::core::platform {

namespace {

// "Lanes:login:<host>". LANES_CREDENTIAL_NAMESPACE (tests) replaces "Lanes" so test runs never
// touch real logins.
std::wstring prefix() {
  const auto ns = environment_variable("LANES_CREDENTIAL_NAMESPACE");
  return win::widen(ns && !ns->empty() ? *ns : "Lanes") + L":login:";
}

std::string normalize(std::string host) {
  for (char &c : host) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c + 32);
    }
  }
  return host;
}

struct CredFree {
  void operator()(void *p) const noexcept { ::CredFree(p); }
};

} // namespace

Result<void> save_login(const SiteLogin &login) {
  const std::string host = normalize(login.host);
  if (host.empty()) {
    return make_error(ErrorCode::InvalidArgument, "a login needs a host name");
  }
  std::wstring target = prefix() + win::widen(host);
  std::wstring user = win::widen(login.username);
  // Stored as UTF-8 bytes; only this user can read it back.
  std::string secret = login.password;
  CREDENTIALW cred{};
  cred.Type = CRED_TYPE_GENERIC;
  cred.TargetName = target.data();
  cred.UserName = user.data();
  cred.CredentialBlobSize = static_cast<DWORD>(secret.size());
  cred.CredentialBlob = reinterpret_cast<LPBYTE>(secret.data());
  cred.Persist = CRED_PERSIST_LOCAL_MACHINE;
  if (!::CredWriteW(&cred, 0)) {
    return make_error(win::last_error("save login"));
  }
  return {};
}

Result<std::optional<SiteLogin>> find_login(const std::string &host) {
  const std::string h = normalize(host);
  if (h.empty()) {
    return std::optional<SiteLogin>{};
  }
  const std::wstring target = prefix() + win::widen(h);
  PCREDENTIALW raw = nullptr;
  if (!::CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &raw)) {
    if (::GetLastError() == ERROR_NOT_FOUND) {
      return std::optional<SiteLogin>{};
    }
    return make_error(win::last_error("read login"));
  }
  std::unique_ptr<void, CredFree> owner(raw);
  SiteLogin login;
  login.host = h;
  login.username = raw->UserName != nullptr ? win::narrow(raw->UserName) : std::string();
  login.password.assign(reinterpret_cast<const char *>(raw->CredentialBlob),
                        raw->CredentialBlobSize);
  return std::optional<SiteLogin>(std::move(login));
}

Result<void> delete_login(const std::string &host) {
  const std::wstring target = prefix() + win::widen(normalize(host));
  if (!::CredDeleteW(target.c_str(), CRED_TYPE_GENERIC, 0) && ::GetLastError() != ERROR_NOT_FOUND) {
    return make_error(win::last_error("delete login"));
  }
  return {};
}

Result<std::vector<SiteLogin>> list_logins() {
  const std::wstring filter = prefix() + L"*";
  DWORD count = 0;
  PCREDENTIALW *raw = nullptr;
  if (!::CredEnumerateW(filter.c_str(), 0, &count, &raw)) {
    if (::GetLastError() == ERROR_NOT_FOUND) {
      return std::vector<SiteLogin>{};
    }
    return make_error(win::last_error("list logins"));
  }
  std::unique_ptr<void, CredFree> owner(raw);
  const std::size_t prefix_length = prefix().size();
  std::vector<SiteLogin> logins;
  for (DWORD i = 0; i < count; ++i) {
    const std::wstring target = raw[i]->TargetName;
    SiteLogin login;
    login.host = win::narrow(std::wstring_view(target).substr(prefix_length));
    login.username = raw[i]->UserName != nullptr ? win::narrow(raw[i]->UserName) : std::string();
    logins.push_back(std::move(login));
  }
  return logins;
}

} // namespace lanes::core::platform
