// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/platform/console.hpp"

#include <mutex>

#include <io.h>
#include <stdio.h>

#include "core/platform/win/win32.hpp"

namespace lanes::core::platform {

namespace {

std::mutex g_interrupt_mutex;
std::function<void()> g_interrupt_handler;

BOOL WINAPI console_handler(DWORD type) {
  if (type != CTRL_C_EVENT && type != CTRL_BREAK_EVENT) {
    return FALSE; // close/logoff/shutdown: default handling
  }
  std::function<void()> handler;
  {
    std::lock_guard lock(g_interrupt_mutex);
    handler = g_interrupt_handler;
  }
  if (!handler) {
    return FALSE;
  }
  handler();
  return TRUE;
}

} // namespace

bool stderr_is_terminal() { return _isatty(_fileno(stderr)) != 0; }

int terminal_columns() {
  CONSOLE_SCREEN_BUFFER_INFO info{};
  if (::GetConsoleScreenBufferInfo(::GetStdHandle(STD_ERROR_HANDLE), &info)) {
    return info.srWindow.Right - info.srWindow.Left + 1;
  }
  return 80;
}

void use_utf8_console() {
  ::SetConsoleOutputCP(CP_UTF8);
  ::SetConsoleCP(CP_UTF8);
}

void on_console_interrupt(std::function<void()> handler) {
  {
    std::lock_guard lock(g_interrupt_mutex);
    g_interrupt_handler = std::move(handler);
  }
  static std::once_flag installed;
  std::call_once(installed, [] { ::SetConsoleCtrlHandler(console_handler, TRUE); });
}

} // namespace lanes::core::platform
