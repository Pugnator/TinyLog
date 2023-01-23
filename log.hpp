#pragma once

/*! \file A simple logger implementation */

#if !defined(__GNUC__) && !defined(__clang__)
#error "Designed for GCC only"
#endif

#include <iostream>
#include <memory>
#include <fmt/format.h>
#include <windows.h>

//! Macroses for simple usage.
#define LOG(...) LOG_INFO(__VA_ARGS__)
#define LOG_INFO(...) Log::get().log(TraceSeverity::info, __VA_ARGS__)
#define LOG_DEBUG(...) Log::get().log(TraceSeverity::debug, __VA_ARGS__)

//! A tracer type class enumerator.
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
#if defined(__ARM_EABI__)
  critical = 8,
  fatal = 16,
#endif
  debug = 32
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

//! A file tracer. Logs messages to a file.
class FileTracer : public Tracer
{
public:
  void Info(const std::string &message) override;
  void Debug(const std::string &message) override;
  void Warning(const std::string &message) override;
  void Error(const std::string &message) override;
  void Critical(const std::string &message) override;
  void Fatal(const std::string &message) override;
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
    SetConsoleOutputCP(65001);
    std_out_ = GetStdHandle(STD_OUTPUT_HANDLE);
    if (std_out_ == INVALID_HANDLE_VALUE)
    {
      // what to do here?
    }
  }
  void Info(const std::string &message) override;
  void Debug(const std::string &message) override;
  void Warning(const std::string &message) override;
  void Critical(const std::string &message) override;
  void Error(const std::string &message) override;
  void Fatal(const std::string &message) override;

private:
  //! A handle to a terminal.
  HANDLE std_out_;
};

//! A log singletone facade.
class Log
{
public:
  Log(Log const &) = delete;
  void operator=(Log const &) = delete;

  /**
   * @brief Get an instance of a logger.
   *
   * @param type A decired tracer type.
   * @return Log& A logger's singletone instance
   */
  static Log &get(TraceType type = TraceType::console)
  {
    static Log instance(type);
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
    std::string message = fmt::vformat(format, fmt::make_format_args(args...));
    switch (severity)
    {
    case TraceSeverity::info:
      instance_->Info(message);
      break;
    case TraceSeverity::debug:
      instance_->Debug(message);
      break;
    default:
      // not implemented yet
      break;
    }
  }

  /**
   * @brief Set decired logger's level
   *
   * @param level A severity level.
   */
  void set_level(TraceSeverity level)
  {
    // set the bit corresponding to the given severity and all lower severity levels
    logging_level_ |= static_cast<uint32_t>(level) | static_cast<uint32_t>(TraceSeverity::info);
  }

private:
  //! Hidden singletone constructor.
  Log(TraceType type = TraceType::console)
      : logging_level_(0)
  {
    configure(TraceType::console);
  }

  //! Configures enabled tracer.
  void configure(TraceType lt)
  {
    switch (lt)
    {
    case TraceType::devnull:
      instance_ = std::make_unique<VoidTracer>();
      break;
    case TraceType::console:
      instance_ = std::make_unique<ConsoleTracer>();
      break;
    case TraceType::file:
      instance_ = std::make_unique<FileTracer>();
      break;
    default:
      // not implemented yet
      break;
    }
  }

  /**
   * @brief Checks against enable severity levels.
   *
   * @param level severity level to check.
   * @return true if level is enabled.
   * @return false if level is muted.
   */
  bool is_severity_enabled(TraceSeverity level)
  {
    // check if the bit corresponding to the given severity is set
    return (logging_level_ & static_cast<uint32_t>(level)) != 0;
  }

  //! Stores enabled severity level.
  uint32_t logging_level_;
  //! An instance of the actual worker tracer.
  std::unique_ptr<Tracer> instance_;
};
