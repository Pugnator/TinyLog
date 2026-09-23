#include "format.hpp"

#include "platform.hpp"

#include <array>
#include <cctype>
#include <charconv>

namespace tinylog
{
  namespace
  {
    constexpr std::array<std::string_view, 8> level_names = {
        "trace", "debug", "info", "warning", "error", "critical", "fatal", "off"};

    bool iequals(std::string_view a, std::string_view b) noexcept
    {
      if (a.size() != b.size())
        return false;
      for (std::size_t i = 0; i < a.size(); ++i)
      {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
          return false;
      }
      return true;
    }

    void append_digits(std::string &out, unsigned value, int width)
    {
      char buffer[10];
      for (int i = width - 1; i >= 0; --i)
      {
        buffer[i] = static_cast<char>('0' + value % 10);
        value /= 10;
      }
      out.append(buffer, static_cast<std::size_t>(width));
    }

    void append_json_string(std::string &out, std::string_view text)
    {
      static constexpr char hex[] = "0123456789abcdef";
      out.push_back('"');
      std::size_t run = 0;
      for (std::size_t i = 0; i < text.size(); ++i)
      {
        const auto c = static_cast<unsigned char>(text[i]);
        if (c >= 0x20 && c != '"' && c != '\\')
          continue;
        out.append(text.data() + run, i - run);
        run = i + 1;
        switch (c)
        {
        case '"':
          out += "\\\"";
          break;
        case '\\':
          out += "\\\\";
          break;
        case '\n':
          out += "\\n";
          break;
        case '\r':
          out += "\\r";
          break;
        case '\t':
          out += "\\t";
          break;
        default:
          out += "\\u00";
          out.push_back(hex[c >> 4]);
          out.push_back(hex[c & 0xF]);
          break;
        }
      }
      out.append(text.data() + run, text.size() - run);
      out.push_back('"');
    }
  }

  std::string_view to_string(Level level) noexcept
  {
    const auto index = static_cast<std::size_t>(level);
    return index < level_names.size() ? level_names[index] : std::string_view("unknown");
  }

  std::optional<Level> parse_level(std::string_view text) noexcept
  {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
      text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
      text.remove_suffix(1);
    if (text.size() == 1 && text[0] >= '0' && text[0] <= '7')
      return static_cast<Level>(text[0] - '0');
    for (std::size_t i = 0; i < level_names.size(); ++i)
    {
      if (iequals(text, level_names[i]))
        return static_cast<Level>(i);
    }
    if (iequals(text, "warn"))
      return Level::warning;
    if (iequals(text, "err"))
      return Level::error;
    if (iequals(text, "crit"))
      return Level::critical;
    if (iequals(text, "verbose"))
      return Level::trace;
    if (iequals(text, "none"))
      return Level::off;
    return std::nullopt;
  }

  void format_record(const Record &record, const Layout &layout, std::string &out)
  {
    detail::Formatter formatter(layout);
    formatter.format(record, out);
  }

  Sink::~Sink() = default;
}

namespace tinylog::detail
{
  std::string_view ansi_color(Level level) noexcept
  {
    switch (level)
    {
    case Level::trace:
      return "\x1b[90m"; // bright black
    case Level::debug:
      return "\x1b[36m"; // cyan
    case Level::info:
      return "\x1b[32m"; // green
    case Level::warning:
      return "\x1b[1;33m"; // bold yellow
    case Level::error:
      return "\x1b[1;31m"; // bold red
    case Level::critical:
      return "\x1b[1;35m"; // bold magenta
    case Level::fatal:
      return "\x1b[1;37;41m"; // white on red
    default:
      return {};
    }
  }

  std::string_view trim_newline(std::string_view message) noexcept
  {
    if (!message.empty() && message.back() == '\n')
    {
      message.remove_suffix(1);
      if (!message.empty() && message.back() == '\r')
        message.remove_suffix(1);
    }
    return message;
  }

  std::string_view file_name(const char *path) noexcept
  {
    if (path == nullptr)
      return {};
    std::string_view view(path);
    const auto slash = view.find_last_of("/\\");
    return slash == std::string_view::npos ? view : view.substr(slash + 1);
  }

