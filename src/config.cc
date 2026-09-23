#include <tinylog/config.hpp>

#include <cctype>
#include <charconv>
#include <limits>
#include <stdexcept>
#include <string>

namespace tinylog
{
  namespace
  {
    std::string_view trim(std::string_view text)
    {
      while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
        text.remove_prefix(1);
      while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
        text.remove_suffix(1);
      return text;
    }

    std::string lower(std::string_view text)
    {
      std::string out(text);
      for (auto &c : out)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      return out;
    }

    [[noreturn]] void fail(std::string_view key, std::string_view value, std::string_view expected)
    {
      throw std::invalid_argument("tinylog config: bad value '" + std::string(value) + "' for '" +
                                  std::string(key) + "' (expected " + std::string(expected) + ")");
    }

    //! Number followed by an optional suffix; returns the suffix.
    std::uint64_t number(std::string_view key, std::string_view value, std::string_view &suffix)
    {
      std::uint64_t result = 0;
      const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
      if (parsed.ec != std::errc() || parsed.ptr == value.data())
        fail(key, value, "a number");
      suffix = trim(std::string_view(parsed.ptr, static_cast<std::size_t>(value.data() + value.size() - parsed.ptr)));
      return result;
    }

    std::uint64_t size_value(std::string_view key, std::string_view value)
    {
      std::string_view suffix;
      const std::uint64_t n = number(key, value, suffix);
      const std::string unit = lower(suffix);
      std::uint64_t scale = 1;
      if (unit.empty() || unit == "b")
        scale = 1;
      else if (unit == "k" || unit == "kb" || unit == "kib")
        scale = 1ull << 10;
      else if (unit == "m" || unit == "mb" || unit == "mib")
        scale = 1ull << 20;
      else if (unit == "g" || unit == "gb" || unit == "gib")
        scale = 1ull << 30;
      else
        fail(key, value, "a size such as 512K, 10M or 1G");
      if (n > std::numeric_limits<std::uint64_t>::max() / scale)
        fail(key, value, "a smaller size");
      return n * scale;
    }

    std::chrono::milliseconds duration_value(std::string_view key, std::string_view value)
    {
      std::string_view suffix;
      const std::uint64_t n = number(key, value, suffix);
      const std::string unit = lower(suffix);
      using namespace std::chrono;
      if (unit == "ms")
        return milliseconds(n);
      if (unit.empty() || unit == "s")
        return seconds(n);
      if (unit == "m" || unit == "min")
        return minutes(n);
      if (unit == "h")
        return hours(n);
      if (unit == "d")
        return hours(24 * n);
      fail(key, value, "a duration such as 500ms, 30s, 15m, 1h or 1d");
    }

    bool bool_value(std::string_view key, std::string_view value)
    {
      const std::string v = lower(value);
      if (v == "1" || v == "true" || v == "yes" || v == "on")
        return true;
      if (v == "0" || v == "false" || v == "no" || v == "off")
        return false;
      fail(key, value, "true or false");
    }

    int int_value(std::string_view key, std::string_view value)
    {
      int result = 0;
      const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
      if (parsed.ec != std::errc() || parsed.ptr != value.data() + value.size())
        fail(key, value, "an integer");
      return result;
    }

    Level level_value(std::string_view key, std::string_view value)
    {
      const auto level = parse_level(value);
      if (!level)
        fail(key, value, "trace, debug, info, warning, error, critical, fatal or off");
      return *level;
    }

    Compression compression_value(std::string_view key, std::string_view value)
    {
      const std::string v = lower(value);
      if (v == "zstd" || v == "true" || v == "1" || v == "on")
        return Compression::zstd;
      if (v == "none" || v == "false" || v == "0" || v == "off")
        return Compression::none;
      fail(key, value, "zstd or none");
    }

    //! Applies a layout key to every sink layout and to the one later file= entries start from.
    template <typename F>
    void each_layout(Config &config, Layout &next_file, F &&apply)
    {
      apply(next_file);
      if (config.console)
        apply(config.console->layout);
      for (auto &file : config.files)
        apply(file.layout);
    }
  }

