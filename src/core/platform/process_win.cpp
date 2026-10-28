// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/platform/process.hpp"

#include <algorithm>
#include <cstddef>
#include <cwchar>
#include <thread>

#include "core/platform/path.hpp"
#include "core/platform/win/win32.hpp"

namespace lanes::core::platform {

namespace {

using win::UniqueHandle;

// Quotes one argument the way CommandLineToArgvW and the C runtime parse it back.
std::string quote_arg(std::string_view arg) {
  if (!arg.empty() && arg.find_first_of(" \t\n\v\"") == std::string_view::npos) {
    return std::string(arg);
  }
  std::string out = "\"";
  for (std::size_t i = 0;; ++i) {
    std::size_t backslashes = 0;
    while (i < arg.size() && arg[i] == '\\') {
      ++i;
      ++backslashes;
    }
    if (i == arg.size()) {
      out.append(backslashes * 2, '\\'); // they precede the closing quote
      break;
    }
    if (arg[i] == '"') {
      out.append(backslashes * 2 + 1, '\\');
      out += '"';
    } else {
      out.append(backslashes, '\\');
      out += arg[i];
    }
  }
  out += '"';
  return out;
}

std::wstring_view env_name(std::wstring_view entry) {
  // Skip index 0: hidden per-drive variables look like "=C:=C:\dir".
  const auto eq = entry.find(L'=', 1);
  return entry.substr(0, eq);
}

bool equal_ignore_case(std::wstring_view a, std::wstring_view b) {
  return ::CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(),
                                static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

// Inherited environment with `overrides` applied, as a CreateProcessW Unicode block.
std::wstring build_environment(const std::vector<std::pair<std::string, std::string>> &overrides) {
  std::vector<std::wstring> entries;
  if (wchar_t *block = ::GetEnvironmentStringsW()) {
    for (const wchar_t *p = block; *p != L'\0'; p += std::wcslen(p) + 1) {
      entries.emplace_back(p);
    }
    ::FreeEnvironmentStringsW(block);
  }
  for (const auto &[name, value] : overrides) {
    const std::wstring wname = win::widen(name);
    std::erase_if(entries,
                  [&](const std::wstring &e) { return equal_ignore_case(env_name(e), wname); });
    entries.push_back(wname + L"=" + win::widen(value));
  }
  std::sort(entries.begin(), entries.end(), [](const std::wstring &a, const std::wstring &b) {
    return ::CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(),
                                  static_cast<int>(b.size()), TRUE) == CSTR_LESS_THAN;
  });
  std::wstring block;
  for (const auto &e : entries) {
    block += e;
    block += L'\0';
  }
  block += L'\0';
  return block;
}

void read_lines(UniqueHandle pipe, const std::function<void(std::string_view)> &on_line) {
  std::string pending;
  std::string buffer(64 * 1024, '\0');
  const auto emit = [&](std::string_view line) {
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }
    on_line(line);
  };
  for (;;) {
    DWORD n = 0;
    if (!::ReadFile(pipe.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &n, nullptr) ||
        n == 0) {
      break; // ERROR_BROKEN_PIPE: every writer has closed its end
    }
    if (!on_line) {
      continue;
    }
    pending.append(buffer.data(), n);
    std::size_t start = 0;
    for (std::size_t nl; (nl = pending.find('\n', start)) != std::string::npos; start = nl + 1) {
      emit(std::string_view(pending).substr(start, nl - start));
    }
    pending.erase(0, start);
  }
  if (on_line && !pending.empty()) {
    emit(pending);
  }
}

struct Pipe {
  UniqueHandle read;
  UniqueHandle write; // inheritable, handed to the child
};

Result<Pipe> make_pipe() {
  SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
  HANDLE r = nullptr;
  HANDLE w = nullptr;
  if (!::CreatePipe(&r, &w, &sa, 0)) {
    return make_error(win::last_error("create pipe"));
  }
  Pipe pipe{UniqueHandle(r), UniqueHandle(w)};
  if (!::SetHandleInformation(r, HANDLE_FLAG_INHERIT, 0)) {
    return make_error(win::last_error("configure pipe"));
  }
  return pipe;
}

} // namespace

std::string windows_command_line(const std::filesystem::path &executable,
                                 const std::vector<std::string> &args) {
  // The program name is parsed differently (no escapes); paths cannot contain '"'.
  std::string line = "\"" + path_to_utf8(executable) + "\"";
  for (const auto &arg : args) {
    line += ' ';
    line += quote_arg(arg);
  }
  return line;
}

struct Process::Impl {
  UniqueHandle process;
  UniqueHandle job;
  DWORD pid = 0;
  std::thread out_reader;
  std::thread err_reader;
  std::optional<int> exit_code;

  void join_readers() {
    if (out_reader.joinable()) {
      out_reader.join();
    }
    if (err_reader.joinable()) {
      err_reader.join();
    }
  }
};

