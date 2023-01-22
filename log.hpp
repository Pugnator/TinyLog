#pragma once

#if !defined(__GNUC__) && !defined(__clang__)
#error "Designed for GCC only"
#endif

#include <iostream>
#include <memory>
#include <fmt/format.h>
#include <windows.h>

#define LOG(...) LOG_INFO(__VA_ARGS__)
#define LOG_INFO(...) Log::get().log(TraceSeverity::info, __VA_ARGS__)
#define LOG_DEBUG(...) Log::get().log(TraceSeverity::debug, __VA_ARGS__)

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

enum class TraceSeverity
{
  info,
  debug,
  warning,
  error,
#if defined(__ARM_EABI__)
  critical,
  fatal,
#endif
};

class Tracer
{
public:
  virtual ~Tracer() {}
  virtual void Info(const std::string &message) = 0;
  virtual void Debug(const std::string &message) = 0;
  virtual void Warning(const std::string &message) = 0;
  virtual void Critical(const std::string &message) = 0;
  virtual void Error(const std::string &message) = 0;
  virtual void Fatal(const std::string &message) = 0;
};

class FileTracer : public Tracer
{
public:
  void Info(const std::string &message) override;
  void Debug(const std::string &message) override;
  void Warning(const std::string &message) override;
  void Critical(const std::string &message) override;
  void Error(const std::string &message) override;
  void Fatal(const std::string &message) override;
};

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
  HANDLE std_out_;
};

class Log
{
public:
  Log(Log const &) = delete;
  void operator=(Log const &) = delete;

  static Log &get(TraceType type = TraceType::console)
  {
    static Log instance(type);
    return instance;
  }

  template <typename S, typename... Args>
  void log(TraceSeverity severity, const S &format, Args &&...args)
  {
    std::string message = fmt::vformat(format, fmt::make_format_args(args...));
    switch (severity)
    {
    case TraceSeverity::info:
      instance_->Info(message);
      break;
    case TraceSeverity::debug:
      instance_->Debug(message);
      break;
    }
  }

private:
  Log(TraceType type = TraceType::console)
  {
    configure(TraceType::console);
  }

  void configure(TraceType lt);
  std::unique_ptr<Tracer> instance_;
};