  void apply_config_string(Config &config, std::string_view text)
  {
    // Layout keys apply to the sinks configured so far and to files added later.
    Layout next_file = config.console ? config.console->layout : Layout{};
    while (!text.empty())
    {
      const auto end = text.find_first_of(";\n");
      std::string_view entry = trim(text.substr(0, end));
      text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
      if (entry.empty() || entry.front() == '#')
        continue;

      const auto equals = entry.find('=');
      if (equals == std::string_view::npos)
        throw std::invalid_argument("tinylog config: expected key=value, got '" + std::string(entry) + "'");
      const std::string key = lower(trim(entry.substr(0, equals)));
      const std::string_view value = trim(entry.substr(equals + 1));

      auto file = [&]() -> FileSinkConfig &
      {
        if (config.files.empty())
          throw std::invalid_argument("tinylog config: '" + key + "' needs a preceding file=<path>");
        return config.files.back();
      };

      if (key == "level")
        config.level = level_value(key, value);
      else if (key == "mode")
      {
        const std::string v = lower(value);
        if (v == "sync")
          config.mode = Mode::sync;
        else if (v == "async")
          config.mode = Mode::async;
        else
          fail(key, value, "sync or async");
      }
      else if (key == "queue")
        config.queue_capacity = static_cast<std::size_t>(size_value(key, value));
      else if (key == "overflow")
      {
        const std::string v = lower(value);
        if (v == "block")
          config.overflow = OverflowPolicy::block;
        else if (v == "drop")
          config.overflow = OverflowPolicy::drop;
        else
          fail(key, value, "block or drop");
      }
      else if (key == "flush")
        config.flush_level = level_value(key, value);
      else if (key == "flush_interval")
        config.flush_interval = duration_value(key, value);
      else if (key == "console")
      {
        const std::string v = lower(value);
        if (v == "off" || v == "none" || v == "false" || v == "0")
          config.console.reset();
        else
        {
          if (!config.console)
            config.console = ConsoleSinkConfig{};
          if (v == "stdout" || v == "out" || v == "on" || v == "true" || v == "1")
            config.console->stream = ConsoleStream::out;
          else if (v == "stderr" || v == "err")
            config.console->stream = ConsoleStream::err;
          else
            fail(key, value, "stdout, stderr or off");
        }
      }
      else if (key == "console.color")
      {
        if (!config.console)
          config.console = ConsoleSinkConfig{};
        const std::string v = lower(value);
        if (v == "auto")
          config.console->color = ColorMode::automatic;
        else if (v == "always" || v == "true" || v == "on")
          config.console->color = ColorMode::always;
        else if (v == "never" || v == "false" || v == "off")
          config.console->color = ColorMode::never;
        else
          fail(key, value, "auto, always or never");
      }
      else if (key == "console.level")
      {
        if (!config.console)
          config.console = ConsoleSinkConfig{};
        config.console->min_level = level_value(key, value);
      }
      else if (key == "format")
      {
        const std::string v = lower(value);
        Format format = Format::text;
        if (v == "json")
          format = Format::json;
        else if (v != "text")
          fail(key, value, "text or json");
        each_layout(config, next_file, [&](Layout &layout)
                    { layout.format = format; });
      }
      else if (key == "timestamp")
      {
        const std::string v = lower(value);
        TimestampPrecision precision{};
        if (v == "none" || v == "off")
          precision = TimestampPrecision::none;
        else if (v == "s" || v == "seconds")
          precision = TimestampPrecision::seconds;
        else if (v == "ms" || v == "millis")
          precision = TimestampPrecision::millis;
        else if (v == "us" || v == "micros")
          precision = TimestampPrecision::micros;
        else
          fail(key, value, "none, s, ms or us");
        each_layout(config, next_file, [&](Layout &layout)
                    { layout.timestamp = precision; });
      }
      else if (key == "utc" || key == "thread" || key == "source" || key == "show_level")
      {
        const bool on = bool_value(key, value);
        each_layout(config, next_file, [&](Layout &layout)
                    {
                      if (key == "utc")
                        layout.utc = on;
                      else if (key == "thread")
                        layout.show_thread = on;
                      else if (key == "source")
                        layout.show_source = on;
                      else
                        layout.show_level = on; });
      }
      else if (key == "file")
      {
        FileSinkConfig sink;
        sink.layout = next_file;
        sink.path = std::filesystem::path(std::u8string(value.begin(), value.end()));
        config.files.push_back(std::move(sink));
      }
      else if (key == "file.level")
        file().min_level = level_value(key, value);
      else if (key == "file.compress")
        file().compression = compression_value(key, value);
      else if (key == "file.compress_level")
        file().compression_level = int_value(key, value);
      else if (key == "file.frame")
        file().frame_size = static_cast<std::size_t>(size_value(key, value));
      else if (key == "file.buffer")
        file().buffer_size = static_cast<std::size_t>(size_value(key, value));
      else if (key == "file.truncate")
        file().truncate = bool_value(key, value);
      else if (key == "file.format")
      {
        const std::string v = lower(value);
        if (v == "json")
          file().layout.format = Format::json;
        else if (v == "text")
          file().layout.format = Format::text;
        else
          fail(key, value, "text or json");
      }
      else if (key == "rotate.size")
        file().rotation.max_size = size_value(key, value);
      else if (key == "rotate.interval")
        file().rotation.interval = std::chrono::duration_cast<std::chrono::seconds>(duration_value(key, value));
      else if (key == "rotate.keep")
      {
        const int keep = int_value(key, value);
        if (keep < 0)
          fail(key, value, "0 (keep all) or more");
        file().rotation.max_files = static_cast<std::size_t>(keep);
      }
      else if (key == "rotate.on_open")
        file().rotation.on_open = bool_value(key, value);
      else if (key == "rotate.compress")
        file().rotation.compress = compression_value(key, value);
      else if (key == "rotate.compress_level")
        file().rotation.compress_level = int_value(key, value);
      else
        throw std::invalid_argument("tinylog config: unknown key '" + key + "'");
    }
  }

  Config parse_config(std::string_view text)
  {
    Config config;
    apply_config_string(config, text);
    return config;
  }
}
