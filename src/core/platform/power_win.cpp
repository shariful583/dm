// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/platform/power.hpp"

#include <mutex>
#include <system_error>

#include <windows.h>

#include <powrprof.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <urlmon.h>
#include <wrl/client.h>

#include "core/net/request.hpp"
#include "core/platform/path.hpp"
#include "core/platform/win/win32.hpp"

namespace lanes::core::platform {

namespace {

using Microsoft::WRL::ComPtr;

// {6B5D1F2E-8C4A-4E59-9C1E-4C2A9B7F3D10}: identifies Lanes to Attachment Services, which keeps
// per-client settings (e.g. "don't ask again") under it.
constexpr GUID kLanesClient = {
    0x6b5d1f2e, 0x8c4a, 0x4e59, {0x9c, 0x1e, 0x4c, 0x2a, 0x9b, 0x7f, 0x3d, 0x10}};

// COM for the calling thread while it lives (worker threads have none of their own).
class ComScope {
public:
  ComScope() : hr_(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
  ~ComScope() {
    if (SUCCEEDED(hr_)) {
      ::CoUninitialize();
    }
  }
  bool ok() const { return SUCCEEDED(hr_) || hr_ == RPC_E_CHANGED_MODE; }

private:
  HRESULT hr_;
};

Error hresult_error(HRESULT hr, const std::string &what) {
  return win::win32_error(static_cast<DWORD>(HRESULT_CODE(hr)), what);
}

} // namespace

// --- KeepAwake ---

struct KeepAwake::Impl {
  mutable std::mutex mutex;
  HANDLE request = nullptr;
  bool active = false;
};

KeepAwake::KeepAwake() : impl_(std::make_unique<Impl>()) {
  REASON_CONTEXT reason{};
  reason.Version = POWER_REQUEST_CONTEXT_VERSION;
  reason.Flags = POWER_REQUEST_CONTEXT_SIMPLE_STRING;
  static wchar_t text[] = L"Lanes is downloading files";
  reason.Reason.SimpleReasonString = text;
  impl_->request = ::PowerCreateRequest(&reason);
  if (impl_->request == INVALID_HANDLE_VALUE) {
    impl_->request = nullptr;
  }
}

KeepAwake::~KeepAwake() {
  set(false);
  if (impl_->request != nullptr) {
    ::CloseHandle(impl_->request);
  }
}

void KeepAwake::set(bool awake) {
  std::lock_guard lock(impl_->mutex);
  if (awake == impl_->active || impl_->request == nullptr) {
    return;
  }
  const BOOL ok = awake ? ::PowerSetRequest(impl_->request, PowerRequestSystemRequired)
                        : ::PowerClearRequest(impl_->request, PowerRequestSystemRequired);
  if (ok) {
    impl_->active = awake;
  }
}

bool KeepAwake::active() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->active;
}

// --- power actions ---

Result<void> power_action(PowerAction action) {
  if (action == PowerAction::Sleep || action == PowerAction::Hibernate) {
    if (!::SetSuspendState(action == PowerAction::Hibernate ? TRUE : FALSE, FALSE, FALSE)) {
      return make_error(win::last_error(action == PowerAction::Hibernate ? "hibernate" : "sleep"));
    }
    return {};
  }
  // Shutting down needs the shutdown privilege, which every user has but must switch on.
  HANDLE token = nullptr;
  if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
    TOKEN_PRIVILEGES p{};
    p.PrivilegeCount = 1;
    p.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (::LookupPrivilegeValueW(nullptr, SE_SHUTDOWN_NAME, &p.Privileges[0].Luid)) {
      ::AdjustTokenPrivileges(token, FALSE, &p, 0, nullptr, nullptr);
    }
    ::CloseHandle(token);
  }
  if (!::ExitWindowsEx(EWX_SHUTDOWN | EWX_POWEROFF, SHTDN_REASON_MAJOR_OTHER |
                                                        SHTDN_REASON_MINOR_OTHER |
                                                        SHTDN_REASON_FLAG_PLANNED)) {
    return make_error(win::last_error("shut down"));
  }
  return {};
}

// --- Mark of the Web ---

Result<void> mark_and_scan(const std::filesystem::path &file, const std::string &source_url,
                           const std::string &referrer) {
  ComScope com;
  if (!com.ok()) {
    return make_error(ErrorCode::Unknown, "COM is not available for the antivirus check");
  }
  ComPtr<IAttachmentExecute> attachment;
  HRESULT hr = ::CoCreateInstance(CLSID_AttachmentServices, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&attachment));
  if (FAILED(hr)) {
    return make_error(hresult_error(hr, "start Windows Attachment Services"));
  }
  attachment->SetClientGuid(kLanesClient);
  attachment->SetClientTitle(L"Lanes");
  const std::wstring local = to_long_path(file).wstring();
  attachment->SetLocalPath(local.c_str());
  // Never the user name / password of the address.
  const std::wstring source = win::widen(net::strip_credentials(source_url));
  if (!source.empty()) {
    attachment->SetSource(source.c_str());
  }
  const std::wstring ref = win::widen(net::strip_credentials(referrer));
  if (!ref.empty()) {
    attachment->SetReferrer(ref.c_str());
  }
  // Save() writes the zone information and runs the antivirus (no dialogs: no Prompt()).
  hr = attachment->Save();
  std::error_code ec;
  if (FAILED(hr) || !std::filesystem::exists(to_long_path(file), ec)) {
    if (hr == INET_E_SECURITY_PROBLEM || !std::filesystem::exists(to_long_path(file), ec)) {
      return make_error(ErrorCode::AccessDenied,
                        "the antivirus blocked this file (it may contain a virus)");
    }
    return make_error(hresult_error(hr, "check the file with the antivirus"));
  }
  return {};
}

} // namespace lanes::core::platform
