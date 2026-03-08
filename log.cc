#include "log.hpp"
#include <chrono>
#include <ctime>
#include <sstream>
#include <vector>
#include <algorithm>
#include <stdexcept>

#include <zstd.h>

namespace
{
  std::string current_timestamp()
  {
    std::time_t now = std::time(nullptr);
    std::tm tm_snapshot{};
#if ((defined(WIN32) || defined(__MINGW32__) || defined(__MINGW64__)))
    localtime_s(&tm_snapshot, &now);
#else
    localtime_r(&now, &tm_snapshot);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm_snapshot, "[%Y-%m-%d %H:%M:%S] ");
    return oss.str();
  }
}

void VoidTracer::Info(const std::string &message) {}
void VoidTracer::Debug(const std::string &message) {}
void VoidTracer::Warning(const std::string &message) {}
void VoidTracer::Critical(const std::string &message) {}
void VoidTracer::Error(const std::string &message) {}
void VoidTracer::Fatal(const std::string &message) {}

// ---------------------------------------------------------------------------
// FileTracer – construction / destruction
// ---------------------------------------------------------------------------

FileTracer::FileTracer(const std::string &filepath, const RotationConfig &rotation)
    : filepath_(filepath), rotation_(rotation)
{
  open_log_file();
}

FileTracer::~FileTracer()
{
  if (file_handle_.is_open())
    file_handle_.close();
}

void FileTracer::open_log_file()
{
  file_handle_ = std::ofstream(filepath_, std::ios::app | std::ios::binary);
  if (!file_handle_.is_open())
  {
    throw std::runtime_error("Failed to open log file: " + filepath_.string());
  }
  // Track existing file size so rotation triggers correctly on an append.
  if (std::filesystem::exists(filepath_))
  {
    current_size_ = static_cast<std::size_t>(std::filesystem::file_size(filepath_));
  }
  else
  {
    current_size_ = 0;
  }
}

// ---------------------------------------------------------------------------
// FileTracer – rotation helpers
// ---------------------------------------------------------------------------

void FileTracer::maybe_rotate()
{
  if (rotation_.max_file_size == 0)
    return; // rotation disabled
  if (current_size_ >= rotation_.max_file_size)
    rotate();
}

void FileTracer::rotate()
{
  // Close the current file before renaming.
  if (file_handle_.is_open())
    file_handle_.close();

  // Shift existing backups:  log.N -> log.N+1  (highest first to avoid collisions).
  // We look for both plain and .zst variants.
  const auto stem = filepath_.stem().string();
  const auto ext  = filepath_.extension().string();
  const auto dir  = filepath_.parent_path().empty() ? std::filesystem::current_path() : filepath_.parent_path();

  // Shift existing backups upward.
  // Start from max_backup_count-1 down to 1 so nothing is overwritten.
  if (rotation_.max_backup_count > 0)
  {
    for (std::size_t i = rotation_.max_backup_count - 1; i >= 1; --i)
    {
      auto src_plain = dir / (stem + "." + std::to_string(i) + ext);
      auto src_zst   = dir / (stem + "." + std::to_string(i) + ext + ".zst");
      auto dst_plain = dir / (stem + "." + std::to_string(i + 1) + ext);
      auto dst_zst   = dir / (stem + "." + std::to_string(i + 1) + ext + ".zst");

      std::error_code ec;
      if (std::filesystem::exists(src_zst, ec))
        std::filesystem::rename(src_zst, dst_zst, ec);
      else if (std::filesystem::exists(src_plain, ec))
        std::filesystem::rename(src_plain, dst_plain, ec);
    }
  }

  // Rename the current active file to .1
  {
    auto dst = dir / (stem + ".1" + ext);
    std::error_code ec;
    std::filesystem::rename(filepath_, dst, ec);

    // Compress the freshly-rotated file if requested.
    if (!ec && rotation_.compress)
    {
      compress_file_zstd(dst);
    }
  }

  prune_old_backups();

  // Re-open a fresh active log file.
  open_log_file();
}

