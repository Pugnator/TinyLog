#include "platform.hpp"

#include <chrono>
#include <cstdlib>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif
#if defined(__FreeBSD__)
#include <pthread_np.h>
#endif
#endif

namespace tinylog::platform
{
  namespace
  {
    std::uint64_t query_thread_id() noexcept
    {
#if defined(_WIN32)
      return static_cast<std::uint64_t>(::GetCurrentThreadId());
#elif defined(__linux__)
      return static_cast<std::uint64_t>(::syscall(SYS_gettid));
#elif defined(__APPLE__)
      std::uint64_t id = 0;
      ::pthread_threadid_np(nullptr, &id);
      return id;
#elif defined(__FreeBSD__)
      return static_cast<std::uint64_t>(::pthread_getthreadid_np());
#else
      return static_cast<std::uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
#endif
    }

    [[noreturn]] void throw_last_error(const char *what, const std::filesystem::path &path)
    {
#if defined(_WIN32)
      const int code = static_cast<int>(::GetLastError());
#else
      const int code = errno;
#endif
      throw std::system_error(code, std::system_category(), std::string(what) + " '" + path.string() + "'");
    }
  }

  std::uint64_t thread_id() noexcept
  {
    thread_local const std::uint64_t id = query_thread_id();
    return id;
  }

  std::tm local_time(std::time_t t) noexcept
  {
    std::tm tm{};
#if defined(_WIN32)
    ::localtime_s(&tm, &t);
#else
    ::localtime_r(&t, &tm);
#endif
    return tm;
  }

  std::tm utc_time(std::time_t t) noexcept
  {
    std::tm tm{};
#if defined(_WIN32)
    ::gmtime_s(&tm, &t);
#else
    ::gmtime_r(&t, &tm);
#endif
    return tm;
  }

  std::optional<std::string> getenv(const char *name)
  {
#if defined(_WIN32)
    // _wgetenv would be needed for non-ASCII values; configuration keys are ASCII.
    char *value = nullptr;
    std::size_t length = 0;
    if (::_dupenv_s(&value, &length, name) != 0 || value == nullptr)
      return std::nullopt;
    std::string result(value);
    std::free(value);
    return result;
#else
    const char *value = std::getenv(name);
    if (value == nullptr)
      return std::nullopt;
    return std::string(value);
#endif
  }

  // ---------------------------------------------------------------------------
  // File
  // ---------------------------------------------------------------------------

  File::~File() { close(); }

  File::File(File &&other) noexcept
  {
    *this = std::move(other);
  }

  File &File::operator=(File &&other) noexcept
  {
    if (this != &other)
    {
      close();
#if defined(_WIN32)
      handle_ = other.handle_;
      other.handle_ = nullptr;
#else
      fd_ = other.fd_;
      other.fd_ = -1;
#endif
    }
    return *this;
  }

#if defined(_WIN32)

  void File::open(const std::filesystem::path &path, Mode mode, bool truncate)
  {
    close();
    const DWORD access = mode == Mode::append ? FILE_APPEND_DATA | FILE_READ_DATA | FILE_WRITE_DATA | SYNCHRONIZE
                                              : GENERIC_READ;
    const DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    const DWORD disposition = mode == Mode::read ? OPEN_EXISTING : (truncate ? CREATE_ALWAYS : OPEN_ALWAYS);
    HANDLE h = ::CreateFileW(path.c_str(), access, share, nullptr, disposition, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
      throw_last_error("cannot open", path);
    handle_ = h;
    // Writes go to the file pointer, so keep it at the end (append semantics).
    LARGE_INTEGER zero{};
    ::SetFilePointerEx(h, zero, nullptr, FILE_END);
  }

  bool File::is_open() const noexcept { return handle_ != nullptr; }

  void File::close() noexcept
  {
    if (handle_ != nullptr)
    {
      ::CloseHandle(static_cast<HANDLE>(handle_));
      handle_ = nullptr;
    }
  }

  void File::write(const void *data, std::size_t size)
  {
    const char *cursor = static_cast<const char *>(data);
    while (size > 0)
    {
      const DWORD chunk = size > (1u << 30) ? (1u << 30) : static_cast<DWORD>(size);
      DWORD written = 0;
      if (!::WriteFile(static_cast<HANDLE>(handle_), cursor, chunk, &written, nullptr) || written == 0)
        throw std::system_error(static_cast<int>(::GetLastError()), std::system_category(), "log write failed");
      cursor += written;
      size -= written;
    }
  }

  std::size_t File::read_at(std::uint64_t offset, void *data, std::size_t size)
  {
    OVERLAPPED ov{};
    ov.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFu);
    ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
    DWORD read = 0;
    const DWORD chunk = size > (1u << 30) ? (1u << 30) : static_cast<DWORD>(size);
    if (!::ReadFile(static_cast<HANDLE>(handle_), data, chunk, &read, &ov))
    {
      const DWORD err = ::GetLastError();
      if (err == ERROR_HANDLE_EOF)
        return 0;
      throw std::system_error(static_cast<int>(err), std::system_category(), "log read failed");
    }
    return read;
  }

