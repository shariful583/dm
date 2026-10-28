// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/platform/file.hpp"

#include <algorithm>
#include <utility>

#include "core/platform/path.hpp"
#include "core/platform/win/win32.hpp"

#include <winioctl.h>

namespace lanes::core::platform {

namespace {

constexpr DWORD kMaxIoChunk = 1u << 30; // per WriteFile/ReadFile call

OVERLAPPED overlapped_at(std::uint64_t offset) {
  OVERLAPPED ov{};
  ov.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFu);
  ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
  return ov;
}

} // namespace

Result<File> File::open(const std::filesystem::path &path, OpenMode mode) {
  DWORD access = GENERIC_READ;
  DWORD share = FILE_SHARE_READ | FILE_SHARE_DELETE;
  DWORD disposition = OPEN_EXISTING;
  switch (mode) {
  case OpenMode::ReadOnly:
    share |= FILE_SHARE_WRITE;
    break;
  case OpenMode::ReadWrite:
    access |= GENERIC_WRITE;
    disposition = OPEN_ALWAYS;
    break;
  case OpenMode::CreateAlways:
    access |= GENERIC_WRITE;
    disposition = CREATE_ALWAYS;
    break;
  }

  const HANDLE h = ::CreateFileW(to_long_path(path).c_str(), access, share, nullptr, disposition,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    return make_error(win::last_error("open file", path));
  }
  File file;
  file.path_ = path;
  file.handle_ = h;
  return file;
}

File::~File() { close(); }

File::File(File &&other) noexcept
    : path_(std::move(other.path_)), handle_(std::exchange(other.handle_, nullptr)) {}

File &File::operator=(File &&other) noexcept {
  if (this != &other) {
    close();
    path_ = std::move(other.path_);
    handle_ = std::exchange(other.handle_, nullptr);
  }
  return *this;
}

bool File::is_open() const noexcept { return handle_ != nullptr; }

void File::close() noexcept {
  if (handle_ != nullptr) {
    ::CloseHandle(handle_);
    handle_ = nullptr;
  }
}

Result<std::uint64_t> File::size() const {
  LARGE_INTEGER size{};
  if (!::GetFileSizeEx(handle_, &size)) {
    return make_error(win::last_error("get file size", path_));
  }
  return static_cast<std::uint64_t>(size.QuadPart);
}

Result<void> File::resize(std::uint64_t size) {
  FILE_END_OF_FILE_INFO info{};
  info.EndOfFile.QuadPart = static_cast<LONGLONG>(size);
  if (!::SetFileInformationByHandle(handle_, FileEndOfFileInfo, &info, sizeof(info))) {
    return make_error(win::last_error("resize file", path_));
  }
  return {};
}

Result<bool> File::set_sparse(bool sparse) {
  FILE_SET_SPARSE_BUFFER buffer{};
  buffer.SetSparse = sparse ? TRUE : FALSE;
  DWORD returned = 0;
  if (!::DeviceIoControl(handle_, FSCTL_SET_SPARSE, &buffer, sizeof(buffer), nullptr, 0, &returned,
                         nullptr)) {
    const DWORD code = ::GetLastError();
    if (code == ERROR_INVALID_FUNCTION || code == ERROR_NOT_SUPPORTED ||
        code == ERROR_INVALID_PARAMETER) {
      return false;
    }
    return make_error(win::win32_error(code, "set sparse attribute", path_));
  }
  return sparse;
}

Result<bool> File::preallocate(std::uint64_t size) {
  auto sparse = set_sparse(true);
  if (!sparse) {
    return sparse;
  }
  if (auto resized = resize(size); !resized) {
    return make_error(resized.error());
  }
  return *sparse;
}

Result<void> File::write_at(std::uint64_t offset, std::span<const char> data) {
  while (!data.empty()) {
    const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(data.size(), kMaxIoChunk));
    OVERLAPPED ov = overlapped_at(offset);
    DWORD written = 0;
    if (!::WriteFile(handle_, data.data(), chunk, &written, &ov)) {
      return make_error(win::last_error("write file", path_));
    }
    if (written == 0) {
      return make_error(ErrorCode::Io, "write file '" + path_to_utf8(path_) + "': no progress");
    }
    offset += written;
    data = data.subspan(written);
  }
  return {};
}

Result<std::size_t> File::read_at(std::uint64_t offset, std::span<char> out) const {
  std::size_t total = 0;
  while (total < out.size()) {
    const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(out.size() - total, kMaxIoChunk));
    OVERLAPPED ov = overlapped_at(offset + total);
    DWORD read = 0;
    if (!::ReadFile(handle_, out.data() + total, chunk, &read, &ov)) {
      if (::GetLastError() == ERROR_HANDLE_EOF) {
        break;
      }
      return make_error(win::last_error("read file", path_));
    }
    if (read == 0) {
      break;
    }
    total += read;
  }
  return total;
}

Result<void> File::flush() {
  if (!::FlushFileBuffers(handle_)) {
    return make_error(win::last_error("flush file", path_));
  }
  return {};
}

} // namespace lanes::core::platform
