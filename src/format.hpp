#pragma once

/*! \file Record rendering with a per-second timestamp cache. Internal. */

#include <tinylog/sink.hpp>

#include <cstdint>
#include <string>

namespace tinylog::detail
{
  //! Colour for a level as an ANSI SGR sequence ("" for none).
  std::string_view ansi_color(Level level) noexcept;
  inline constexpr std::string_view ansi_reset = "\x1b[0m";

  /**
   * @brief Renders records for one sink.
   *
   * Converting a time to calendar fields costs a system call; it is done once
   * per second and the text reused. Not thread-safe (each sink owns one).
   */
  class Formatter
  {
  public:
    explicit Formatter(const Layout &layout = {}) : layout_(layout) {}
    const Layout &layout() const noexcept { return layout_; }

    //! Appends the rendered record plus '\n' to `out`.
    void format(const Record &record, std::string &out);

    /**
     * @brief Text layout split around the message, so a sink can colour the
     * level tag: out = prefix, then the message and '\n' are appended by the caller.
     * Returns the [begin, end) range of the level tag inside `out` (empty if hidden).
     */
    std::pair<std::size_t, std::size_t> format_prefix(const Record &record, std::string &out);

  private:
    void append_timestamp(std::chrono::system_clock::time_point time, bool utc, bool iso, std::string &out);
    void format_json(const Record &record, std::string &out);

    Layout layout_;
    std::int64_t cached_second_ = INT64_MIN;
    bool cached_utc_ = false;
    bool cached_iso_ = false;
    char cached_text_[32] = {};
    std::size_t cached_length_ = 0;
  };

  //! Message without one trailing "\n" / "\r\n": callers of the old API ended lines themselves.
  std::string_view trim_newline(std::string_view message) noexcept;

  //! Basename of a __FILE__ path.
  std::string_view file_name(const char *path) noexcept;
}