  std::uint64_t File::size() const
  {
    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(static_cast<HANDLE>(handle_), &size))
      throw std::system_error(static_cast<int>(::GetLastError()), std::system_category(), "cannot stat log");
    return static_cast<std::uint64_t>(size.QuadPart);
  }

  void File::truncate(std::uint64_t size)
  {
    LARGE_INTEGER pos{};
    pos.QuadPart = static_cast<LONGLONG>(size);
    // The pointer is left at the new end, where the next append belongs.
    if (!::SetFilePointerEx(static_cast<HANDLE>(handle_), pos, nullptr, FILE_BEGIN) ||
        !::SetEndOfFile(static_cast<HANDLE>(handle_)))
      throw std::system_error(static_cast<int>(::GetLastError()), std::system_category(), "cannot truncate log");
  }

  std::error_code rename_file(const std::filesystem::path &from, const std::filesystem::path &to) noexcept
  {
    // Virus scanners and search indexers briefly open fresh files without
    // FILE_SHARE_DELETE; a few short retries ride that out.
    DWORD err = 0;
    for (int attempt = 0; attempt < 10; ++attempt)
    {
      if (::MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING))
        return {};
      err = ::GetLastError();
      if (err != ERROR_ACCESS_DENIED && err != ERROR_SHARING_VIOLATION && err != ERROR_LOCK_VIOLATION)
        break;
      ::Sleep(20);
    }
    return std::error_code(static_cast<int>(err), std::system_category());
  }

  // ---------------------------------------------------------------------------
  // Console
  // ---------------------------------------------------------------------------

  Console::Console(bool use_stderr) noexcept
  {
    HANDLE h = ::GetStdHandle(use_stderr ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
    if (h == nullptr || h == INVALID_HANDLE_VALUE)
      return;
    handle_ = h;
    // A redirected handle (file, pipe, CI capture) is valid but not a console:
    // WriteConsole fails on it, WriteFile does not.
    DWORD mode = 0;
    terminal_ = ::GetFileType(h) == FILE_TYPE_CHAR && ::GetConsoleMode(h, &mode) != 0;
    if (terminal_)
    {
      ansi_ = (mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0 ||
              ::SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
    }
  }

  void Console::write(std::string_view text) noexcept
  {
    if (handle_ == nullptr || text.empty())
      return;
    HANDLE h = static_cast<HANDLE>(handle_);
    if (terminal_)
    {
      // UTF-16 renders correctly whatever the console code page is, without
      // changing the process-wide code page.
      thread_local std::vector<wchar_t> wide;
      const int length = static_cast<int>(text.size());
      const int needed = ::MultiByteToWideChar(CP_UTF8, 0, text.data(), length, nullptr, 0);
      if (needed > 0)
      {
        wide.resize(static_cast<std::size_t>(needed));
        ::MultiByteToWideChar(CP_UTF8, 0, text.data(), length, wide.data(), needed);
        DWORD written = 0;
        if (::WriteConsoleW(h, wide.data(), static_cast<DWORD>(needed), &written, nullptr))
          return;
      }
      // Fall through to a byte write rather than lose the line.
    }
    const char *cursor = text.data();
    std::size_t remaining = text.size();
    while (remaining > 0)
    {
      DWORD written = 0;
      if (!::WriteFile(h, cursor, static_cast<DWORD>(remaining), &written, nullptr) || written == 0)
        return;
      cursor += written;
      remaining -= written;
    }
  }

#else // POSIX

  void File::open(const std::filesystem::path &path, Mode mode, bool truncate)
  {
    close();
    int flags = O_CLOEXEC;
    if (mode == Mode::append)
      flags |= O_RDWR | O_CREAT | O_APPEND | (truncate ? O_TRUNC : 0);
    else
      flags |= O_RDONLY;
    int fd = ::open(path.c_str(), flags, 0644);
    if (fd < 0)
      throw_last_error("cannot open", path);
    fd_ = fd;
  }

  bool File::is_open() const noexcept { return fd_ >= 0; }

  void File::close() noexcept
  {
    if (fd_ >= 0)
    {
      ::close(fd_);
      fd_ = -1;
    }
  }

  void File::write(const void *data, std::size_t size)
  {
    const char *cursor = static_cast<const char *>(data);
    while (size > 0)
    {
      const ssize_t written = ::write(fd_, cursor, size);
      if (written < 0)
      {
        if (errno == EINTR)
          continue;
        throw std::system_error(errno, std::system_category(), "log write failed");
      }
      cursor += written;
      size -= static_cast<std::size_t>(written);
    }
  }

  std::size_t File::read_at(std::uint64_t offset, void *data, std::size_t size)
  {
    for (;;)
    {
      const ssize_t n = ::pread(fd_, data, size, static_cast<off_t>(offset));
      if (n >= 0)
        return static_cast<std::size_t>(n);
      if (errno != EINTR)
        throw std::system_error(errno, std::system_category(), "log read failed");
    }
  }

  std::uint64_t File::size() const
  {
    struct stat st{};
    if (::fstat(fd_, &st) != 0)
      throw std::system_error(errno, std::system_category(), "cannot stat log");
    return static_cast<std::uint64_t>(st.st_size);
  }

  void File::truncate(std::uint64_t size)
  {
    if (::ftruncate(fd_, static_cast<off_t>(size)) != 0)
      throw std::system_error(errno, std::system_category(), "cannot truncate log");
  }

  std::error_code rename_file(const std::filesystem::path &from, const std::filesystem::path &to) noexcept
  {
    if (::rename(from.c_str(), to.c_str()) == 0)
      return {};
    return std::error_code(errno, std::system_category());
  }

  Console::Console(bool use_stderr) noexcept
      : fd_(use_stderr ? STDERR_FILENO : STDOUT_FILENO)
  {
    terminal_ = ::isatty(fd_) == 1;
    const char *term = std::getenv("TERM");
    ansi_ = terminal_ && !(term != nullptr && std::string_view(term) == "dumb");
  }

  void Console::write(std::string_view text) noexcept
  {
    const char *cursor = text.data();
    std::size_t remaining = text.size();
    while (remaining > 0)
    {
      const ssize_t written = ::write(fd_, cursor, remaining);
      if (written < 0)
      {
        if (errno == EINTR)
          continue;
        return;
      }
      cursor += written;
      remaining -= static_cast<std::size_t>(written);
    }
  }

#endif
}
