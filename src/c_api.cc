#include <tinylog/tinylog.h>
#include <tinylog/tinylog.hpp>

#include <string>

namespace
{
  thread_local std::string t_last_error;

  tinylog::Level clamp_level(int level) noexcept
  {
    if (level < 0)
      return tinylog::Level::trace;
    if (level > static_cast<int>(tinylog::Level::off))
      return tinylog::Level::off;
    return static_cast<tinylog::Level>(level);
  }
}

extern "C"
{
  int tinylog_init(const char *settings)
  {
    try
    {
      t_last_error.clear();
      tinylog::init(tinylog::parse_config(settings != nullptr ? settings : ""));
      return 0;
    }
    catch (const std::exception &e)
    {
      t_last_error = e.what();
    }
    catch (...)
    {
      t_last_error = "unknown error";
    }
    return -1;
  }

  const char *tinylog_last_error(void) { return t_last_error.c_str(); }
  void tinylog_shutdown(void) { tinylog::shutdown(); }
  void tinylog_flush(void) { tinylog::flush(); }
  void tinylog_set_level(int level) { tinylog::set_level(clamp_level(level)); }
  int tinylog_should_log(int level) { return tinylog::should_log(clamp_level(level)) ? 1 : 0; }
  const char *tinylog_version(void) { return TINYLOG_VERSION_STRING; }

  void tinylog_write(int level, const char *file, int line, const char *function, const char *message,
                     size_t length)
  {
    const tinylog::SourceLocation where{file, function, static_cast<std::uint32_t>(line < 0 ? 0 : line)};
    tinylog::write(clamp_level(level), std::string_view(message != nullptr ? message : "", message != nullptr ? length : 0),
                   where);
  }
}
