// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/platform/console.hpp"

#include <mutex>

#include <io.h>
#include <stdio.h>

#include <cstdio>

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

std::optional<std::string> read_password(std::string_view prompt) {
  std::fwrite(prompt.data(), 1, prompt.size(), stderr);
  std::fflush(stderr);
  const HANDLE in = ::GetStdHandle(STD_INPUT_HANDLE);
  DWORD mode = 0;
  const bool console = ::GetConsoleMode(in, &mode) != 0;
  if (console) {
    // Wide read: the console delivers real Unicode regardless of code pages.
    ::SetConsoleMode(in, (mode & ~static_cast<DWORD>(ENABLE_ECHO_INPUT)) | ENABLE_LINE_INPUT);
    std::wstring line;
    wchar_t buffer[256];
    DWORD read = 0;
    bool got_any = false;
    while (::ReadConsoleW(in, buffer, 256, &read, nullptr) && read > 0) {
      got_any = true;
      line.append(buffer, read);
      if (line.find(L'\n') != std::wstring::npos) {
        break;
      }
    }
    ::SetConsoleMode(in, mode);
    std::fputc('\n', stderr); // the Enter key was not echoed
    while (!line.empty() && (line.back() == L'\n' || line.back() == L'\r')) {
      line.pop_back();
    }
    return got_any ? std::optional(win::narrow(line)) : std::nullopt;
  }
  std::string line;
  for (int c = std::fgetc(stdin); c != EOF && c != '\n'; c = std::fgetc(stdin)) {
    line += static_cast<char>(c);
  }
  if (line.empty() && std::feof(stdin)) {
    return std::nullopt;
  }
  if (!line.empty() && line.back() == '\r') {
    line.pop_back();
  }
  return line;
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