bool FileTracer::compress_file_zstd(const std::filesystem::path &src)
{
  // Read the entire source file into memory.
  std::ifstream ifs(src, std::ios::binary | std::ios::ate);
  if (!ifs.is_open())
    return false;

  const auto src_size = static_cast<std::size_t>(ifs.tellg());
  ifs.seekg(0, std::ios::beg);

  std::vector<char> src_buf(src_size);
  if (!ifs.read(src_buf.data(), static_cast<std::streamsize>(src_size)))
    return false;
  ifs.close();

  // Allocate destination buffer.
  const std::size_t dst_capacity = ZSTD_compressBound(src_size);
  std::vector<char> dst_buf(dst_capacity);

  // Compress at maximum level.
  const int max_level = ZSTD_maxCLevel();
  const std::size_t compressed_size =
      ZSTD_compress(dst_buf.data(), dst_capacity,
                    src_buf.data(), src_size,
                    max_level);

  if (ZSTD_isError(compressed_size))
    return false;

  // Write compressed data alongside the original name with .zst suffix.
  const auto dst_path = std::filesystem::path(src.string() + ".zst");
  std::ofstream ofs(dst_path, std::ios::binary | std::ios::trunc);
  if (!ofs.is_open())
    return false;

  ofs.write(dst_buf.data(), static_cast<std::streamsize>(compressed_size));
  ofs.close();

  // Remove the uncompressed source.
  std::error_code ec;
  std::filesystem::remove(src, ec);
  return true;
}

void FileTracer::prune_old_backups()
{
  if (rotation_.max_backup_count == 0)
    return; // unlimited

  const auto stem = filepath_.stem().string();
  const auto ext  = filepath_.extension().string();
  const auto dir  = filepath_.parent_path().empty() ? std::filesystem::current_path() : filepath_.parent_path();

  for (std::size_t i = rotation_.max_backup_count + 1; ; ++i)
  {
    auto plain = dir / (stem + "." + std::to_string(i) + ext);
    auto zst   = dir / (stem + "." + std::to_string(i) + ext + ".zst");
    std::error_code ec;
    bool removed = false;
    if (std::filesystem::exists(plain, ec)) { std::filesystem::remove(plain, ec); removed = true; }
    if (std::filesystem::exists(zst, ec))   { std::filesystem::remove(zst, ec);   removed = true; }
    if (!removed)
      break; // no more old backups to prune
  }
}

// ---------------------------------------------------------------------------
// FileTracer – severity methods
// ---------------------------------------------------------------------------

void FileTracer::Info(const std::string &message)
{
  std::lock_guard<std::mutex> lock(mutex_);
  const std::string line = current_timestamp() + message;
  file_handle_ << line;
  file_handle_.flush();
  current_size_ += line.size();
  maybe_rotate();
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
  std::string header("*** FATAL ***: ");
  Info(header + message);
}

#if ((defined(WIN32) || defined(__MINGW32__) || defined(__MINGW64__)))

void ConsoleTracer::write_impl(const std::string &formatted)
{
  if (std_out_ == INVALID_HANDLE_VALUE)
    return;
  DWORD dwBytesWritten;
  WriteConsoleA(std_out_, formatted.c_str(), static_cast<DWORD>(formatted.length()), &dwBytesWritten, NULL);
}

void ConsoleTracer::Info(const std::string &message)
{
  std::lock_guard<std::mutex> lock(mutex_);
  if (std_out_ == INVALID_HANDLE_VALUE)
    return;
  SetConsoleTextAttribute(std_out_, FOREGROUND_INTENSITY | FOREGROUND_GREEN | FOREGROUND_BLUE);
  write_impl(current_timestamp() + message);
}

void ConsoleTracer::Debug(const std::string &message)
{
  std::lock_guard<std::mutex> lock(mutex_);
  if (std_out_ == INVALID_HANDLE_VALUE)
    return;
  SetConsoleTextAttribute(std_out_, FOREGROUND_INTENSITY | FOREGROUND_GREEN);
  write_impl(current_timestamp() + "Debug: " + message);
}

void ConsoleTracer::Warning(const std::string &message)
{
  std::lock_guard<std::mutex> lock(mutex_);
  if (std_out_ == INVALID_HANDLE_VALUE)
    return;
  SetConsoleTextAttribute(std_out_, FOREGROUND_INTENSITY | FOREGROUND_RED | FOREGROUND_GREEN);
  write_impl(current_timestamp() + "Warning: " + message);
}

