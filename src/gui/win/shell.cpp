// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/win/shell.hpp"

#include <algorithm>

#include <windows.h>

#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>

namespace lanes::gui::win {

namespace {

using Microsoft::WRL::ComPtr;

constexpr UINT kTrayCallback = WM_APP + 1;
constexpr UINT kTrayId = 1;

HWND hwnd_of(QWindow *window) { return window ? reinterpret_cast<HWND>(window->winId()) : nullptr; }

std::wstring wide(const QString &s) { return s.toStdWString(); }

template <std::size_t N> void copy_text(wchar_t (&dst)[N], const QString &src) {
  const std::wstring w = wide(src.left(static_cast<qsizetype>(N) - 1));
  std::copy(w.begin(), w.end(), dst);
  dst[w.size()] = L'\0';
}

HICON load_app_icon(int size) {
  return static_cast<HICON>(::LoadImageW(::GetModuleHandleW(nullptr), L"IDI_ICON1", IMAGE_ICON,
                                         size, size, LR_DEFAULTCOLOR));
}

} // namespace

// --- TrayIcon ---

struct TrayIcon::Impl {
  TrayIcon *owner = nullptr;
  HWND hwnd = nullptr;
  HICON small_icon = nullptr;
  HICON large_icon = nullptr;
  UINT taskbar_created = 0;
  QString tooltip;
  std::vector<MenuItem> menu;
  qint64 last_tag = 0;
  bool added = false;

  NOTIFYICONDATAW base() const {
    NOTIFYICONDATAW n{};
    n.cbSize = sizeof(n);
    n.hWnd = hwnd;
    n.uID = kTrayId;
    return n;
  }

  void add() {
    NOTIFYICONDATAW n = base();
    n.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    n.uCallbackMessage = kTrayCallback;
    n.hIcon = small_icon;
    copy_text(n.szTip, tooltip);
    added = ::Shell_NotifyIconW(NIM_ADD, &n) != FALSE;
    n.uVersion = NOTIFYICON_VERSION_4;
    ::Shell_NotifyIconW(NIM_SETVERSION, &n);
  }

  static void append(HMENU menu, const std::vector<MenuItem> &items) {
    for (const auto &item : items) {
      if (item.text.isEmpty()) {
        ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
      } else if (!item.children.empty()) {
        HMENU sub = ::CreatePopupMenu();
        append(sub, item.children);
        ::AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(sub), wide(item.text).c_str());
      } else {
        UINT flags = MF_STRING;
        flags |= item.checked ? MF_CHECKED : MF_UNCHECKED;
        flags |= item.enabled ? MF_ENABLED : MF_GRAYED;
        ::AppendMenuW(menu, flags, static_cast<UINT_PTR>(item.id), wide(item.text).c_str());
      }
    }
  }

  void show_menu(POINT at) {
    HMENU popup = ::CreatePopupMenu();
    append(popup, menu);
    ::SetForegroundWindow(hwnd); // otherwise the menu does not close when clicking elsewhere
    const UINT flags = TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY |
                       (::GetSystemMetrics(SM_MENUDROPALIGNMENT) ? TPM_RIGHTALIGN : TPM_LEFTALIGN);
    const int chosen =
        static_cast<int>(::TrackPopupMenuEx(popup, flags, at.x, at.y, hwnd, nullptr));
    ::PostMessageW(hwnd, WM_NULL, 0, 0);
    ::DestroyMenu(popup); // also destroys submenus
    if (chosen != 0) {
      emit owner->menuTriggered(chosen);
    }
  }

  static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto *self = reinterpret_cast<Impl *>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self != nullptr) {
      if (msg == kTrayCallback) {
        switch (LOWORD(lp)) {
        case NIN_SELECT:
        case NIN_KEYSELECT:
          emit self->owner->activated();
          return 0;
        case WM_CONTEXTMENU:
          self->show_menu(POINT{static_cast<short>(LOWORD(wp)), static_cast<short>(HIWORD(wp))});
          return 0;
        case NIN_BALLOONUSERCLICK:
          emit self->owner->messageClicked(self->last_tag);
          return 0;
        default:
          return 0;
        }
      }
      if (msg == self->taskbar_created && self->taskbar_created != 0) {
        self->add(); // Explorer restarted
        return 0;
      }
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
  }
};

TrayIcon::TrayIcon(const QString &tooltip, QObject *parent)
    : QObject(parent), impl_(std::make_unique<Impl>()) {
  Impl &d = *impl_;
  d.owner = this;
  d.tooltip = tooltip;
  const HINSTANCE instance = ::GetModuleHandleW(nullptr);
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = &Impl::proc;
  wc.hInstance = instance;
  wc.lpszClassName = L"LanesTrayWindow";
  ::RegisterClassExW(&wc); // fails harmlessly when already registered
  // A hidden top-level window (not message-only): it must receive the TaskbarCreated broadcast.
  d.hwnd = ::CreateWindowExW(0, wc.lpszClassName, L"Lanes", WS_OVERLAPPED, 0, 0, 0, 0, nullptr,
                             nullptr, instance, nullptr);
  ::SetWindowLongPtrW(d.hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&d));
  d.taskbar_created = ::RegisterWindowMessageW(L"TaskbarCreated");
  d.small_icon = load_app_icon(::GetSystemMetrics(SM_CXSMICON));
  d.large_icon = load_app_icon(::GetSystemMetrics(SM_CXICON));
  d.add();
}

