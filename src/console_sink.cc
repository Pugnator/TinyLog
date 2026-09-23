#include "sinks.hpp"

#include "format.hpp"
#include "platform.hpp"

namespace tinylog::detail
{
  namespace
  {
    bool want_color(ColorMode mode, const platform::Console &console)
    {
      switch (mode)
      {
      case ColorMode::always:
        return true;
      case ColorMode::never:
        return false;
      default:
        // https://no-color.org: any non-empty NO_COLOR disables colour.
        if (const auto no_color = platform::getenv("NO_COLOR"); no_color && !no_color->empty())
          return false;
        return console.is_terminal() && console.supports_ansi();
      }
    }
  }

  ConsoleSink::ConsoleSink(const ConsoleSinkConfig &config)
      : console_(std::make_unique<platform::Console>(config.stream == ConsoleStream::err)),
        formatter_(config.layout)
  {
    min_level = config.min_level;
    color_ = config.layout.format == Format::text && want_color(config.color, *console_);
    buffer_.reserve(4096);
  }

  ConsoleSink::~ConsoleSink()
  {
    flush();
  }

  void ConsoleSink::write(const Record &record)
  {
    if (!color_)
    {
      formatter_.format(record, buffer_);
    }
    else
    {
      const auto tag = formatter_.format_prefix(record, buffer_);
      if (tag.first != tag.second)
      {
        const auto color = ansi_color(record.level);
        buffer_.insert(tag.first, color);
        buffer_.insert(tag.second + color.size(), ansi_reset);
      }
      buffer_ += trim_newline(record.message);
      buffer_.push_back('\n');
    }
    // Terminals are slow; bound the batch so output keeps flowing under load.
    if (buffer_.size() >= 32 * 1024)
      flush();
  }

  void ConsoleSink::flush()
  {
    if (buffer_.empty())
      return;
    console_->write(buffer_);
    buffer_.clear();
  }
}

namespace tinylog
{
  std::shared_ptr<Sink> make_console_sink(const ConsoleSinkConfig &config)
  {
    return std::make_shared<detail::ConsoleSink>(config);
  }
}
