#include "log.hpp"

void VoidTracer::Info(const std::string &message) {}
void VoidTracer::Debug(const std::string &message) {}
void VoidTracer::Warning(const std::string &message) {}
void VoidTracer::Critical(const std::string &message) {}
void VoidTracer::Error(const std::string &message) {}
void VoidTracer::Fatal(const std::string &message) {}

void FileTracer::Info(const std::string &message) {}
void FileTracer::Debug(const std::string &message) {}
void FileTracer::Warning(const std::string &message) {}
void FileTracer::Critical(const std::string &message) {}
void FileTracer::Error(const std::string &message) {}
void FileTracer::Fatal(const std::string &message) {}

#if ((defined(WIN32) || defined(__MINGW32__) || defined(__MINGW64__)))
    
void ConsoleTracer::Info(const std::string &message)
{
  if (std_out_ == INVALID_HANDLE_VALUE)
  {
    return;
  }
  DWORD dwBytesWritten;
  WriteConsole(std_out_, message.c_str(), message.length(), &dwBytesWritten, NULL);
};

#endif //#if defined(WIN32) || #if defined(__MINGW32__) || #if defined(__MINGW64__)

void ConsoleTracer::Debug(const std::string &message)
{
  std::string header("Debug: ");
}

void ConsoleTracer::Warning(const std::string &message)
{
  std::string header("Warning: ");
}

void ConsoleTracer::Critical(const std::string &message)
{
  std::string header("CRITICAL: ");
}

void ConsoleTracer::Error(const std::string &message)
{
  std::string header("ERROR: ");
}

void ConsoleTracer::Fatal(const std::string &message)
{
  std::string header("*** FATAL ***: ");
  //*((char *)0) = 0xDEADBEAF;
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
    instance_ = std::make_unique<FileTracer>();
    break;
  default:
    break;
  }
}
