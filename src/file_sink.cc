#include "sinks.hpp"

#include "zstd_util.hpp"

#include <algorithm>
#include <map>
#include <vector>

namespace fs = std::filesystem;

namespace tinylog::detail
{
  namespace
  {
    using native_string = fs::path::string_type;
    using native_char = native_string::value_type;

    std::int64_t epoch_seconds(std::chrono::system_clock::time_point time)
    {
      return std::chrono::duration_cast<std::chrono::seconds>(time.time_since_epoch()).count();
    }

    //! "20260923-101502" in local or UTC time.
    std::string stamp(std::chrono::system_clock::time_point time, bool utc)
    {
      const auto t = static_cast<std::time_t>(epoch_seconds(time));
      const std::tm tm = utc ? platform::utc_time(t) : platform::local_time(t);
      char buffer[32];
      const auto n = std::strftime(buffer, sizeof(buffer), "%Y%m%d-%H%M%S", &tm);
      return std::string(buffer, n);
    }

    native_string to_native(std::string_view ascii)
    {
      return native_string(ascii.begin(), ascii.end());
    }

    bool is_digit(native_char c) { return c >= native_char('0') && c <= native_char('9'); }

    bool ends_with(const native_string &text, const native_string &suffix)
    {
      return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
    }

    struct RotatedName
    {
      std::string key; //!< Sortable: timestamp plus zero-padded sequence number.
      bool compressed = false;
      bool temporary = false;
    };

    /**
     * @brief Recognizes `<stem>.<YYYYMMDD-HHMMSS>[.N]<ext>[.zst][.tmp]`.
     */
    bool parse_rotated(native_string name, const native_string &stem, const native_string &extension,
                       RotatedName &out)
    {
      out = {};
      static const native_string tmp = to_native(".tmp");
      static const native_string zst = to_native(".zst");
      if (ends_with(name, tmp))
      {
        out.temporary = true;
        name.resize(name.size() - tmp.size());
      }
      if (ends_with(name, zst))
      {
        out.compressed = true;
        name.resize(name.size() - zst.size());
      }
      if (!ends_with(name, extension))
        return false;
      name.resize(name.size() - extension.size());
      if (name.size() < stem.size() + 1 + 15 || name.compare(0, stem.size(), stem) != 0 ||
          name[stem.size()] != native_char('.'))
        return false;
      const native_string rest = name.substr(stem.size() + 1);
      // YYYYMMDD-HHMMSS
      for (std::size_t i = 0; i < 15; ++i)
      {
        if (i == 8 ? rest[i] != native_char('-') : !is_digit(rest[i]))
          return false;
      }
      std::string key;
      for (std::size_t i = 0; i < 15; ++i)
        key.push_back(static_cast<char>(rest[i])); // ASCII digits, checked above
      std::uint64_t sequence = 0;
      if (rest.size() > 15)
      {
        if (rest[15] != native_char('.') || rest.size() == 16 || rest.size() > 16 + 9)
          return false;
        for (std::size_t i = 16; i < rest.size(); ++i)
        {
          if (!is_digit(rest[i]))
            return false;
          sequence = sequence * 10 + static_cast<std::uint64_t>(rest[i] - native_char('0'));
        }
      }
      char suffix[16];
      std::snprintf(suffix, sizeof(suffix), ".%09llu", static_cast<unsigned long long>(sequence));
      out.key = key + suffix;
      return true;
    }
  }

  // ---------------------------------------------------------------------------
  // Worker
  // ---------------------------------------------------------------------------

  Worker::~Worker()
  {
    stop();
  }

  bool Worker::post(Job job)
  {
    if (!g_background_allowed.load(std::memory_order_acquire))
      return false;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!thread_.joinable())
    {
      stop_ = false;
      exited_ = false;
      thread_ = std::thread(&Worker::run, this);
    }
    jobs_.push_back(std::move(job));
    wake_.notify_one();
    return true;
  }

  void Worker::run()
  {
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;)
    {
      wake_.wait(lock, [&]
                 { return stop_.load() || !jobs_.empty(); });
      if (stop_)
        break;
      Job job = std::move(jobs_.front());
      jobs_.pop_front();
      busy_ = true;
      lock.unlock();
      try
      {
        job(stop_);
      }
      catch (const std::exception &e)
      {
        report_error(std::string("background job failed: ") + e.what());
      }
      catch (...)
      {
        report_error("background job failed");
      }
      lock.lock();
      busy_ = false;
      if (jobs_.empty())
        idle_.notify_all();
    }
    jobs_.clear();
    busy_ = false;
    idle_.notify_all();
    exited_ = true;
  }

  void Worker::wait_idle()
  {
    std::unique_lock<std::mutex> lock(mutex_);
    idle_.wait(lock, [&]
               { return !thread_.joinable() || stop_ || (jobs_.empty() && !busy_); });
  }

  void Worker::stop() noexcept
  {
    if (g_process_exiting.load())
    {
      // The thread was terminated by the OS, possibly holding mutex_.
      if (thread_.joinable())
        thread_.detach();
      return;
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!thread_.joinable())
        return;
      stop_ = true;
    }
    wake_.notify_all();
