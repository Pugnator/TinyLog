#include <tinylog/tinylog.hpp>

#include "sinks.hpp"

#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace tinylog::detail
{
  std::atomic<std::uint32_t> g_level_mask{mask_from(Level::info)};
  std::atomic<bool> g_background_allowed{true};
  std::atomic<bool> g_finalizing{false};
  std::atomic<bool> g_process_exiting{false};

  Counters &counters() noexcept
  {
    static Counters instance;
    return instance;
  }

  void report_error(std::string_view what) noexcept
  {
    const auto count = counters().errors.fetch_add(1, std::memory_order_relaxed) + 1;
    // The first few in full, then a reminder now and then: a full disk must not flood stderr.
    if (count <= 10 || count % 10000 == 0)
    {
      std::fprintf(stderr, "tinylog: %.*s (error #%llu)\n", static_cast<int>(what.size()), what.data(),
                   static_cast<unsigned long long>(count));
      std::fflush(stderr);
    }
  }

  namespace
  {
    //! Fixed part of a queued record; the message bytes follow it.
    struct QueuedHeader
    {
      std::int64_t time;
      std::uint64_t thread_id;
      const char *file;
      const char *function;
      std::uint32_t line;
      std::uint32_t size;
      std::uint32_t channel; //!< 0: the global log.
      Level level;
    };

    constexpr std::size_t align_up(std::size_t n) noexcept
    {
      return (n + alignof(QueuedHeader) - 1) & ~(alignof(QueuedHeader) - 1);
    }

    //! Windows runs DLL static destructors under the loader lock, where a join deadlocks.
#if defined(_WIN32)
    constexpr bool finalize_detaches = true;
#else
    constexpr bool finalize_detaches = false;
#endif

    //! Set while this thread is inside a sink, to refuse re-entrant logging.
    thread_local bool t_in_sink = false;
    //! Set while this thread runs a flush the user asked for.
    thread_local bool t_forced_flush = false;

    struct SinkScope
    {
      SinkScope() { t_in_sink = true; }
      ~SinkScope() { t_in_sink = false; }
    };

    /**
     * @brief The process-wide logger state behind the free functions.
     *
     * Never destroyed (so logging from static destructors stays safe); a
     * separate finalizer flushes it at exit.
     */
    class Core
    {
    public:
      static Core &instance()
      {
        static Core *core = new Core();
        static Finalizer finalizer;
        return *core;
      }

      void init(Config config)
      {
        std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
        init_locked(std::move(config));
      }

      void ensure_initialized()
      {
        if (initialized_.load(std::memory_order_acquire))
          return;
        std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
        if (initialized_.load(std::memory_order_relaxed))
          return;
        const auto mask = g_level_mask.load();
        try
        {
          Config config;
          config.level = level_from_mask(mask);
          init_locked(std::move(config), mask);
        }
        catch (const std::exception &e)
        {
          report_error(std::string("default configuration failed: ") + e.what());
          Config fallback;
          fallback.use_env = false;
          init_locked(std::move(fallback), mask);
        }
      }

      void submit(Level level, const SourceLocation &where, std::string_view message,
                  std::uint32_t channel = 0) noexcept
      {
        if (t_in_sink)
        {
          report_error("a sink logged from inside write(); message dropped");
          return;
        }
        ensure_initialized_noexcept();
        const auto now = std::chrono::system_clock::now();
        const auto tid = platform::thread_id();

        if (async_.load(std::memory_order_acquire))
        {
          std::unique_lock<std::mutex> queue(queue_mutex_);
          if (enqueue_locked(queue, level, where, message, now, tid, channel))
            return;
          // The worker stopped meanwhile: write synchronously below.
        }

        std::lock_guard<std::mutex> sinks(sink_mutex_);
        counters().logged.fetch_add(1, std::memory_order_relaxed);
        Record record{level, now, tid, where, message};
        dispatch(record, channel);
        if (level >= flush_level_ || (flush_interval_.count() > 0 && now - last_flush_ >= flush_interval_))
          flush_sinks(now);
      }

      void flush() noexcept
      {
        {
          std::unique_lock<std::mutex> queue(queue_mutex_);
          if (async_active_)
          {
            const auto target = enqueued_;
            flush_requested_ = true;
            work_cv_.notify_one();
            done_cv_.wait(queue, [&]
                          { return flushed_ >= target || !async_active_; });
            return;
          }
        }
        std::lock_guard<std::mutex> sinks(sink_mutex_);
        flush_sinks(std::chrono::system_clock::now(), true);
      }

      void wait_idle() noexcept
      {
        flush();
        std::vector<std::shared_ptr<Sink>> snapshot;
        {
          std::lock_guard<std::mutex> sinks(sink_mutex_);
          snapshot = sinks_;
          for (const auto &[id, channel] : channels_)
            snapshot.insert(snapshot.end(), channel.sinks.begin(), channel.sinks.end());
        }
        for (const auto &sink : snapshot)
        {
          try
          {
            sink->wait_idle();
          }
          catch (...)
          {
          }
        }
      }

      void shutdown() noexcept
      {
        std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
        stop_worker(true);
        g_background_allowed.store(false);
        std::lock_guard<std::mutex> sinks(sink_mutex_);
        flush_sinks(std::chrono::system_clock::now(), true);
        for (const auto &sink : sinks_)
          guarded([&]
                  { sink->stop_background(); });
        for (const auto &[id, channel] : channels_)
          for (const auto &sink : channel.owned)
            guarded([&]
                    { sink->stop_background(); });
        // Late messages (static destructors...) are written and flushed at once.
        flush_level_ = Level::trace;
      }

      void add_sink(std::shared_ptr<Sink> sink)
      {
        if (!sink)
          return;
        ensure_initialized();
        std::lock_guard<std::mutex> sinks(sink_mutex_);
        sinks_.push_back(std::move(sink));
      }

      Channel open_channel(std::string_view name, const ChannelConfig &config)
      {
        ensure_initialized();
        std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
        {
          std::lock_guard<std::mutex> sinks(sink_mutex_);
          if (channel_ids_.contains(std::string(name)))
            throw std::invalid_argument("tinylog: channel '" + std::string(name) + "' is already open");
        }
        // Files are opened outside the sink lock: logging goes on meanwhile.
        ChannelEntry entry;
        if (config.console)
          entry.owned.push_back(std::make_shared<ConsoleSink>(*config.console));
        for (const auto &file : config.files)
          entry.owned.push_back(std::make_shared<FileSink>(file));
        entry.sinks = entry.owned;
        for (const auto &sink : config.sinks)
        {
          if (sink)
            entry.sinks.push_back(sink);
        }
        entry.forward = config.forward;
        entry.state = std::make_shared<ChannelState>();
        entry.state->name = std::string(name);
        entry.state->id = next_channel_id_++;
        entry.state->level_mask.store(mask_from(config.level) | Channel::open_marker);
        Channel handle(entry.state);
        std::lock_guard<std::mutex> sinks(sink_mutex_);
        channel_ids_.emplace(entry.state->name, entry.state->id);
        channels_.emplace(entry.state->id, std::move(entry));
        return handle;
      }

      Channel find_channel(std::string_view name) noexcept
      {
        std::lock_guard<std::mutex> sinks(sink_mutex_);
        const auto id = channel_ids_.find(std::string(name));
        return id == channel_ids_.end() ? Channel() : Channel(channels_.at(id->second).state);
      }

      bool close_channel(std::string_view name) noexcept
      {
        std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
        std::shared_ptr<ChannelState> state;
        {
          std::lock_guard<std::mutex> sinks(sink_mutex_);
          const auto id = channel_ids_.find(std::string(name));
          if (id == channel_ids_.end())
            return false;
          state = channels_.at(id->second).state;
        }
        // No new records, then deliver the queued ones before the sinks go.
        state->level_mask.store(0);
        flush();
        ChannelEntry entry;
        {
          std::lock_guard<std::mutex> sinks(sink_mutex_);
          auto found = channels_.find(state->id);
          entry = std::move(found->second);
          channels_.erase(found);
          channel_ids_.erase(state->name);
        }
        // Rotated files are compressed and pruned in the background; let that
        // finish so a closed channel leaves no more files than it may.
        for (const auto &sink : entry.owned)
        {
          guarded([&]
                  { sink->wait_idle(); });
          guarded([&]
                  { sink->stop_background(); });
        }
        return true;
      }

    private:
      struct ChannelEntry
      {
        std::shared_ptr<ChannelState> state;
        std::vector<std::shared_ptr<Sink>> sinks; //!< Owned and user sinks.
        std::vector<std::shared_ptr<Sink>> owned;
        bool forward = false;
      };

      struct Finalizer
      {
        ~Finalizer()
        {
          if (g_process_exiting.load())
            return; // other threads are gone, maybe holding our locks
          g_finalizing.store(true);
          Core::instance().shutdown();
        }
      };

      Core() = default;

      static Level level_from_mask(std::uint32_t mask) noexcept
      {
        for (unsigned i = 0; i < static_cast<unsigned>(Level::off); ++i)
        {
          if (mask & (1u << i))
            return static_cast<Level>(i);
        }
        return Level::off;
      }

      template <typename F>
      static void guarded(F &&f) noexcept
      {
        try
        {
          f();
        }
        catch (const std::exception &e)
        {
          report_error(e.what());
        }
        catch (...)
        {
          report_error("unknown exception in a sink");
        }
      }

      void ensure_initialized_noexcept() noexcept
      {
        try
        {
          ensure_initialized();
        }
        catch (...)
        {
          report_error("logger initialization failed");
        }
      }

      //! `keep_mask`: keep this level mask instead of Config::level (lazy default init).
      void init_locked(Config config, std::optional<std::uint32_t> keep_mask = std::nullopt)
      {
        if (config.use_env)
        {
          if (const auto text = platform::getenv("TINYLOG_CONFIG"); text && !text->empty())
            apply_config_string(config, *text);
          // A lazy default init keeps the mask: TINYLOG_LEVEL was applied at start-up
          // and set_level() calls made since then must win.
          if (const auto text = platform::getenv("TINYLOG_LEVEL"); text && !text->empty() && !keep_mask)
          {
            if (const auto level = parse_level(*text))
            {
              config.level = *level;
            }
          }
        }

        stop_worker(true);
        g_background_allowed.store(true);

        {
          std::lock_guard<std::mutex> sinks(sink_mutex_);
          flush_sinks(std::chrono::system_clock::now(), true);
          // Close the old files before opening new ones: a compressed log must
          // end its frame before another writer appends to the same file.
          for (auto &sink : owned_sinks_)
            guarded([&]
                    { sink->stop_background(); });
          owned_sinks_.clear();
          sinks_.clear();

          std::vector<std::shared_ptr<Sink>> created;
          try
          {
            if (config.console)
              created.push_back(std::make_shared<ConsoleSink>(*config.console));
            for (const auto &file : config.files)
              created.push_back(std::make_shared<FileSink>(file));
          }
          catch (...)
          {
            // Keep a working logger rather than none, then report the failure.
            owned_sinks_.push_back(std::make_shared<ConsoleSink>(ConsoleSinkConfig{}));
            sinks_ = owned_sinks_;
            initialized_.store(true, std::memory_order_release);
            throw;
          }
          owned_sinks_ = std::move(created);
          sinks_ = owned_sinks_;
          for (auto &sink : config.sinks)
          {
            if (sink)
              sinks_.push_back(sink);
          }
          flush_level_ = config.flush_level;
          flush_interval_ = config.flush_interval;
          last_flush_ = std::chrono::system_clock::now();
        }

        g_level_mask.store(keep_mask ? *keep_mask : mask_from(config.level));
        counters().logged = 0;
        counters().dropped = 0;
        counters().rotations = 0;
        counters().errors = 0;

        if (config.mode == Mode::async)
          start_worker(config);
        initialized_.store(true, std::memory_order_release);
      }

      // -----------------------------------------------------------------------
      // Dispatch (caller holds sink_mutex_)
      // -----------------------------------------------------------------------

      void dispatch(Record record, std::uint32_t channel) noexcept
      {
        SinkScope scope;
        const auto deliver = [&](const std::vector<std::shared_ptr<Sink>> &targets)
        {
          for (const auto &sink : targets)
          {
            if (record.level >= sink->min_level)
              guarded([&]
                      { sink->write(record); });
          }
        };
        if (channel == 0)
        {
          deliver(sinks_);
          return;
        }
        const auto found = channels_.find(channel);
        if (found == channels_.end())
          return; // closed meanwhile
        record.channel = found->second.state->name;
        deliver(found->second.sinks);
        if (found->second.forward)
          deliver(sinks_);
      }

      void flush_sinks(std::chrono::system_clock::time_point now, bool forced = false) noexcept
      {
        SinkScope scope;
        t_forced_flush = forced;
        for (const auto &sink : sinks_)
          guarded([&]
                  { sink->flush(); });
        for (const auto &[id, channel] : channels_)
          for (const auto &sink : channel.sinks)
            guarded([&]
                    { sink->flush(); });
        t_forced_flush = false;
        last_flush_ = now;
      }

      // -----------------------------------------------------------------------
      // Async queue: producers append to one buffer, the worker swaps it out.
      // -----------------------------------------------------------------------

      bool enqueue_locked(std::unique_lock<std::mutex> &queue, Level level, const SourceLocation &where,
                          std::string_view message, std::chrono::system_clock::time_point now,
                          std::uint64_t tid, std::uint32_t channel)
      {
        if (!async_active_)
          return false;
        const std::size_t size = std::min<std::size_t>(message.size(), UINT32_MAX);
        const std::size_t need = sizeof(QueuedHeader) + align_up(size);
        // An oversized message is still accepted into an empty buffer.
        if (!active_.empty() && active_.size() + need > capacity_)
        {
          if (overflow_ == OverflowPolicy::drop)
          {
            counters().dropped.fetch_add(1, std::memory_order_relaxed);
            return true;
          }
          space_cv_.wait(queue, [&]
                         { return active_.empty() || active_.size() + need <= capacity_ || !async_active_; });
          if (!async_active_)
            return false;
        }
        const std::size_t offset = active_.size();
        active_.resize(offset + need);
        QueuedHeader header{now.time_since_epoch().count(), tid, where.file, where.function, where.line,
                            static_cast<std::uint32_t>(size), channel, level};
        std::memcpy(active_.data() + offset, &header, sizeof(header));
        std::memcpy(active_.data() + offset + sizeof(header), message.data(), size);
        ++enqueued_;
        counters().logged.fetch_add(1, std::memory_order_relaxed);
        // A polling worker finds the record by itself; waking it per message
        // would cost a system call and a context switch each time.
        const bool wake = worker_sleeping_ || active_.size() >= capacity_ / 2;
        queue.unlock();
        if (wake)
          work_cv_.notify_one();
        return true;
      }

      void start_worker(const Config &config)
      {
        std::lock_guard<std::mutex> queue(queue_mutex_);
        capacity_ = std::max<std::size_t>(config.queue_capacity, 4096);
        overflow_ = config.overflow;
        active_.clear();
        active_.reserve(capacity_ + 1024);
        enqueued_ = flushed_ = 0;
        flush_requested_ = false;
        async_active_ = true;
        worker_sleeping_ = false;
        worker_exited_ = false;
        worker_ = std::thread(&Core::worker_main, this);
        async_.store(true, std::memory_order_release);
      }

      //! Delivers everything queued, then stops the worker. `join` is false while finalizing.
      void stop_worker(bool join) noexcept
      {
        {
          std::lock_guard<std::mutex> queue(queue_mutex_);
          if (!async_active_)
            return;
          async_active_ = false;
        }
        async_.store(false, std::memory_order_release);
        work_cv_.notify_all();
        space_cv_.notify_all();
        done_cv_.notify_all();
        if (!worker_.joinable())
          return;
        if (join && (!g_finalizing.load() || !finalize_detaches))
        {
          worker_.join();
          return;
        }
        // Static destructors of a DLL run under the loader lock, where joining
        // deadlocks: wait for the worker to finish its loop, then let it go.
        for (int i = 0; i < 400 && !worker_exited_.load(); ++i)
          std::this_thread::sleep_for(std::chrono::milliseconds(5));
        worker_.detach();
      }

      void worker_main() noexcept
      {
        std::vector<std::byte> batch;
        batch.reserve(capacity_ + 1024);
        bool dirty = false;
        int idle_polls = 0;
        auto last_flush = std::chrono::steady_clock::now();
        std::unique_lock<std::mutex> queue(queue_mutex_);
        for (;;)
        {
          if (active_.empty() && !flush_requested_ && async_active_)
          {
            // Poll while records keep coming (producers then need not notify);
            // after a quiet spell, sleep until a producer wakes us.
            if (idle_polls < max_idle_polls)
            {
              ++idle_polls;
              work_cv_.wait_for(queue, poll_interval);
            }
            else
            {
              worker_sleeping_ = true;
              if (dirty && flush_interval_.count() > 0)
                work_cv_.wait_until(queue, last_flush + flush_interval_);
              else
                work_cv_.wait(queue);
              worker_sleeping_ = false;
            }
          }
          if (!active_.empty())
            idle_polls = 0;
          batch.swap(active_);
          const std::uint64_t target = enqueued_;
          const bool requested = std::exchange(flush_requested_, false);
          const bool stopping = !async_active_;
          queue.unlock();
          space_cv_.notify_all();

          Level highest = Level::trace;
          if (!batch.empty())
          {
            std::lock_guard<std::mutex> sinks(sink_mutex_);
            std::size_t offset = 0;
            while (offset < batch.size())
            {
              QueuedHeader header;
              std::memcpy(&header, batch.data() + offset, sizeof(header));
              const char *text = reinterpret_cast<const char *>(batch.data() + offset + sizeof(header));
              Record record{header.level,
                            std::chrono::system_clock::time_point(std::chrono::system_clock::duration(header.time)),
                            header.thread_id,
                            SourceLocation{header.file, header.function, header.line},
                            std::string_view(text, header.size)};
              dispatch(record, header.channel);
              highest = std::max(highest, header.level);
              offset += sizeof(header) + align_up(header.size);
            }
            batch.clear();
            dirty = true;
          }

          const auto now = std::chrono::steady_clock::now();
          const bool due = flush_interval_.count() > 0 && now - last_flush >= flush_interval_;
          const bool forced = requested || stopping;
          const bool flush_now = forced || highest >= flush_level_ || due;
          if (flush_now && (dirty || forced))
          {
            std::lock_guard<std::mutex> sinks(sink_mutex_);
            flush_sinks(std::chrono::system_clock::now(), forced);
            dirty = false;
            last_flush = now;
          }

          queue.lock();
          if (flush_now)
            flushed_ = target;
          done_cv_.notify_all();
          if (stopping && active_.empty())
            break;
        }
        worker_exited_.store(true);
      }

      std::mutex lifecycle_mutex_; //!< Serializes init/shutdown.
      std::atomic<bool> initialized_{false};

      std::mutex sink_mutex_; //!< Guards the sinks and every call into them.
      std::vector<std::shared_ptr<Sink>> sinks_;
      std::vector<std::shared_ptr<Sink>> owned_sinks_;
      Level flush_level_ = Level::trace;
      std::chrono::milliseconds flush_interval_{1000};
      std::chrono::system_clock::time_point last_flush_{};
      //! Open channels by id, and their ids by name (both under sink_mutex_).
      std::map<std::uint32_t, ChannelEntry> channels_;
      std::map<std::string, std::uint32_t> channel_ids_;
      std::uint32_t next_channel_id_ = 1; //!< Under lifecycle_mutex_; 0 is the global log.

      std::atomic<bool> async_{false};
      std::mutex queue_mutex_;
      std::condition_variable work_cv_;  //!< Worker: records or a flush request arrived.
      std::condition_variable space_cv_; //!< Producers: the buffer was emptied.
      std::condition_variable done_cv_;  //!< flush(): a batch was delivered.
      std::vector<std::byte> active_;
      std::size_t capacity_ = 4u << 20;
      OverflowPolicy overflow_ = OverflowPolicy::block;
      bool async_active_ = false;
      bool flush_requested_ = false;
      //! The worker is blocked and must be notified (it is polling otherwise).
      bool worker_sleeping_ = false;
      static constexpr std::chrono::milliseconds poll_interval{1};
      static constexpr int max_idle_polls = 100;
      std::uint64_t enqueued_ = 0;
      std::uint64_t flushed_ = 0;
      std::thread worker_;
      std::atomic<bool> worker_exited_{false};
    };

    //! Applies TINYLOG_LEVEL before main(), so should_log() honours it even without init().
    [[maybe_unused]] const bool env_level_applied = []
    {
      if (const auto text = platform::getenv("TINYLOG_LEVEL"))
      {
        if (const auto level = parse_level(*text))
          g_level_mask.store(mask_from(*level));
      }
      return true;
    }();
  }

  bool forced_flush() noexcept { return t_forced_flush; }

  void init_checked(const Config &config, int header_version)
  {
    // Before 1.0 a minor release may break the ABI; afterwards only a major one.
    const bool compatible = TINYLOG_VERSION_MAJOR == 0
                                ? header_version / 100 == TINYLOG_VERSION_NUMBER / 100
                                : header_version / 10000 == TINYLOG_VERSION_NUMBER / 10000;
    if (!compatible)
    {
      throw std::runtime_error("tinylog headers (" + std::to_string(header_version) +
                               ") do not match the library (" + std::to_string(TINYLOG_VERSION_NUMBER) + ")");
    }
    Core::instance().init(config);
  }

  namespace
  {
    void vlog_to(std::uint32_t channel, Level level, const SourceLocation &where, std::string_view format,
                 std::format_args args) noexcept
    {
      // One buffer per nesting depth: a formatter may itself log.
      //
      // The buffers die with the thread's other thread_local objects - and on
      // the main thread that happens inside exit(), BEFORE the atexit handlers
      // and static destructors run, which may well log. Formatting into them
      // then wrote into freed memory. `gone` is trivially destructible, so it
      // stays readable after that point; once it is set, a line is formatted
      // into a local string instead.
      thread_local bool gone = false;
      struct Buffers
      {
        std::string text[4];
        unsigned depth = 0;
        ~Buffers() { gone = true; }
      };
      std::string late;
      std::string *slot = &late;
      unsigned *depth = nullptr;
      if (!gone)
      {
        thread_local Buffers buffers;
        if (buffers.depth >= std::size(buffers.text))
          return;
        depth = &buffers.depth;
        slot = &buffers.text[buffers.depth++];
      }
      std::string &buffer = *slot;
      buffer.clear();
      try
      {
        std::vformat_to(std::back_inserter(buffer), format, args);
      }
      catch (const std::exception &e)
      {
        buffer = "[format error: ";
        buffer += e.what();
        buffer += "] ";
        buffer += format;
      }
      catch (...)
      {
        buffer = "[format error] ";
        buffer += format;
      }
      Core::instance().submit(level, where, buffer, channel);
      if (depth != nullptr)
        --*depth;
      // Do not let one huge message pin memory for the life of the thread.
      if (buffer.capacity() > (1u << 20))
        std::string().swap(buffer);
    }
  }

  void vlog(Level level, const SourceLocation &where, std::string_view format, std::format_args args) noexcept
  {
    vlog_to(0, level, where, format, args);
  }

  void channel_vlog(const ChannelState &channel, Level level, const SourceLocation &where, std::string_view format,
                    std::format_args args) noexcept
  {
    vlog_to(channel.id, level, where, format, args);
  }

  void channel_write(const ChannelState &channel, Level level, std::string_view message,
                     const SourceLocation &where) noexcept
  {
    Core::instance().submit(level, where, message, channel.id);
  }

}

