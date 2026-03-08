#pragma once

/*! \file A simple logger implementation */

#include <mutex>
#include <shared_mutex>
#include <fstream>
#include <format>
#include <mutex>
#include <memory>
#include <iomanip>
#include <string>
#include <cstdint>
#include <atomic>
#include <filesystem>

#if ((defined(WIN32) || defined(__MINGW32__) || defined(__MINGW64__)))
#include <windows.h>
#endif

#ifdef __linux__
#include <iostream>
#endif

//! Macross for simple usage.
#define LOG(...) LOG_INFO(__VA_ARGS__)
#define LOG_LEVEL(x) Log::get().set_level(x)
#define LOG_INFO(...) Log::get().log(TraceSeverity::info, __VA_ARGS__)
#define LOG_DEBUG(...) Log::get().log(TraceSeverity::debug, __VA_ARGS__)
#define LOG_EXCEPTION(description, exception) \
  Log::get().log(TraceSeverity::debug, "{}: {} at {} {}:{}\n", description, exception.what(), __PRETTY_FUNCTION__, __FILE__, __LINE__)

#define LOG_CALL(...) Log::get().log(TraceSeverity::verbose, __VA_ARGS__)

//! A tracer-type class enumerator.
enum class TraceType
{
  devnull,
  console,
  file,
#if defined(__ARM_EABI__)
  uart,
  swd,
  rtt
#endif
};

//! A tracer severity class enumerator.
enum class TraceSeverity
{
  info = 1,
  warning = 2,
  error = 4,
  debug = 8,
  verbose = 16,
  critical = 32,
};

//! Configuration for log-file rotation and compression.
struct RotationConfig
{
  //! Maximum size in bytes before rotation is triggered. 0 = no size limit.
  std::size_t max_file_size = 0;
  //! Maximum number of rotated (back-up) files to keep. 0 = unlimited.
  std::size_t max_backup_count = 5;
  //! Compress rotated files with zstd (maximum compression level).
  bool compress = false;
};

//! A tracer abstract interface.
class Tracer
{
public:
  virtual ~Tracer() {}
  /**
   * @brief Generic informational message
   */
  virtual void Info(const std::string &message) = 0;
  /**
   * @brief A message usually only needed for debug purposes.
   */
  virtual void Debug(const std::string &message) = 0;
  /**
   * @brief A warning an user should pay an attention to.
   */
  virtual void Warning(const std::string &message) = 0;
  /**
   * @brief  A problem that can cause the system or application to malfunction or produce incorrect results,
   * but it does not necessarily bring the system or application down completely.
   */
  virtual void Error(const std::string &message) = 0;

  /**
   * @brief A serious problem that can cause significant impact to the system or application,
   * but the system or application can still continue to function with some limitations or degradation of service.
   */
  virtual void Critical(const std::string &message) = 0;
  /**
   * @brief A problem that causes the system or application to crash or become completely non-functional.
   */
  virtual void Fatal(const std::string &message) = 0;
};

//! A file tracer. Logs messages to a file with optional rotation & zstd compression.
class FileTracer : public Tracer
{
public:
  explicit FileTracer(const std::string &filepath = "log.txt",
                      const RotationConfig &rotation = {});
  ~FileTracer();

  void Info(const std::string &message) override;
  void Debug(const std::string &message) override;
  void Warning(const std::string &message) override;
  void Error(const std::string &message) override;
  void Critical(const std::string &message) override;
  void Fatal(const std::string &message) override;

private:
  //! Open (or re-open) the active log file.
  void open_log_file();
  //! Check whether the current file exceeds the size limit and rotate if needed.
  void maybe_rotate();
  //! Perform a single rotation step: close, rename, compress, prune.
  void rotate();
  //! Compress a file in-place using zstd at maximum compression level.
  static bool compress_file_zstd(const std::filesystem::path &src);
  //! Remove excess backup files beyond max_backup_count.
  void prune_old_backups();

  //! Path to the active log file.
  std::filesystem::path filepath_;
  //! Current number of bytes written since the file was opened.
  std::size_t current_size_ = 0;
  //! Rotation / compression configuration.
  RotationConfig rotation_;
  //! A handle to a filestream.
  std::ofstream file_handle_;
  //! A mutex to protect filestream.
  std::mutex mutex_;
};