void ConsoleTracer::Error(const std::string &message)
{
  std::lock_guard<std::mutex> lock(mutex_);
  if (std_out_ == INVALID_HANDLE_VALUE)
    return;
  SetConsoleTextAttribute(std_out_, FOREGROUND_INTENSITY | FOREGROUND_RED);
  write_impl(current_timestamp() + "ERROR: " + message);
}

void ConsoleTracer::Critical(const std::string &message)
{
  std::lock_guard<std::mutex> lock(mutex_);
  if (std_out_ == INVALID_HANDLE_VALUE)
    return;
  SetConsoleTextAttribute(std_out_, FOREGROUND_INTENSITY | FOREGROUND_RED | FOREGROUND_BLUE);
  write_impl(current_timestamp() + "CRITICAL: " + message);
}

void ConsoleTracer::Fatal(const std::string &message)
{
  std::lock_guard<std::mutex> lock(mutex_);
  if (std_out_ == INVALID_HANDLE_VALUE)
    return;
  SetConsoleTextAttribute(std_out_, BACKGROUND_RED | FOREGROUND_INTENSITY | FOREGROUND_RED);
  write_impl(current_timestamp() + "*** FATAL ***: " + message);
  SetConsoleTextAttribute(std_out_, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
}

#else

void ConsoleTracer::write_impl(const std::string &formatted)
{
  std::cout << formatted;
}

void ConsoleTracer::Info(const std::string &message)
{
  std::lock_guard<std::mutex> lock(mutex_);
  write_impl(current_timestamp() + message);
}

void ConsoleTracer::Debug(const std::string &message)
{
  std::lock_guard<std::mutex> lock(mutex_);
  write_impl(current_timestamp() + "Debug: " + message);
}

void ConsoleTracer::Warning(const std::string &message)
{
  std::lock_guard<std::mutex> lock(mutex_);
  write_impl(current_timestamp() + "Warning: " + message);
}

void ConsoleTracer::Error(const std::string &message)
{
  std::lock_guard<std::mutex> lock(mutex_);
  write_impl(current_timestamp() + "ERROR: " + message);
}

void ConsoleTracer::Critical(const std::string &message)
{
  std::lock_guard<std::mutex> lock(mutex_);
  write_impl(current_timestamp() + "CRITICAL: " + message);
}

void ConsoleTracer::Fatal(const std::string &message)
{
  std::lock_guard<std::mutex> lock(mutex_);
  write_impl(current_timestamp() + "*** FATAL ***: " + message);
}
#endif

Log::Log()
{
  set_level(TraceSeverity::info);
#if ((defined(WIN32) || defined(__MINGW32__) || defined(__MINGW64__)))
  HMODULE isFromDll = GetModuleHandle(NULL);
  if (!isFromDll)
  {
    configure_impl(TraceType::file);
  }
  else
#endif
    configure_impl(TraceType::console);
}

Log& Log::set_level(TraceSeverity level)
{
  logging_level_.fetch_or(static_cast<uint32_t>(level));
  return *this;
}

Log& Log::clear_level(TraceSeverity level)
{
  logging_level_.fetch_and(~static_cast<uint32_t>(level));
  return *this;
}

bool Log::is_severity_enabled(TraceSeverity level)
{
  return (logging_level_.load(std::memory_order_relaxed) & static_cast<uint32_t>(level)) != 0;
}

Log& Log::reset_levels()
{
  logging_level_.store(0);
  return *this;
}

void Log::configure_impl(TraceType lt)
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

Log& Log::configure(TraceType lt)
{
  std::unique_lock<std::shared_mutex> lock(instance_mutex_);
  configure_impl(lt);
  return *this;
}

Log& Log::configure(TraceType lt, const std::string &filepath)
{
  std::unique_lock<std::shared_mutex> lock(instance_mutex_);
  if (lt == TraceType::file && !filepath.empty())
  {
    instance_ = std::make_unique<FileTracer>(filepath);
  }
  else
  {
    configure_impl(lt);
  }
  return *this;
}

Log& Log::configure(TraceType lt, const std::string &filepath, const RotationConfig &rotation)
{
  std::unique_lock<std::shared_mutex> lock(instance_mutex_);
  if (lt == TraceType::file && !filepath.empty())
  {
    instance_ = std::make_unique<FileTracer>(filepath, rotation);
  }
  else
  {
    configure_impl(lt);
  }
  return *this;
}
