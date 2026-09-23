#pragma once

/*! \file Start-up configuration: sinks, rotation, compression, async mode. */

#include <tinylog/sink.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace tinylog
{
  enum class Mode : std::uint8_t
  {
    //! The calling thread formats and writes. Simple, lowest memory, blocks on I/O.
    sync,
    //! The calling thread formats and enqueues; a worker thread writes in batches.
    async,
  };

  //! What an async producer does when the queue is full.
  enum class OverflowPolicy : std::uint8_t
  {
    block, //!< Wait for room: nothing is lost, the caller may stall.
    drop,  //!< Discard the message and count it (see Stats::dropped).
  };

  enum class Compression : std::uint8_t
  {
    none,
    zstd,
  };

  enum class ConsoleStream : std::uint8_t
  {
    out,
    err,
  };

  enum class ColorMode : std::uint8_t
  {
    automatic, //!< Colour only a real terminal, and honour NO_COLOR.
    always,
    never,
  };

  struct ConsoleSinkConfig
  {
    ConsoleStream stream = ConsoleStream::out;
    ColorMode color = ColorMode::automatic;
    Level min_level = Level::trace;
    Layout layout{};
  };

  /**
   * @brief When and how the active log file is rotated.
   *
   * A rotated file is renamed to `<stem>.<YYYYMMDD-HHMMSS>[.N]<ext>` (plus
   * `.zst` when compressed). Compression of rotated files and retention run on
   * a background thread, never on the logging path.
   */
  struct RotationPolicy
  {
    //! Rotate before the file would grow past this many bytes on disk. 0 = never.
    std::uint64_t max_size = 0;
    //! Rotate when the clock crosses a multiple of this interval (UTC-aligned). 0 = never.
    std::chrono::seconds interval{0};
    //! Rotate a non-empty existing file when the sink opens it.
    bool on_open = false;
    //! Rotated files to keep; the oldest are deleted. 0 = keep all.
    std::size_t max_files = 5;
    //! Compress rotated files (ignored when the active file is already compressed).
    Compression compress = Compression::none;
    //! zstd level for rotated files: 1 (fast) .. 19 (small); 20..22 need lots of RAM.
    int compress_level = 19;
  };

  struct FileSinkConfig
  {
    //! Active log file. With live compression ".zst" is appended unless present.
    std::filesystem::path path = "log.txt";
    Level min_level = Level::trace;
    Layout layout{};
    RotationPolicy rotation{};
    /**
     * @brief Compress the active file itself.
     *
     * The file is a sequence of independent zstd frames, so a restarted process
     * appends to it and `zstd -d` decodes the whole file. A frame left
     * unfinished by a crash is repaired the next time the file is opened.
     */
    Compression compression = Compression::none;
    //! zstd level for the active file. Low levels keep up with heavy logging.
    int compression_level = 3;
    //! Uncompressed bytes per zstd frame of the active file.
    std::size_t frame_size = 4u << 20;
    /**
     * @brief Live compression: least time between two flushes of compressed data.
     *
     * Every flush ends a zstd block, and tiny blocks compress badly (a block per
     * record can be larger than the text). Flushes arriving sooner are deferred
     * until this much time passed or 64 KiB accumulated. Rotation, flush(),
     * shutdown() and wait_idle() always write everything. 0 honours every flush.
     */
    std::chrono::milliseconds compressed_flush_interval{1000};
    //! Bytes buffered in memory before they are handed to the OS.
    std::size_t buffer_size = 64u << 10;
    //! Start with an empty file instead of appending.
    bool truncate = false;
  };

  /**
   * @brief Everything init() needs; every field has a usable default.
   */
  struct Config
  {
    //! Threshold: this level and everything more severe is enabled.
    Level level = Level::info;
    Mode mode = Mode::sync;
    //! Async only: bytes of pending messages before the overflow policy applies.
    std::size_t queue_capacity = 4u << 20;
    OverflowPolicy overflow = OverflowPolicy::block;
    //! Flush the sinks after a record of this level or above.
    Level flush_level = Level::trace;
    //! Flush at least this often while records keep arriving. 0 = only by level.
    std::chrono::milliseconds flush_interval{1000};
    //! Console output; std::nullopt disables it.
    std::optional<ConsoleSinkConfig> console = ConsoleSinkConfig{};
    std::vector<FileSinkConfig> files;
    //! User sinks, receiving records next to the built-in ones.
    std::vector<std::shared_ptr<Sink>> sinks;
    //! Apply TINYLOG_LEVEL / TINYLOG_CONFIG from the environment on top of this config.
    bool use_env = true;
  };

  /**
   * @brief Applies "key=value" settings, separated by ';' or newlines, onto `config`.
   *
   * Keys: level, mode, queue, overflow, flush, flush_interval, console,
   * console.color, format, timestamp, utc, thread, source, file, file.level,
   * file.compress, file.compress_level, file.frame, file.buffer, file.truncate,
   * rotate.size, rotate.interval, rotate.keep, rotate.on_open, rotate.compress,
   * rotate.compress_level. `file=` starts a new file sink; the file.* and
   * rotate.* keys apply to the last one. Sizes accept K/M/G (KiB...) suffixes,
   * intervals ms/s/m/h/d.
   *
   * @throws std::invalid_argument naming the offending key.
   */
  TINYLOG_API void apply_config_string(Config &config, std::string_view text);

  //! Builds a config from a settings string (see apply_config_string()).
  TINYLOG_API Config parse_config(std::string_view text);

  //! Creates a standalone console sink, e.g. to compose into Config::sinks.
  TINYLOG_API std::shared_ptr<Sink> make_console_sink(const ConsoleSinkConfig &config);

  //! Creates a standalone file sink. @throws std::system_error if the file cannot be opened.
  TINYLOG_API std::shared_ptr<Sink> make_file_sink(const FileSinkConfig &config);
}
