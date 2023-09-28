#include "log.hpp"

void VoidTracer::Info(const std::string &message) {}
void VoidTracer::Debug(const std::string &message) {}
void VoidTracer::Warning(const std::string &message) {}
void VoidTracer::Critical(const std::string &message) {}
void VoidTracer::Error(const std::string &message) {}
void VoidTracer::Fatal(const std::string &message) {}

void FileTracer::Info(const std::string &message)
{
  std::time_t t = std::time(nullptr);
  std::tm tm = *std::localtime(&t);
  // Write the log message to the file
  file_handle_ << std::put_time(&tm, "[%Y-%m-%d %H:%M:%S] ") << message;
}

void FileTracer::Debug(const std::string &message)
{
  std::string header("Debug: ");
  Info(header + message);
}

void FileTracer::Warning(const std::string &message)
{
  std::string header("Warning: ");
  Info(header + message);
}

void FileTracer::Critical(const std::string &message)
{
  std::string header("CRITICAL: ");
  Info(header + message);
}

void FileTracer::Error(const std::string &message)
{
  std::string header("ERROR: ");
  Info(header + message);
}

void FileTracer::Fatal(const std::string &message)
{
}

#if ((defined(WIN32) || defined(__MINGW32__) || defined(__MINGW64__)))

void ConsoleTracer::Info(const std::string &message)
{
  if (std_out_ == INVALID_HANDLE_VALUE)
    return;

  SetConsoleTextAttribute(std_out_, FOREGROUND_INTENSITY | FOREGROUND_GREEN | FOREGROUND_BLUE);
  DWORD dwBytesWritten;
  WriteConsole(std_out_, message.c_str(), message.length(), &dwBytesWritten, NULL);
}

void ConsoleTracer::Debug(const std::string &message)
{
  if (std_out_ == INVALID_HANDLE_VALUE)
    return;
  SetConsoleTextAttribute(std_out_, FOREGROUND_INTENSITY | FOREGROUND_GREEN);
  std::string header("Debug: ");
  Info(header + message);
}

void ConsoleTracer::Warning(const std::string &message)
{
  if (std_out_ == INVALID_HANDLE_VALUE)
    return;
  SetConsoleTextAttribute(std_out_, FOREGROUND_INTENSITY | FOREGROUND_RED | FOREGROUND_GREEN);
  std::string header("Warning: ");
  Info(header + message);
}

void ConsoleTracer::Error(const std::string &message)
{
  if (std_out_ == INVALID_HANDLE_VALUE)
    return;
  SetConsoleTextAttribute(std_out_, FOREGROUND_INTENSITY | FOREGROUND_RED);
  std::string header("ERROR: ");
  Info(header + message);
}

void ConsoleTracer::Critical(const std::string &message)
{
  if (std_out_ == INVALID_HANDLE_VALUE)
    return;
  SetConsoleTextAttribute(std_out_, FOREGROUND_INTENSITY | FOREGROUND_RED | FOREGROUND_BLUE);
  std::string header("CRITICAL: ");
  Info(header + message);
}

void ConsoleTracer::Fatal(const std::string &message)
{
  if (std_out_ == INVALID_HANDLE_VALUE)
    return;
  SetConsoleTextAttribute(std_out_, BACKGROUND_RED | FOREGROUND_INTENSITY | FOREGROUND_RED);
  std::string header("*** FATAL ***: ");
  Info(header + message);
  // After the log, reset the color for any following console output.
  SetConsoleTextAttribute(std_out_, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
}

#else

void ConsoleTracer::Info(const std::string &message)
{
  std::cout << message;
}

void ConsoleTracer::Debug(const std::string &message)
{
  std::string header("Debug: ");
  Info(header + message);
}

void ConsoleTracer::Warning(const std::string &message)
{
  std::string header("Warning: ");
  Info(header + message);
}

void ConsoleTracer::Error(const std::string &message)
{
  std::string header("ERROR: ");
  Info(header + message);
}

void ConsoleTracer::Critical(const std::string &message)
{
  std::string header("CRITICAL: ");
  Info(header + message);
}

void ConsoleTracer::Fatal(const std::string &message)
{
  std::string header("*** FATAL ***: ");
  Info(header + message);
  //*((char *)0) = 0xDEADBEAF;
}
#endif

Log::Log()
{
  set_level(TraceSeverity::info);
#if ((defined(WIN32) || defined(__MINGW32__) || defined(__MINGW64__)))
  HMODULE isFromDll = GetModuleHandle(NULL);
  if (!isFromDll)
  {
    configure(TraceType::file);
  }
  else
#endif
    configure(TraceType::console);
}

void Log::set_level(TraceSeverity level)
{
  logging_level_ |= static_cast<uint32_t>(level);
}

void Log::clear_level(TraceSeverity level)
{
  logging_level_ &= ~static_cast<uint32_t>(level);
}

bool Log::is_severity_enabled(TraceSeverity level)
{
  return (logging_level_ & static_cast<uint32_t>(level)) != 0;
}

void Log::reset_levels()
{
  logging_level_ = 0;
}

void Log::configure(TraceType lt)
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
    // provide some kind of a log file path here
    instance_ = std::make_unique<FileTracer>();
    break;
  default:
    // not implemented yet
    break;
  }
}