Result<Process> Process::start(ProcessOptions options) {
  auto out = make_pipe();
  if (!out) {
    return make_error(out.error());
  }
  auto err = make_pipe();
  if (!err) {
    return make_error(err.error());
  }
  SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
  UniqueHandle null_in(::CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                                     OPEN_EXISTING, 0, nullptr));
  if (!null_in) {
    return make_error(win::last_error("open NUL"));
  }

  // Inherit only these three handles, not every inheritable handle in the process (another
  // thread may be starting a child with its own pipes at the same time).
  SIZE_T attr_size = 0;
  ::InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
  std::vector<std::byte> attr_buffer(attr_size);
  auto *attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buffer.data());
  if (!::InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size)) {
    return make_error(win::last_error("init process attributes"));
  }
  struct AttrGuard {
    LPPROC_THREAD_ATTRIBUTE_LIST list;
    ~AttrGuard() { ::DeleteProcThreadAttributeList(list); }
  } attr_guard{attrs};
  HANDLE inherited[] = {null_in.get(), out->write.get(), err->write.get()};
  if (!::UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited,
                                   sizeof(inherited), nullptr, nullptr)) {
    return make_error(win::last_error("set inherited handles"));
  }

  UniqueHandle job(::CreateJobObjectW(nullptr, nullptr));
  if (!job) {
    return make_error(win::last_error("create job object"));
  }
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (!::SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits,
                                 sizeof(limits))) {
    return make_error(win::last_error("configure job object"));
  }

  STARTUPINFOEXW si{};
  si.StartupInfo.cb = sizeof(si);
  si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  si.StartupInfo.hStdInput = null_in.get();
  si.StartupInfo.hStdOutput = out->write.get();
  si.StartupInfo.hStdError = err->write.get();
  si.lpAttributeList = attrs;

  std::wstring command_line = win::widen(windows_command_line(options.executable, options.args));
  std::wstring environment;
  if (!options.env.empty()) {
    environment = build_environment(options.env);
  }
  const std::wstring &exe = options.executable.native();
  const std::wstring &cwd = options.working_directory.native();

  PROCESS_INFORMATION pi{};
  if (!::CreateProcessW(exe.c_str(), command_line.data(), nullptr, nullptr, TRUE,
                        CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT |
                            EXTENDED_STARTUPINFO_PRESENT,
                        environment.empty() ? nullptr : environment.data(),
                        cwd.empty() ? nullptr : cwd.c_str(), &si.StartupInfo, &pi)) {
    return make_error(win::last_error("start process", options.executable));
  }
  UniqueHandle process(pi.hProcess);
  UniqueHandle thread(pi.hThread);
  // Assign before the first instruction runs, so nothing it starts can escape the job.
  if (!::AssignProcessToJobObject(job.get(), process.get())) {
    const Error error = win::last_error("assign job object", options.executable);
    ::TerminateProcess(process.get(), 1);
    return make_error(error);
  }
  ::ResumeThread(thread.get());

  // Close our copies of the child's ends; the readers see end-of-file once the child exits.
  out->write.reset();
  err->write.reset();
  null_in.reset();

  Process result;
  result.impl_->process = std::move(process);
  result.impl_->job = std::move(job);
  result.impl_->pid = pi.dwProcessId;
  result.impl_->out_reader =
      std::thread(read_lines, std::move(out->read), std::move(options.on_stdout_line));
  result.impl_->err_reader =
      std::thread(read_lines, std::move(err->read), std::move(options.on_stderr_line));
  return result;
}

Process::Process() : impl_(std::make_unique<Impl>()) {}

Process::~Process() {
  if (impl_ && impl_->process && !impl_->exit_code) {
    kill();
    wait();
  }
}

Process::Process(Process &&) noexcept = default;

Process &Process::operator=(Process &&other) noexcept {
  if (this != &other) {
    if (impl_ && impl_->process && !impl_->exit_code) {
      kill();
      wait();
    }
    impl_ = std::move(other.impl_);
  }
  return *this;
}

std::optional<int> Process::wait(std::optional<std::chrono::milliseconds> timeout) {
  if (!impl_ || !impl_->process) {
    return std::nullopt;
  }
  if (impl_->exit_code) {
    return impl_->exit_code;
  }
  DWORD ms = INFINITE;
  if (timeout) {
    ms = static_cast<DWORD>(std::clamp<long long>(timeout->count(), 0, INFINITE - 1));
  }
  if (::WaitForSingleObject(impl_->process.get(), ms) != WAIT_OBJECT_0) {
    return std::nullopt;
  }
  DWORD code = 0;
  ::GetExitCodeProcess(impl_->process.get(), &code);
  // Anything the child left behind would hold the output pipes open forever.
  ::TerminateJobObject(impl_->job.get(), 1);
  impl_->join_readers();
  impl_->exit_code = static_cast<int>(code);
  return impl_->exit_code;
}

void Process::kill() {
  if (impl_ && impl_->job) {
    ::TerminateJobObject(impl_->job.get(), 1);
  }
}

std::uint32_t Process::pid() const { return impl_ ? impl_->pid : 0; }

Result<CapturedOutput> run_capture(ProcessOptions options, std::chrono::milliseconds timeout) {
  CapturedOutput captured;
  options.on_stdout_line = [&captured](std::string_view line) {
    captured.out.append(line).push_back('\n');
  };
  options.on_stderr_line = [&captured](std::string_view line) {
    captured.err.append(line).push_back('\n');
  };
  const std::filesystem::path executable = options.executable;
  auto process = Process::start(std::move(options));
  if (!process) {
    return make_error(process.error());
  }
  const auto code = process->wait(timeout);
  if (!code) {
    process->kill();
    process->wait();
    return make_error(ErrorCode::Timeout, "'" + path_to_utf8(executable) +
                                              "' did not finish within " +
                                              std::to_string(timeout.count()) + " ms");
  }
  if (!captured.out.empty()) {
    captured.out.pop_back();
  }
  if (!captured.err.empty()) {
    captured.err.pop_back();
  }
  captured.exit_code = *code;
  return captured;
}

} // namespace lanes::core::platform