  void Formatter::append_timestamp(std::chrono::system_clock::time_point time, bool utc, bool iso, std::string &out)
  {
    using namespace std::chrono;
    const auto since_epoch = time.time_since_epoch();
    auto seconds = duration_cast<std::chrono::seconds>(since_epoch);
    if (seconds > since_epoch) // floor for times before 1970
      seconds -= std::chrono::seconds(1);
    const auto sub = duration_cast<microseconds>(since_epoch - seconds).count();

    if (seconds.count() != cached_second_ || utc != cached_utc_ || iso != cached_iso_)
    {
      const auto t = static_cast<std::time_t>(seconds.count());
      const std::tm tm = utc ? platform::utc_time(t) : platform::local_time(t);
      std::string text;
      text.reserve(20);
      append_digits(text, static_cast<unsigned>(tm.tm_year + 1900), 4);
      text.push_back('-');
      append_digits(text, static_cast<unsigned>(tm.tm_mon + 1), 2);
      text.push_back('-');
      append_digits(text, static_cast<unsigned>(tm.tm_mday), 2);
      text.push_back(iso ? 'T' : ' ');
      append_digits(text, static_cast<unsigned>(tm.tm_hour), 2);
      text.push_back(':');
      append_digits(text, static_cast<unsigned>(tm.tm_min), 2);
      text.push_back(':');
      append_digits(text, static_cast<unsigned>(tm.tm_sec), 2);
      cached_length_ = text.size();
      text.copy(cached_text_, cached_length_);
      cached_second_ = seconds.count();
      cached_utc_ = utc;
      cached_iso_ = iso;
    }
    out.append(cached_text_, cached_length_);
    switch (layout_.timestamp)
    {
    case TimestampPrecision::millis:
      out.push_back('.');
      append_digits(out, static_cast<unsigned>(sub / 1000), 3);
      break;
    case TimestampPrecision::micros:
      out.push_back('.');
      append_digits(out, static_cast<unsigned>(sub), 6);
      break;
    default:
      break;
    }
  }

  std::pair<std::size_t, std::size_t> Formatter::format_prefix(const Record &record, std::string &out)
  {
    std::pair<std::size_t, std::size_t> tag{out.size(), out.size()};
    if (layout_.timestamp != TimestampPrecision::none)
    {
      out.push_back('[');
      append_timestamp(record.time, layout_.utc, false, out);
      out += "] ";
    }
    if (layout_.show_level)
    {
      out.push_back('[');
      tag.first = out.size();
      out += to_string(record.level);
      tag.second = out.size();
      out += "] ";
    }
    if (layout_.show_thread)
    {
      out.push_back('[');
      char buffer[24];
      const auto result = std::to_chars(buffer, buffer + sizeof(buffer), record.thread_id);
      out.append(buffer, result.ptr);
      out += "] ";
    }
    if (layout_.show_source && record.where.file != nullptr)
    {
      out.push_back('[');
      out += file_name(record.where.file);
      out.push_back(':');
      char buffer[12];
      const auto result = std::to_chars(buffer, buffer + sizeof(buffer), record.where.line);
      out.append(buffer, result.ptr);
      out += "] ";
    }
    return tag;
  }

  void Formatter::format_json(const Record &record, std::string &out)
  {
    out += "{\"ts\":\"";
    append_timestamp(record.time, true, true, out);
    out += "Z\",\"level\":\"";
    out += to_string(record.level);
    out += "\",\"thread\":";
    char buffer[24];
    auto result = std::to_chars(buffer, buffer + sizeof(buffer), record.thread_id);
    out.append(buffer, result.ptr);
    if (record.where.file != nullptr)
    {
      out += ",\"file\":";
      append_json_string(out, file_name(record.where.file));
      out += ",\"line\":";
      result = std::to_chars(buffer, buffer + sizeof(buffer), record.where.line);
      out.append(buffer, result.ptr);
      if (record.where.function != nullptr)
      {
        out += ",\"func\":";
        append_json_string(out, record.where.function);
      }
    }
    out += ",\"msg\":";
    append_json_string(out, trim_newline(record.message));
    out += "}\n";
  }

  void Formatter::format(const Record &record, std::string &out)
  {
    if (layout_.format == Format::json)
    {
      format_json(record, out);
      return;
    }
    format_prefix(record, out);
    out += trim_newline(record.message);
    out.push_back('\n');
  }
}
