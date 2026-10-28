// SPDX-License-Identifier: GPL-3.0-or-later
// Child program for process-runner tests. Usage: lanes_test_child <mode> [args...]
//   args ...           print "argc=N", then each argument as hex-encoded UTF-8, one per line
//   exit N             exit with code N
//   stderr TEXT        print TEXT to stderr
//   spam N             print N lines "line <i>" to stdout and stderr
//   partial            print "no newline at end" without a trailing newline
//   sleep MS           sleep MS milliseconds
//   env NAME           print the value of environment variable NAME
//   cwd                print the current directory (UTF-8)
//   spawn-sleeper MS   start "sleep MS" as a grandchild, print its PID, then sleep MS
#include <cstdio>
#include <cstdlib>
#include <string>

#include <windows.h>

namespace {

std::string utf8(const wchar_t *s) {
  const int n = ::WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
  std::string out(static_cast<std::size_t>(n), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, s, -1, out.data(), n, nullptr, nullptr);
  out.pop_back(); // terminator
  return out;
}

std::string hex(const std::string &s) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string out;
  for (const char c : s) {
    const auto b = static_cast<unsigned char>(c);
    out += kDigits[b >> 4];
    out += kDigits[b & 0x0F];
  }
  return out;
}

} // namespace

int wmain(int argc, wchar_t **argv) {
  if (argc < 2) {
    return 2;
  }
  const std::wstring mode = argv[1];
  if (mode == L"args") {
    std::printf("argc=%d\n", argc - 2);
    for (int i = 2; i < argc; ++i) {
      std::printf("%s\n", hex(utf8(argv[i])).c_str());
    }
  } else if (mode == L"exit" && argc > 2) {
    return _wtoi(argv[2]);
  } else if (mode == L"stderr" && argc > 2) {
    std::fprintf(stderr, "%s\n", utf8(argv[2]).c_str());
  } else if (mode == L"spam" && argc > 2) {
    const int n = _wtoi(argv[2]);
    for (int i = 0; i < n; ++i) {
      std::printf("line %d\n", i);
      std::fprintf(stderr, "err %d\n", i);
    }
  } else if (mode == L"partial") {
    std::printf("no newline at end");
  } else if (mode == L"sleep" && argc > 2) {
    ::Sleep(static_cast<DWORD>(_wtoi(argv[2])));
  } else if (mode == L"env" && argc > 2) {
    const DWORD n = ::GetEnvironmentVariableW(argv[2], nullptr, 0);
    std::wstring value(n, L'\0');
    ::GetEnvironmentVariableW(argv[2], value.data(), n);
    value.resize(n == 0 ? 0 : n - 1);
    std::printf("%s\n", utf8(value.c_str()).c_str());
  } else if (mode == L"cwd") {
    wchar_t buf[4096];
    ::GetCurrentDirectoryW(4096, buf);
    std::printf("%s\n", utf8(buf).c_str());
  } else if (mode == L"spawn-sleeper" && argc > 2) {
    wchar_t self[MAX_PATH];
    ::GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring cmd = L"\"" + std::wstring(self) + L"\" sleep " + argv[2];
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!::CreateProcessW(self, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                          nullptr, &si, &pi)) {
      return 3;
    }
    std::printf("%lu\n", pi.dwProcessId);
    std::fflush(stdout);
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    ::Sleep(static_cast<DWORD>(_wtoi(argv[2])));
  } else {
    return 2;
  }
  std::fflush(stdout);
  std::fflush(stderr);
  return 0;
}
