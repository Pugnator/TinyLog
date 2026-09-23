#pragma once

/*! \file Log records, levels, layouts and the sink interface. */

#include <tinylog/export.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace tinylog
{
  //! Message severity, ordered from the most to the least verbose.
  enum class Level : std::uint8_t
  {
    trace = 0,
    debug = 1,
    info = 2,
    warning = 3,
    error = 4,
    critical = 5,
    fatal = 6,
    off = 7,
  };

  //! Bit of a single level inside a level mask.
  constexpr std::uint32_t level_bit(Level level) noexcept
  {
    return level >= Level::off ? 0u : (1u << static_cast<unsigned>(level));
  }

  //! Mask enabling `level` and everything more severe (threshold semantics).
  constexpr std::uint32_t mask_from(Level level) noexcept
  {
    constexpr std::uint32_t all = (1u << static_cast<unsigned>(Level::off)) - 1u;
    return level >= Level::off ? 0u : (all << static_cast<unsigned>(level)) & all;
  }

  //! Lower-case level name: "trace", "debug", "info", "warning", ...
  TINYLOG_API std::string_view to_string(Level level) noexcept;

  //! Parses a level name ("warn", "WARNING", "err", ...) or digit "0".."7".
  TINYLOG_API std::optional<Level> parse_level(std::string_view text) noexcept;

  //! Where a record was produced. Pointers must outlive the process (string literals).
  struct SourceLocation
  {
    const char *file = nullptr;
    const char *function = nullptr;
    std::uint32_t line = 0;
  };

  //! One log event as seen by a sink. The message excludes the trailing newline.
  struct Record
  {
    Level level = Level::info;
    std::chrono::system_clock::time_point time{};
    std::uint64_t thread_id = 0;
    SourceLocation where{};
    std::string_view message{};
  };

  enum class Format : std::uint8_t
  {
    text, //!< "[2026-09-23 10:15:02.123] [info] message"
    json, //!< One JSON object per line (JSON Lines), timestamps in UTC.
  };

  enum class TimestampPrecision : std::uint8_t
  {
    none,
    seconds,
    millis,
    micros,
  };

  //! How a record is rendered into a line of text.
  struct Layout
  {
    Format format = Format::text;
    TimestampPrecision timestamp = TimestampPrecision::millis;
    //! UTC instead of local time (text format; JSON is always UTC).
    bool utc = false;
    bool show_level = true;
    bool show_thread = false;
    //! Append "file:line" of the call site.
    bool show_source = false;
  };

  /**
   * @brief Renders `record` according to `layout` and appends it, newline included, to `out`.
   *
   * Exposed so custom sinks produce output identical to the built-in ones.
   */
  TINYLOG_API void format_record(const Record &record, const Layout &layout, std::string &out);

  /**
   * @brief A destination for log records.
   *
   * The core never calls one sink from two threads at once, so implementations
   * need no locking of their own. write() may buffer; flush() must make
   * everything written so far visible to readers (it is not an fsync).
   * Exceptions thrown by a sink are caught and reported, never propagated.
   */
  class TINYLOG_API Sink
  {
  public:
    virtual ~Sink();
    virtual void write(const Record &record) = 0;
    virtual void flush() {}
    //! Waits for background work (compression, retention) started by this sink.
    virtual void wait_idle() {}
    //! Stops background work; called by shutdown() before the process exits.
    virtual void stop_background() {}

    //! Records below this level are not passed to write().
    Level min_level = Level::trace;
  };

  //! Sink forwarding every record to a function; handy for GUIs and tests.
  class CallbackSink final : public Sink
  {
  public:
    using Callback = std::function<void(const Record &)>;
    explicit CallbackSink(Callback callback) : callback_(std::move(callback)) {}
    void write(const Record &record) override
    {
      if (callback_)
        callback_(record);
    }

  private:
    Callback callback_;
  };
}