namespace tinylog
{
  std::string_view version() noexcept { return TINYLOG_VERSION_STRING; }
  int version_number() noexcept { return TINYLOG_VERSION_NUMBER; }

  void shutdown() noexcept { detail::Core::instance().shutdown(); }
  void flush() noexcept { detail::Core::instance().flush(); }
  void wait_idle() noexcept { detail::Core::instance().wait_idle(); }
  void add_sink(std::shared_ptr<Sink> sink) { detail::Core::instance().add_sink(std::move(sink)); }

  Channel open_channel(std::string_view name, const ChannelConfig &config)
  {
    return detail::Core::instance().open_channel(name, config);
  }
  Channel find_channel(std::string_view name) noexcept { return detail::Core::instance().find_channel(name); }
  bool close_channel(std::string_view name) noexcept { return detail::Core::instance().close_channel(name); }

  void set_level(Level level) noexcept { detail::g_level_mask.store(mask_from(level)); }
  void enable(Level level) noexcept { detail::g_level_mask.fetch_or(level_bit(level)); }
  void disable(Level level) noexcept { detail::g_level_mask.fetch_and(~level_bit(level)); }
  void set_level_mask(std::uint32_t mask) noexcept { detail::g_level_mask.store(mask & mask_from(Level::trace)); }
  std::uint32_t level_mask() noexcept { return detail::g_level_mask.load(); }

  Level level() noexcept
  {
    const auto mask = detail::g_level_mask.load();
    for (unsigned i = 0; i < static_cast<unsigned>(Level::off); ++i)
    {
      if (mask & (1u << i))
        return static_cast<Level>(i);
    }
    return Level::off;
  }

  Stats stats() noexcept
  {
    auto &c = detail::counters();
    return Stats{c.logged.load(), c.dropped.load(), c.rotations.load(), c.errors.load()};
  }

  void write(Level level, std::string_view message, const SourceLocation &where) noexcept
  {
    if (should_log(level))
      detail::Core::instance().submit(level, where, message);
  }
}
