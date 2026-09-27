#pragma once

/*! \file tinylog - a small, fast C++20 logger with zstd-compressed rotation.
 *
 * \code
 *   tinylog::Config cfg;
 *   cfg.level = tinylog::Level::debug;
 *   cfg.mode = tinylog::Mode::async;
 *   auto &file = cfg.files.emplace_back();
 *   file.path = "app.log";
 *   file.rotation.max_size = 10 << 20;
 *   file.rotation.compress = tinylog::Compression::zstd;
 *   tinylog::Guard guard(cfg);            // init now, flush + stop at scope exit
 *
 *   TLOG_INFO("listening on port {}", port);
 * \endcode
 */

#include <tinylog/config.hpp>
#include <tinylog/export.hpp>
#include <tinylog/sink.hpp>
#include <tinylog/version.hpp>

#include <atomic>
#include <cstdint>
#include <format>
#include <memory>
#include <string>
#include <string_view>

namespace tinylog
{
  namespace detail
  {
    //! Enabled levels, one bit per Level. Read on every log call; lives in the library.
    extern TINYLOG_API std::atomic<std::uint32_t> g_level_mask;

    TINYLOG_API void init_checked(const Config &config, int header_version);
    TINYLOG_API void vlog(Level level, const SourceLocation &where, std::string_view format,
                          std::format_args args) noexcept;

    //! A channel's shared state; Channel handles point at it. Opaque to users.
    struct ChannelState
    {
      std::string name;
      std::uint32_t id = 0;
      std::atomic<std::uint32_t> level_mask{0};
    };
    TINYLOG_API void channel_vlog(const ChannelState &channel, Level level, const SourceLocation &where,
                                  std::string_view format, std::format_args args) noexcept;
    TINYLOG_API void channel_write(const ChannelState &channel, Level level, std::string_view message,
                                   const SourceLocation &where) noexcept;
  }

  //! Library version, e.g. "0.2.0". Compare with TINYLOG_VERSION_STRING to detect a stale DLL.
  TINYLOG_API std::string_view version() noexcept;
  //! Library version as TINYLOG_VERSION_NUMBER.
  TINYLOG_API int version_number() noexcept;

  /**
   * @brief Configures the logger, replacing any previous configuration.
   *
   * May be called again at any time; pending messages are delivered to the old
   * sinks first. Until init() is called the logger writes `info` and above to
   * stdout. Also verifies the headers match the compiled library.
   *
   * @throws std::system_error if a log file cannot be opened,
   *         std::invalid_argument on a malformed TINYLOG_CONFIG,
   *         std::runtime_error on a header/library version mismatch.
   */
  inline void init(const Config &config = {})
  {
    detail::init_checked(config, TINYLOG_VERSION_NUMBER);
  }

  /**
   * @brief Delivers pending messages, flushes and stops background threads.
   *
   * Call it before main() returns, and before unloading a DLL that logs:
   * worker threads must not be joined from DllMain or static destructors.
   * Logging still works afterwards, synchronously.
   */
  TINYLOG_API void shutdown() noexcept;

  //! Delivers every message logged so far and flushes all sinks.
  TINYLOG_API void flush() noexcept;

  //! flush(), then waits for background compression and retention to finish.
  TINYLOG_API void wait_idle() noexcept;

  //! Enables `level` and everything more severe, disables the rest.
  TINYLOG_API void set_level(Level level) noexcept;
  //! The least severe enabled level (Level::off when nothing is enabled).
  TINYLOG_API Level level() noexcept;
  //! Enables a single level, leaving the others untouched.
  TINYLOG_API void enable(Level level) noexcept;
  //! Disables a single level, leaving the others untouched.
  TINYLOG_API void disable(Level level) noexcept;
  //! Replaces the whole mask; bit N enables Level(N).
  TINYLOG_API void set_level_mask(std::uint32_t mask) noexcept;
  TINYLOG_API std::uint32_t level_mask() noexcept;

  //! True if a message of this level would be logged. One relaxed atomic load.
  inline bool should_log(Level level) noexcept
  {
    return (detail::g_level_mask.load(std::memory_order_relaxed) & level_bit(level)) != 0;
  }

  //! Adds a sink to the running configuration.
  TINYLOG_API void add_sink(std::shared_ptr<Sink> sink);

  //! Counters since the last init().
  struct Stats
  {
    std::uint64_t logged = 0;     //!< Records accepted.
    std::uint64_t dropped = 0;    //!< Records discarded by OverflowPolicy::drop.
    std::uint64_t rotations = 0;  //!< Files rotated.
    std::uint64_t errors = 0;     //!< Sink, I/O and compression failures.
  };
  TINYLOG_API Stats stats() noexcept;

  //! Logs a message that is already formatted.
  TINYLOG_API void write(Level level, std::string_view message, const SourceLocation &where = {}) noexcept;