//! A void tracer. Used when you want to silent all message or there is nowhere to output.
class VoidTracer : public Tracer
{
public:
  void Info(const std::string &message) override;
  void Debug(const std::string &message) override;
  void Warning(const std::string &message) override;
  void Critical(const std::string &message) override;
  void Error(const std::string &message) override;
  void Fatal(const std::string &message) override;
};

//! A console/terminal tracer.
class ConsoleTracer : public Tracer
{
public:
  ConsoleTracer()
  {
#if ((defined(WIN32) || defined(__MINGW32__) || defined(__MINGW64__)))
    SetConsoleOutputCP(65001);
    std_out_ = GetStdHandle(STD_OUTPUT_HANDLE);
    if (std_out_ == INVALID_HANDLE_VALUE)
    {
      // what to do here?
    }
#endif
  }
  void Info(const std::string &message) override;
  void Debug(const std::string &message) override;
  void Warning(const std::string &message) override;
  void Critical(const std::string &message) override;
  void Error(const std::string &message) override;
  void Fatal(const std::string &message) override;

private:
  //! Internal write without locking – caller must hold mutex_.
  void write_impl(const std::string &formatted);

#if ((defined(WIN32) || defined(__MINGW32__) || defined(__MINGW64__)))
  //! A handle to a terminal.
  HANDLE std_out_;
#endif
  //! A mutex to protect terminal.
  std::mutex mutex_;
};

//! A log singletone facade.
class Log
{
public:
  /**
   * @brief Get an instance of a logger.
   *
   * @return Log& A logger's singletone instance
   */
  static Log &get()
  {
    static Log instance;
    return instance;
  }

  /**
   * @brief A main logging entry point for an user.
   *
   * @tparam S
   * @tparam Args
   * @param severity a log message severity level
   * @param format
   * @param args
   */
  template <typename S, typename... Args>
  void log(TraceSeverity severity, const S &format, Args &&...args)
  {
    if (!is_severity_enabled(severity))
    {
      // This channel is muted.
      return;
    }
    std::string message = std::vformat(format, std::make_format_args(args...));
    std::shared_lock<std::shared_mutex> lock(instance_mutex_);
    switch (severity)
    {
    case TraceSeverity::info:
      instance_->Info(message);
      break;
    case TraceSeverity::debug:
      instance_->Debug(message);
      break;
    case TraceSeverity::warning:
      instance_->Warning(message);
      break;
    case TraceSeverity::error:
      instance_->Error(message);
      break;
    case TraceSeverity::critical:
      instance_->Critical(message);
      break;
    case TraceSeverity::verbose:
      instance_->Info(message);
      break;
    default:
      instance_->Info(message);
      break;
    }
  }
  /**
   * @brief Set desired logger's level
   *
   * @param level A severity level.
   */
  Log& set_level(TraceSeverity level);
  /**
   * @brief Clear desired logger's level
   *
   * @param level A severity level.
   */
  Log& clear_level(TraceSeverity level);
  /**
   * @brief Reset all logger's levels
   *
   */
  Log& reset_levels();

  //! Configures enabled tracer.
  Log& configure(TraceType lt);

  //! Configures file tracer with a custom path.
  Log& configure(TraceType lt, const std::string &filepath);

  //! Configures file tracer with a custom path and rotation settings.
  Log& configure(TraceType lt, const std::string &filepath, const RotationConfig &rotation);

private:
  Log();
  Log(Log const &) = delete;
  Log(Log &&) = delete;
  Log &operator=(Log const &) = delete;
  Log &operator=(Log &&) = delete;

  /**
   * @brief Checks against enable severity levels.
   *
   * @param level severity level to check.
   * @return true if level is enabled.
   * @return false if level is muted.
   */
  bool is_severity_enabled(TraceSeverity level);

  //! Internal configure without locking – caller must hold instance_mutex_.
  void configure_impl(TraceType lt);

  //! Stores enabled severity level (atomic for lock-free read/write).
  std::atomic<uint32_t> logging_level_{0};
  //! Protects instance_ for concurrent log/configure access.
  mutable std::shared_mutex instance_mutex_;
  //! An instance of the actual worker tracer.
  std::unique_ptr<Tracer> instance_;
};