#if defined(_WIN32)
    const bool detach = g_finalizing.load();
#else
    const bool detach = false;
#endif
    if (detach)
    {
      // Static destructors of a DLL run under the loader lock, where joining a
      // thread deadlocks. Wait for the job loop to finish instead.
      for (int i = 0; i < 400 && !exited_.load(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      thread_.detach();
    }
    else
    {
      thread_.join();
    }
  }

  // ---------------------------------------------------------------------------
  // FileSink
  // ---------------------------------------------------------------------------

  FileSink::FileSink(const FileSinkConfig &config) : config_(config), formatter_(config.layout)
  {
    min_level = config.min_level;
    fs::path base = config.path;
    if (base.empty())
      base = "log.txt";
    std::error_code ec;
    base = fs::absolute(base, ec).lexically_normal();
    if (compress_live())
    {
      if (base.extension() == ".zst")
      {
        active_ = base;
        base.replace_extension();
      }
      else
      {
        active_ = base;
        active_ += ".zst";
      }
      compressor_ = std::make_unique<zstd::FrameWriter>(config.compression_level, config.frame_size);
    }
    else
    {
      active_ = base;
    }
    dir_ = base.parent_path();
    stem_ = base.stem().native();
    extension_ = base.extension().native();
    if (config_.buffer_size == 0)
      config_.buffer_size = 1;
    text_.reserve(config_.buffer_size + 1024);

    fs::create_directories(dir_, ec);
    open_active(true);
  }

  FileSink::~FileSink()
  {
    try
    {
      drain(false);
      if (compressor_)
      {
        compressor_->end_frame(compressed_);
        write_disk();
      }
    }
    catch (const std::exception &e)
    {
      report_error(std::string("closing log failed: ") + e.what());
    }
    catch (...)
    {
    }
  }

  void FileSink::open_active(bool startup)
  {
    const auto now = std::chrono::system_clock::now();
    if (startup && config_.truncate)
    {
      file_.open(active_, platform::File::Mode::append, true);
    }
    else
    {
      std::error_code ec;
      if (compressor_ && fs::exists(active_, ec))
        recover_compressed();
      file_.open(active_, platform::File::Mode::append, false);
    }
    disk_size_ = file_.size();
    frame_start_ = disk_size_;
    if (!startup)
      return;

    bool rotate_now = config_.rotation.on_open && disk_size_ > 0;
    const auto interval = config_.rotation.interval.count();
    if (interval > 0 && disk_size_ > 0)
    {
      // A file last written in an earlier period belongs to that period.
      std::error_code ec;
      const auto written = fs::last_write_time(active_, ec);
      if (!ec)
      {
        const auto as_system = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            written - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
        if (epoch_seconds(as_system) / interval < epoch_seconds(now) / interval)
          rotate_now = true;
      }
    }
    update_boundary(now);
    if (rotate_now)
      rotate(now);
    if (config_.rotation.compress == Compression::zstd && !compressor_)
      pick_up_leftovers();
  }

  void FileSink::recover_compressed()
  {
    platform::File reader;
    reader.open(active_, platform::File::Mode::read);
    const std::uint64_t size = reader.size();
    if (size == 0)
      return;
    const auto scan = zstd::scan_frames(reader, size);
    if (scan.status == zstd::ScanStatus::complete)
      return;

    if (scan.status == zstd::ScanStatus::truncated)
    {
      // A crash left the last frame unfinished. Keep what decodes, cut the
      // frame off, and compress the recovered text again as a new frame.
      std::string recovered = zstd::salvage(reader, scan.valid_end, size);
      reader.close();
      platform::File writer;
      writer.open(active_, platform::File::Mode::append, false);
      writer.truncate(scan.valid_end);
      writer.close();
      text_.insert(0, recovered);
      return;
    }

    // Not zstd data: never destroy it, move the file aside.
    reader.close();
    auto aside = active_;
    aside += "." + stamp(std::chrono::system_clock::now(), config_.layout.utc) + ".corrupt";
    if (const auto ec = platform::rename_file(active_, aside))
      throw std::system_error(ec, "cannot move damaged log '" + active_.string() + "' aside");
    report_error("'" + active_.string() + "' is not a zstd stream; moved to '" + aside.string() + "'");
  }

  void FileSink::update_boundary(std::chrono::system_clock::time_point now)
  {
    const auto interval = config_.rotation.interval.count();
    next_boundary_ = interval > 0 ? (epoch_seconds(now) / interval + 1) * interval : 0;
  }

  fs::path FileSink::rotated_path(std::chrono::system_clock::time_point now)
  {
    // Several rotations within one second get increasing sequence numbers;
    // never reuse a number freed by retention, or the newest file would sort first.
    const std::string current = stamp(now, config_.layout.utc);
    unsigned sequence = current == last_stamp_ ? last_sequence_ + 1 : 0;
    last_stamp_ = current;
    const native_string time = to_native(current);
    for (;; ++sequence)
    {
      native_string name = stem_;
      name += native_char('.');
      name += time;
      if (sequence > 0)
        name += to_native("." + std::to_string(sequence));
      name += extension_;
      fs::path plain = dir_ / name;
      fs::path compressed = plain;
      compressed += ".zst";
      std::error_code ec;
      if (!fs::exists(plain, ec) && !fs::exists(compressed, ec))
      {
        last_sequence_ = sequence;
        return compressor_ ? compressed : plain;
      }
    }
  }

  void FileSink::write_disk()
  {
    if (compressed_.empty())
      return;
    file_.write(compressed_.data(), compressed_.size());
    disk_size_ += compressed_.size();
    compressed_.clear();
  }

  void FileSink::drain(bool flush, bool force)
  {
    try
    {
      if (compressor_)
      {
        if (!text_.empty())
        {
          unflushed_input_ += text_.size();
          const auto frame = compressor_->write(text_, compressed_);
          if (frame != std::string::npos)
            frame_start_ = disk_size_ + frame;
          text_.clear();
        }
        if (flush && unflushed_input_ > 0)
        {
          const auto now = std::chrono::steady_clock::now();
          if (force || unflushed_input_ >= (64u << 10) ||
              now - last_block_flush_ >= config_.compressed_flush_interval)
          {
            compressor_->flush(compressed_);
            unflushed_input_ = 0;
            last_block_flush_ = now;
          }
        }
        write_disk();
      }
      else if (!text_.empty())
      {
        file_.write(text_.data(), text_.size());
        disk_size_ += text_.size();
        text_.clear();
      }
    }
    catch (...)
    {
      // Typically a full disk. Drop what is buffered so memory stays bounded;
      // for a compressed file also cut the unfinished frame so the file stays
      // decodable, and start over with a fresh frame.
      text_.clear();
      compressed_.clear();
      if (compressor_)
      {
        compressor_->reset();
        try
        {
          file_.truncate(frame_start_);
          disk_size_ = frame_start_;
        }
        catch (...)
        {
        }
      }
      throw;
    }
  }

  void FileSink::rotate(std::chrono::system_clock::time_point now)
  {
    if (now < retry_rotation_at_)
      return;
    drain(false);
    if (compressor_)
    {
      compressor_->end_frame(compressed_);
      unflushed_input_ = 0;
      write_disk();
    }
    file_.close();

    const fs::path target = rotated_path(now);
    if (const auto ec = platform::rename_file(active_, target))
    {
      // Keep logging into the current file; try again a little later.
      retry_rotation_at_ = now + std::chrono::seconds(30);
      file_.open(active_, platform::File::Mode::append, false);
      disk_size_ = file_.size();
      frame_start_ = disk_size_;
      update_boundary(now);
      report_error("cannot rotate '" + active_.string() + "': " + ec.message());
      return;
    }
    counters().rotations.fetch_add(1, std::memory_order_relaxed);
    file_.open(active_, platform::File::Mode::append, false);
    disk_size_ = file_.size();
    frame_start_ = disk_size_;
    update_boundary(now);
    schedule_maintenance(target);
  }

  void FileSink::schedule_maintenance(const fs::path &rotated)
  {
    const bool compress = !compressor_ && config_.rotation.compress == Compression::zstd;
    bool queued = false;
    if (compress)
    {
      fs::path target = rotated;
      target += ".zst";
      const int level = config_.rotation.compress_level;
      // Left uncompressed when refused (after shutdown()); picked up at the next start.
      queued = worker_.post([rotated, target, level](const std::atomic<bool> &stop)
                            {
                              std::error_code ec;
                              if (fs::exists(rotated, ec)) // may have been pruned meanwhile
                                zstd::compress_file(rotated, target, level, stop); });
    }
    if (config_.rotation.max_files > 0)
    {
      // Retention runs after compression, on the same thread, so it never
      // deletes a file that is being compressed.
      if (!queued || !worker_.post([this](const std::atomic<bool> &)
                                   { prune(); }))
        prune();
    }
  }

  void FileSink::pick_up_leftovers()
  {
    std::error_code ec;
    std::vector<fs::path> pending;
    for (fs::directory_iterator it(dir_, ec), end; !ec && it != end; it.increment(ec))
    {
      RotatedName parsed;
      const auto &path = it->path();
      if (!parse_rotated(path.filename().native(), stem_, extension_, parsed))
        continue;
      std::error_code ignore;
      if (parsed.temporary)
      {
        fs::remove(path, ignore); // an interrupted compression
        continue;
      }
      if (parsed.compressed)
        continue;
      fs::path compressed = path;
      compressed += ".zst";
      if (fs::exists(compressed, ignore))
        fs::remove(path, ignore); // compressed, but the source was not removed yet
      else
        pending.push_back(path);
    }
    std::sort(pending.begin(), pending.end());
    for (const auto &path : pending)
      schedule_maintenance(path);
    if (pending.empty() && config_.rotation.max_files > 0)
      prune();
  }

  void FileSink::prune() noexcept
  {
    try
    {
      std::map<std::string, std::vector<fs::path>> groups;
      std::error_code ec;
      for (fs::directory_iterator it(dir_, ec), end; !ec && it != end; it.increment(ec))
      {
        RotatedName parsed;
        if (parse_rotated(it->path().filename().native(), stem_, extension_, parsed) && !parsed.temporary)
          groups[parsed.key].push_back(it->path());
      }
      std::size_t excess = groups.size() > config_.rotation.max_files ? groups.size() - config_.rotation.max_files : 0;
      for (auto it = groups.begin(); excess > 0 && it != groups.end(); ++it, --excess)
      {
        for (const auto &path : it->second)
        {
          std::error_code remove_error;
          fs::remove(path, remove_error);
          if (remove_error)
            report_error("cannot remove old log '" + path.string() + "': " + remove_error.message());
        }
      }
    }
    catch (const std::exception &e)
    {
      report_error(std::string("log retention failed: ") + e.what());
    }
  }

  void FileSink::write(const Record &record)
  {
    if (next_boundary_ != 0 && epoch_seconds(record.time) >= next_boundary_)
      rotate(record.time);

    const std::size_t before = text_.size();
    formatter_.format(record, text_);

    if (const auto limit = config_.rotation.max_size; limit > 0)
    {
      // Compressed size is known only for data already through the compressor,
      // so a compressed file may overshoot the limit by about one block.
      const std::uint64_t existing = compressor_ ? disk_size_ + compressed_.size() : disk_size_ + before;
      const std::uint64_t projected = compressor_ ? existing : disk_size_ + text_.size();
      if (existing > 0 && (compressor_ ? projected >= limit : projected > limit))
      {
        std::string line(text_, before);
        text_.resize(before);
        rotate(record.time);
        text_ += line;
      }
    }
    if (text_.size() >= config_.buffer_size)
      drain(false);
  }

  void FileSink::flush()
  {
    drain(true, forced_flush());
  }

  void FileSink::wait_idle()
  {
    worker_.wait_idle();
  }

  void FileSink::stop_background()
  {
    // Leave a complete, decodable file behind; a later write opens a new frame.
    drain(false);
    if (compressor_)
    {
      compressor_->end_frame(compressed_);
      unflushed_input_ = 0;
      write_disk();
      frame_start_ = disk_size_;
    }
    worker_.stop();
  }
}

namespace tinylog
{
  std::shared_ptr<Sink> make_file_sink(const FileSinkConfig &config)
  {
    return std::make_shared<detail::FileSink>(config);
  }
}