TrayIcon::~TrayIcon() {
  Impl &d = *impl_;
  NOTIFYICONDATAW n = d.base();
  ::Shell_NotifyIconW(NIM_DELETE, &n);
  ::DestroyWindow(d.hwnd);
  ::DestroyIcon(d.small_icon);
  ::DestroyIcon(d.large_icon);
}

void TrayIcon::setTooltip(const QString &tooltip) {
  Impl &d = *impl_;
  if (d.tooltip == tooltip) {
    return;
  }
  d.tooltip = tooltip;
  NOTIFYICONDATAW n = d.base();
  n.uFlags = NIF_TIP | NIF_SHOWTIP;
  copy_text(n.szTip, tooltip);
  ::Shell_NotifyIconW(NIM_MODIFY, &n);
}

void TrayIcon::setMenu(std::vector<MenuItem> items) { impl_->menu = std::move(items); }

void TrayIcon::showMessage(const QString &title, const QString &text, qint64 tag) {
  Impl &d = *impl_;
  d.last_tag = tag;
  NOTIFYICONDATAW n = d.base();
  n.uFlags = NIF_INFO;
  copy_text(n.szInfoTitle, title);
  copy_text(n.szInfo, text);
  n.dwInfoFlags = NIIF_USER | NIIF_LARGE_ICON;
  n.hBalloonIcon = d.large_icon;
  ::Shell_NotifyIconW(NIM_MODIFY, &n);
}

// --- TaskbarProgress ---

struct TaskbarProgress::Impl {
  ComPtr<ITaskbarList3> list;
};

TaskbarProgress::TaskbarProgress() : impl_(std::make_unique<Impl>()) {
  // Qt has initialized COM (OLE) on the GUI thread.
  if (SUCCEEDED(::CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&impl_->list)))) {
    impl_->list->HrInit();
  }
}

TaskbarProgress::~TaskbarProgress() = default;

void TaskbarProgress::set(QWindow *window, double progress, bool paused) {
  const HWND hwnd = hwnd_of(window);
  if (!impl_->list || hwnd == nullptr) {
    return;
  }
  if (progress < 0) {
    impl_->list->SetProgressState(hwnd, TBPF_NOPROGRESS);
    return;
  }
  impl_->list->SetProgressState(hwnd, paused ? TBPF_PAUSED : TBPF_NORMAL);
  impl_->list->SetProgressValue(hwnd, static_cast<ULONGLONG>(std::clamp(progress, 0.0, 1.0) * 1000),
                                1000);
}

// --- window style ---

bool style_window(QWindow *window, bool dark, bool mica) {
  const HWND hwnd = hwnd_of(window);
  if (hwnd == nullptr) {
    return false;
  }
  const BOOL use_dark = dark ? TRUE : FALSE;
  ::DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &use_dark, sizeof(use_dark));
  if (!mica) {
    return false;
  }
  const DWM_SYSTEMBACKDROP_TYPE backdrop = DWMSBT_MAINWINDOW; // Mica; Windows 11 22H2+
  if (FAILED(
          ::DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof(backdrop)))) {
    return false;
  }
  const MARGINS whole{-1, -1, -1, -1}; // let the backdrop show through the client area
  return SUCCEEDED(::DwmExtendFrameIntoClientArea(hwnd, &whole));
}

// --- shell ---

void show_in_folder(const QString &path) {
  const std::wstring p = wide(path);
  if (PIDLIST_ABSOLUTE item = ::ILCreateFromPathW(p.c_str())) {
    const HRESULT hr = ::SHOpenFolderAndSelectItems(item, 0, nullptr, 0);
    ::ILFree(item);
    if (SUCCEEDED(hr)) {
      return;
    }
  }
  // The file is gone (or never arrived): open its folder instead.
  const std::wstring folder = wide(path.left(path.lastIndexOf('\\')));
  ::ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

bool open_with_default_app(const QString &path) {
  const std::wstring p = wide(path);
  const auto result = reinterpret_cast<INT_PTR>(
      ::ShellExecuteW(nullptr, L"open", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
  return result > 32;
}

void allow_foreground_handoff() { ::AllowSetForegroundWindow(ASFW_ANY); }

void bring_to_front(QWindow *window) {
  if (window == nullptr) {
    return;
  }
  window->show();
  if (window->windowStates() & Qt::WindowMinimized) {
    window->setWindowStates(window->windowStates() & ~Qt::WindowMinimized);
  }
  window->raise();
  window->requestActivate();
  if (const HWND hwnd = hwnd_of(window)) {
    ::SetForegroundWindow(hwnd);
  }
}

void error_box(const QString &title, const QString &text) {
  ::MessageBoxW(nullptr, wide(text).c_str(), wide(title).c_str(), MB_OK | MB_ICONERROR);
}

} // namespace lanes::gui::win
