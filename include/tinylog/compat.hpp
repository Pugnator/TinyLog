#pragma once

/*! \file The pre-0.2 API (Log::get(), TraceSeverity, LOG_* macros) on top of tinylog.
 *
 * Source compatible with code written for the original singleton logger:
 *   Log::get().configure(TraceType::file, "app.log", RotationConfig{...});
 *   Log::get().set_level(TraceSeverity::debug);
 *   LOG_INFO("started {}\n", name);
 *
 * Differences from the original:
 *   - The singleton lives in the library, so an EXE and a DLL share it.
 *   - A trailing '\n' is optional; each message is exactly one line.
 *   - warning/error/critical/fatal are enabled by default next to info.
 *   - Macro arguments are not evaluated when the level is disabled.
 *   - Format errors are logged instead of thrown.
 */

#include <tinylog/tinylog.hpp>

#include <cstddef>
#include <cstdint>
#include <string>

//! Tracer back-ends of the original API.
enum class TraceType
{
  devnull,
  console,
  file,
};

//! Severity bits of the original API; set_level()/clear_level() toggle single bits.
enum class TraceSeverity : std::uint32_t
{
  info = 1,
  warning = 2,
  error = 4,
  debug = 8,
  verbose = 16,
  critical = 32,
  fatal = 64,
};

//! Rotation settings of the original API (field order kept for designated initializers).
struct RotationConfig
{
  //! Maximum size in bytes before rotation is triggered. 0 = no size limit.
  std::size_t max_file_size = 0;
  //! Maximum number of rotated files to keep. 0 = unlimited.
  std::size_t max_backup_count = 5;
  //! Compress rotated files with zstd (in the background).
  bool compress = false;
  //! zstd level used when compress is set.
  int compress_level = 19;
};

//! Facade of the original API; stateless, every call forwards to tinylog.
class Log
{
public:
  static Log &get() noexcept
  {
    static Log instance;
    return instance;
  }

  static constexpr tinylog::Level to_level(TraceSeverity severity) noexcept
  {
    switch (severity)
    {
    case TraceSeverity::verbose:
      return tinylog::Level::trace;
    case TraceSeverity::debug:
      return tinylog::Level::debug;
    case TraceSeverity::warning:
      return tinylog::Level::warning;
    case TraceSeverity::error:
      return tinylog::Level::error;
    case TraceSeverity::critical:
      return tinylog::Level::critical;
    case TraceSeverity::fatal:
      return tinylog::Level::fatal;
    case TraceSeverity::info:
    default:
      return tinylog::Level::info;
    }
  }

  //! Logs with a run-time format string, like the original std::vformat-based call.
  template <typename S, typename... Args>
  void log(TraceSeverity severity, const S &format, Args &&...args) noexcept
  {
    tinylog::log_runtime(to_level(severity), {}, std::string_view(format), std::forward<Args>(args)...);
  }

  template <typename S, typename... Args>
  void log_at(TraceSeverity severity, const tinylog::SourceLocation &where, const S &format, Args &&...args) noexcept
  {
    tinylog::log_runtime(to_level(severity), where, std::string_view(format), std::forward<Args>(args)...);
  }

  //! Enables one severity in addition to those already enabled.
  Log &set_level(TraceSeverity severity) noexcept
  {
    tinylog::enable(to_level(severity));
    return *this;
  }
  Log &clear_level(TraceSeverity severity) noexcept
  {
    tinylog::disable(to_level(severity));
    return *this;
  }
  Log &reset_levels() noexcept
  {
    tinylog::set_level_mask(0);
    return *this;
  }

  //! Switches the back-end; the enabled severities are kept. Throws if a file cannot be opened.
  Log &configure(TraceType type) { return configure(type, std::string{}, RotationConfig{}); }
  Log &configure(TraceType type, const std::string &filepath) { return configure(type, filepath, RotationConfig{}); }
  Log &configure(TraceType type, const std::string &filepath, const RotationConfig &rotation)
  {
    tinylog::Config config;
    config.console.reset();
    if (type == TraceType::console)
    {
      config.console = tinylog::ConsoleSinkConfig{};
    }
    else if (type == TraceType::file)
    {
      auto &file = config.files.emplace_back();
      file.path = filepath.empty() ? std::string("log.txt") : filepath;
      file.rotation.max_size = rotation.max_file_size;
      file.rotation.max_files = rotation.max_backup_count;
      file.rotation.compress = rotation.compress ? tinylog::Compression::zstd : tinylog::Compression::none;
      file.rotation.compress_level = rotation.compress_level;
    }
    const auto mask = tinylog::level_mask();
    tinylog::init(config);
    tinylog::set_level_mask(mask);
    return *this;
  }
};

#define TINYLOG_COMPAT_LOG_(severity, ...)                                           \
  do                                                                                 \
  {                                                                                  \
    if (::tinylog::should_log(::Log::to_level(severity)))                            \
      ::Log::get().log_at(severity, TINYLOG_HERE, __VA_ARGS__);                      \
  } while (0)

#ifndef LOG
#define LOG(...) LOG_INFO(__VA_ARGS__)
#endif
#ifndef LOG_LEVEL
#define LOG_LEVEL(x) ::Log::get().set_level(x)
#endif
#ifndef LOG_INFO
#define LOG_INFO(...) TINYLOG_COMPAT_LOG_(TraceSeverity::info, __VA_ARGS__)
#endif
#ifndef LOG_DEBUG
#define LOG_DEBUG(...) TINYLOG_COMPAT_LOG_(TraceSeverity::debug, __VA_ARGS__)
#endif
#ifndef LOG_CALL
#define LOG_CALL(...) TINYLOG_COMPAT_LOG_(TraceSeverity::verbose, __VA_ARGS__)
#endif
#ifndef LOG_WARNING
#define LOG_WARNING(...) TINYLOG_COMPAT_LOG_(TraceSeverity::warning, __VA_ARGS__)
#endif
#ifndef LOG_WARN
#define LOG_WARN(...) TINYLOG_COMPAT_LOG_(TraceSeverity::warning, __VA_ARGS__)
#endif
#ifndef LOG_ERROR
#define LOG_ERROR(...) TINYLOG_COMPAT_LOG_(TraceSeverity::error, __VA_ARGS__)
#endif
#ifndef LOG_CRITICAL
#define LOG_CRITICAL(...) TINYLOG_COMPAT_LOG_(TraceSeverity::critical, __VA_ARGS__)
#endif
#ifndef LOG_FATAL
#define LOG_FATAL(...) TINYLOG_COMPAT_LOG_(TraceSeverity::fatal, __VA_ARGS__)
#endif
//! Portable replacement for the original __PRETTY_FUNCTION__-based macro (which broke MSVC).
#ifndef LOG_EXCEPTION
#define LOG_EXCEPTION(description, exception) \
  TINYLOG_COMPAT_LOG_(TraceSeverity::debug, "{}: {} at {} {}:{}", description, (exception).what(), __func__, __FILE__, __LINE__)
#endif