  /**
   * @brief Formats and logs a message. The format string is checked at compile time.
   *
   * Never throws: formatting failures are logged in place of the message.
   * Prefer the TLOG_* macros, which skip argument evaluation for disabled levels.
   */
  template <typename... Args>
  void log(Level level, const SourceLocation &where, std::format_string<Args...> format, Args &&...args) noexcept
  {
    if (should_log(level))
      detail::vlog(level, where, format.get(), std::make_format_args(args...));
  }

  //! Like log(), with a format string only known at run time (checked when used).
  template <typename... Args>
  void log_runtime(Level level, const SourceLocation &where, std::string_view format, Args &&...args) noexcept
  {
    if (should_log(level))
      detail::vlog(level, where, format, std::make_format_args(args...));
  }

  /**
   * @brief A handle to a channel: a named log with sinks of its own.
   *
   * Cheap to copy; every copy logs to the same channel. A default-constructed
   * handle, or one whose channel was closed, logs nowhere. Records go through
   * the same queue, worker and flush policy as the global log, so a channel
   * costs no thread of its own. Use the TLOG_*_TO macros, which skip argument
   * evaluation for disabled levels.
   */
  class Channel
  {
  public:
    Channel() = default;
    explicit Channel(std::shared_ptr<detail::ChannelState> state) : state_(std::move(state)) {}

    //! True while the channel is open.
    explicit operator bool() const noexcept { return state_ && state_->level_mask.load() != 0; }
    std::string_view name() const noexcept { return state_ ? std::string_view(state_->name) : std::string_view(); }

    //! True if a message of this level would be logged. One relaxed atomic load.
    bool should_log(Level level) const noexcept
    {
      return state_ && (state_->level_mask.load(std::memory_order_relaxed) & level_bit(level)) != 0;
    }
    //! This channel's threshold; the global level is not involved.
    void set_level(Level level) noexcept
    {
      if (state_ && state_->level_mask.load() != 0)
        state_->level_mask.store(mask_from(level) | open_marker);
    }

    //! Formats and logs a message. The format string is checked at compile time.
    template <typename... Args>
    void log(Level level, const SourceLocation &where, std::format_string<Args...> format,
             Args &&...args) const noexcept
    {
      if (should_log(level))
        detail::channel_vlog(*state_, level, where, format.get(), std::make_format_args(args...));
    }
    //! Logs a message that is already formatted.
    void write(Level level, std::string_view message, const SourceLocation &where = {}) const noexcept
    {
      if (should_log(level))
        detail::channel_write(*state_, level, message, where);
    }

    //! Set in an open channel's mask, so a threshold of `off` still reads as open.
    static constexpr std::uint32_t open_marker = 1u << 31;

  private:
    std::shared_ptr<detail::ChannelState> state_;
  };

  /**
   * @brief Opens a channel named `name` with the sinks of `config`.
   *
   * @throws std::invalid_argument if a channel of that name is open,
   *         std::system_error if one of its files cannot be opened.
   */
  TINYLOG_API Channel open_channel(std::string_view name, const ChannelConfig &config);
  //! The open channel named `name`, or an empty handle.
  TINYLOG_API Channel find_channel(std::string_view name) noexcept;
  /**
   * @brief Delivers what was logged to the channel so far, then closes its sinks.
   *
   * Handles to it log nowhere afterwards. Returns false if no such channel was open.
   */
  TINYLOG_API bool close_channel(std::string_view name) noexcept;

  /**
   * @brief Initializes on construction, shuts down on destruction.
   *
   * Put one at the top of main() so pending messages are always written.
   */
  class Guard
  {
  public:
    explicit Guard(const Config &config = {}) { init(config); }
    ~Guard() { shutdown(); }
    Guard(const Guard &) = delete;
    Guard &operator=(const Guard &) = delete;
  };
}

// ---------------------------------------------------------------------------
// Macros. Arguments are not evaluated when the level is disabled.
// TINYLOG_ACTIVE_LEVEL (0 = trace .. 7 = off) removes lower levels at compile time.
// ---------------------------------------------------------------------------

#ifndef TINYLOG_ACTIVE_LEVEL
#define TINYLOG_ACTIVE_LEVEL 0
#endif

#define TINYLOG_HERE (::tinylog::SourceLocation{__FILE__, __func__, static_cast<std::uint32_t>(__LINE__)})

#define TINYLOG_LOG(level, ...)                                  \
  do                                                             \
  {                                                              \
    if (::tinylog::should_log(level))                            \
      ::tinylog::log(level, TINYLOG_HERE, __VA_ARGS__);          \
  } while (0)

#define TINYLOG_LOG_TO(channel, level, ...)                      \
  do                                                             \
  {                                                              \
    if ((channel).should_log(level))                             \
      (channel).log(level, TINYLOG_HERE, __VA_ARGS__);           \
  } while (0)

