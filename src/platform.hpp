#pragma once

/*! \file Thin OS layer: files, thread ids, terminals, environment. Internal. */

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace tinylog::platform
{
  //! OS thread id of the caller (cached per thread).
  std::uint64_t thread_id() noexcept;

  //! localtime_r / localtime_s.
  std::tm local_time(std::time_t t) noexcept;
  //! gmtime_r / gmtime_s.
  std::tm utc_time(std::time_t t) noexcept;

  //! Environment variable, or nullopt when unset.
  std::optional<std::string> getenv(const char *name);

  /**
   * @brief An unbuffered OS file handle opened for appending or reading.
   *
   * On Windows the file is shared for read, write and delete so other tools can
   * read, rename or remove a log while it is open.
   */
  class File
  {
  public:
    enum class Mode
    {
      append,
      read,
    };

    File() = default;
    ~File();
    File(const File &) = delete;
    File &operator=(const File &) = delete;
    File(File &&other) noexcept;
    File &operator=(File &&other) noexcept;

    //! @throws std::system_error
    void open(const std::filesystem::path &path, Mode mode, bool truncate = false);
    bool is_open() const noexcept;
    void close() noexcept;

    //! Writes everything or throws std::system_error.
    void write(const void *data, std::size_t size);
    //! Reads up to `size` bytes at `offset`; returns bytes read (0 at EOF).
    std::size_t read_at(std::uint64_t offset, void *data, std::size_t size);
    std::uint64_t size() const;
    //! Cuts the file to `size` bytes; the append position follows.
    void truncate(std::uint64_t size);

  private:
#if defined(_WIN32)
    void *handle_ = nullptr;
#else
    int fd_ = -1;
#endif
  };

  //! Renames, replacing `to`; retries briefly on Windows sharing violations (antivirus, indexers).
  std::error_code rename_file(const std::filesystem::path &from, const std::filesystem::path &to) noexcept;

  //! A console/terminal output channel (stdout or stderr).
  class Console
  {
  public:
    explicit Console(bool use_stderr) noexcept;
    //! True if output goes to an interactive terminal (not a file or pipe).
    bool is_terminal() const noexcept { return terminal_; }
    //! True if ANSI escape sequences are understood.
    bool supports_ansi() const noexcept { return ansi_; }
    //! Writes UTF-8 text; converts to UTF-16 for a Windows console.
    void write(std::string_view text) noexcept;

  private:
#if defined(_WIN32)
    void *handle_ = nullptr;
#else
    int fd_ = -1;
#endif
    bool terminal_ = false;
    bool ansi_ = false;
  };
}
