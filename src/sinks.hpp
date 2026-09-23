#pragma once

/*! \file Built-in sinks and shared internals. Internal. */

#include <tinylog/config.hpp>

#include "format.hpp"
#include "platform.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace tinylog::detail
{
  namespace zstd
  {
    class FrameWriter;
  }

  //! Library-wide counters behind tinylog::stats().
  struct Counters
  {
    std::atomic<std::uint64_t> logged{0};
    std::atomic<std::uint64_t> dropped{0};
    std::atomic<std::uint64_t> rotations{0};
    std::atomic<std::uint64_t> errors{0};
  };
  Counters &counters() noexcept;

  //! True inside a flush the user asked for (flush(), shutdown(), re-init): sinks
  //! must write everything, not only what their own flush policy wants.
  bool forced_flush() noexcept;

  //! Counts an internal failure and reports it on stderr (rate limited). Never throws.
  void report_error(std::string_view what) noexcept;

  //! False after shutdown(): background threads must not be started any more.
  extern std::atomic<bool> g_background_allowed;
  //! True while static destructors run: threads are detached rather than joined.
  extern std::atomic<bool> g_finalizing;
  //! True when a DLL is detached because the process is exiting (other threads are gone).
  extern std::atomic<bool> g_process_exiting;

  class ConsoleSink final : public Sink
  {
  public:
    explicit ConsoleSink(const ConsoleSinkConfig &config);
    ~ConsoleSink() override;
    void write(const Record &record) override;
    void flush() override;

  private:
    std::unique_ptr<platform::Console> console_;
    Formatter formatter_;
    std::string buffer_;
    bool color_ = false;
  };

  /**
   * @brief A single background thread running jobs in order.
   *
   * Compression and retention run here so the logging path never waits for
   * them. Started on the first job; stop() abandons pending work (it is picked
   * up again the next time the file sink opens) and can be followed by new jobs.
   */
  class Worker
  {
  public:
    using Job = std::function<void(const std::atomic<bool> &stop)>;
    ~Worker();
    //! Queues a job; false if background threads are not allowed (after shutdown()).
    bool post(Job job);
    void wait_idle();
    void stop() noexcept;

  private:
    void run();

    std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable idle_;
    std::deque<Job> jobs_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> exited_{false};
    bool busy_ = false;
  };

  class FileSink final : public Sink
  {
  public:
    explicit FileSink(const FileSinkConfig &config);
    ~FileSink() override;
    void write(const Record &record) override;
    void flush() override;
    void wait_idle() override;
    void stop_background() override;

  private:
    void open_active(bool startup);
    void recover_compressed();
    void rotate(std::chrono::system_clock::time_point now);
    //! Writes buffered text out; `flush` also flushes the compressor (subject to its interval).
    void drain(bool flush, bool force = false);
    void write_disk();
    std::filesystem::path rotated_path(std::chrono::system_clock::time_point now);
    void schedule_maintenance(const std::filesystem::path &rotated);
    void pick_up_leftovers();
    void prune() noexcept;
    void update_boundary(std::chrono::system_clock::time_point now);
    bool compress_live() const noexcept { return config_.compression == Compression::zstd; }

    FileSinkConfig config_;
    std::filesystem::path active_; //!< File being written (with .zst when compressed live).
    std::filesystem::path dir_;
    std::filesystem::path::string_type stem_;      //!< "app" for app.log
    std::filesystem::path::string_type extension_; //!< ".log" for app.log, before any ".zst"
    platform::File file_;
    Formatter formatter_;
    std::string text_;       //!< Formatted, not yet written or compressed.
    std::string compressed_; //!< Compressed, not yet written.
    std::unique_ptr<zstd::FrameWriter> compressor_;
    std::uint64_t disk_size_ = 0;
    //! Offset of the compressed frame being written: where a failed write is cut back to.
    std::uint64_t frame_start_ = 0;
    std::int64_t next_boundary_ = 0; //!< Seconds since epoch; 0 = no time rotation.
    //! After a failed rename, rotation is retried no earlier than this.
    std::chrono::system_clock::time_point retry_rotation_at_{};
    //! Last rotation's name stamp and sequence: later names must sort after it.
    std::string last_stamp_;
    unsigned last_sequence_ = 0;
    //! Live compression: when compressed data was last flushed, and input since.
    std::chrono::steady_clock::time_point last_block_flush_{};
    std::size_t unflushed_input_ = 0;
    Worker worker_;
  };
}