#define TINYLOG_DISABLED_(...) \
  do                           \
  {                            \
  } while (0)

#if TINYLOG_ACTIVE_LEVEL <= 0
#define TLOG_TRACE(...) TINYLOG_LOG(::tinylog::Level::trace, __VA_ARGS__)
#else
#define TLOG_TRACE(...) TINYLOG_DISABLED_(__VA_ARGS__)
#endif
#if TINYLOG_ACTIVE_LEVEL <= 0
#define TLOG_TRACE_TO(channel, ...) TINYLOG_LOG_TO(channel, ::tinylog::Level::trace, __VA_ARGS__)
#else
#define TLOG_TRACE_TO(channel, ...) TINYLOG_DISABLED_(channel, __VA_ARGS__)
#endif

#if TINYLOG_ACTIVE_LEVEL <= 1
#define TLOG_DEBUG(...) TINYLOG_LOG(::tinylog::Level::debug, __VA_ARGS__)
#else
#define TLOG_DEBUG(...) TINYLOG_DISABLED_(__VA_ARGS__)
#endif
#if TINYLOG_ACTIVE_LEVEL <= 1
#define TLOG_DEBUG_TO(channel, ...) TINYLOG_LOG_TO(channel, ::tinylog::Level::debug, __VA_ARGS__)
#else
#define TLOG_DEBUG_TO(channel, ...) TINYLOG_DISABLED_(channel, __VA_ARGS__)
#endif

#if TINYLOG_ACTIVE_LEVEL <= 2
#define TLOG_INFO(...) TINYLOG_LOG(::tinylog::Level::info, __VA_ARGS__)
#else
#define TLOG_INFO(...) TINYLOG_DISABLED_(__VA_ARGS__)
#endif
#if TINYLOG_ACTIVE_LEVEL <= 2
#define TLOG_INFO_TO(channel, ...) TINYLOG_LOG_TO(channel, ::tinylog::Level::info, __VA_ARGS__)
#else
#define TLOG_INFO_TO(channel, ...) TINYLOG_DISABLED_(channel, __VA_ARGS__)
#endif

#if TINYLOG_ACTIVE_LEVEL <= 3
#define TLOG_WARN(...) TINYLOG_LOG(::tinylog::Level::warning, __VA_ARGS__)
#else
#define TLOG_WARN(...) TINYLOG_DISABLED_(__VA_ARGS__)
#endif
#if TINYLOG_ACTIVE_LEVEL <= 3
#define TLOG_WARN_TO(channel, ...) TINYLOG_LOG_TO(channel, ::tinylog::Level::warning, __VA_ARGS__)
#else
#define TLOG_WARN_TO(channel, ...) TINYLOG_DISABLED_(channel, __VA_ARGS__)
#endif

#if TINYLOG_ACTIVE_LEVEL <= 4
#define TLOG_ERROR(...) TINYLOG_LOG(::tinylog::Level::error, __VA_ARGS__)
#else
#define TLOG_ERROR(...) TINYLOG_DISABLED_(__VA_ARGS__)
#endif
#if TINYLOG_ACTIVE_LEVEL <= 4
#define TLOG_ERROR_TO(channel, ...) TINYLOG_LOG_TO(channel, ::tinylog::Level::error, __VA_ARGS__)
#else
#define TLOG_ERROR_TO(channel, ...) TINYLOG_DISABLED_(channel, __VA_ARGS__)
#endif

#if TINYLOG_ACTIVE_LEVEL <= 5
#define TLOG_CRITICAL(...) TINYLOG_LOG(::tinylog::Level::critical, __VA_ARGS__)
#else
#define TLOG_CRITICAL(...) TINYLOG_DISABLED_(__VA_ARGS__)
#endif
#if TINYLOG_ACTIVE_LEVEL <= 5
#define TLOG_CRITICAL_TO(channel, ...) TINYLOG_LOG_TO(channel, ::tinylog::Level::critical, __VA_ARGS__)
#else
#define TLOG_CRITICAL_TO(channel, ...) TINYLOG_DISABLED_(channel, __VA_ARGS__)
#endif

#if TINYLOG_ACTIVE_LEVEL <= 6
#define TLOG_FATAL(...) TINYLOG_LOG(::tinylog::Level::fatal, __VA_ARGS__)
#else
#define TLOG_FATAL(...) TINYLOG_DISABLED_(__VA_ARGS__)
#endif
#if TINYLOG_ACTIVE_LEVEL <= 6
#define TLOG_FATAL_TO(channel, ...) TINYLOG_LOG_TO(channel, ::tinylog::Level::fatal, __VA_ARGS__)
#else
#define TLOG_FATAL_TO(channel, ...) TINYLOG_DISABLED_(channel, __VA_ARGS__)
#endif
