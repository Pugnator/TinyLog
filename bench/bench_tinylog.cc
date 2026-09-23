#include "harness.hpp"

#include <tinylog/tinylog.hpp>

namespace
{
  using tinylog::Level;

  //! Discards records: measures the front end and dispatch only.
  struct NullSink : tinylog::Sink
  {
    void write(const tinylog::Record &) override {}
  };

  tinylog::Config base_config()
  {
    tinylog::Config config;
    config.console.reset();
    config.use_env = false;
    config.level = Level::info;
    return config;
  }

  void log_typical(int i, int t)
  {
    TLOG_INFO("iteration {} thread {} value {:.3f} name {}", i, t, i * 0.5, "benchmark");
  }

  void finish()
  {
    tinylog::shutdown();
  }

  tinylog::FileSinkConfig file_at(const std::filesystem::path &dir)
  {
    tinylog::FileSinkConfig file;
    file.path = dir / "bench.log";
    return file;
  }

  bench::Scenario scenario(std::string name, int threads, std::function<void(const std::filesystem::path &)> setup,
                           std::function<void(int, int)> log = log_typical)
  {
    return bench::Scenario{"tinylog", std::move(name), threads, std::move(setup), std::move(log), finish};
  }

  const bench::Register disabled(scenario("disabled level", 1, [](const std::filesystem::path &)
                                          { tinylog::init(base_config()); },
                                          [](int i, int t)
                                          { TLOG_DEBUG("iteration {} thread {} value {:.3f}", i, t, i * 0.5); }));

  const bench::Register null_sync(scenario("null sink, sync", 1, [](const std::filesystem::path &)
                                           {
                                             auto config = base_config();
                                             config.sinks.push_back(std::make_shared<NullSink>());
                                             tinylog::init(config); }));

  const bench::Register file_durable(scenario("file, sync, flush every record", 1, [](const std::filesystem::path &dir)
                                              {
                                                auto config = base_config();
                                                config.files.push_back(file_at(dir));
                                                tinylog::init(config); }));

  const bench::Register file_buffered(scenario("file, sync, buffered", 1, [](const std::filesystem::path &dir)
                                               {
                                                 auto config = base_config();
                                                 config.flush_level = Level::error;
                                                 config.files.push_back(file_at(dir));
                                                 tinylog::init(config); }));

  void async_file(tinylog::Config &config, const std::filesystem::path &dir)
  {
    config.mode = tinylog::Mode::async;
    config.flush_level = Level::error;
    config.queue_capacity = 16u << 20;
    config.files.push_back(file_at(dir));
  }

  const bench::Register file_async(scenario("file, async", 1, [](const std::filesystem::path &dir)
                                            {
                                              auto config = base_config();
                                              async_file(config, dir);
                                              tinylog::init(config); }));

  const bench::Register file_async_4(scenario("file, async", 4, [](const std::filesystem::path &dir)
                                              {
                                                auto config = base_config();
                                                async_file(config, dir);
                                                tinylog::init(config); }));

  const bench::Register file_sync_4(scenario("file, sync, buffered", 4, [](const std::filesystem::path &dir)
                                             {
                                               auto config = base_config();
                                               config.flush_level = Level::error;
                                               config.files.push_back(file_at(dir));
                                               tinylog::init(config); }));

  const bench::Register live_zstd(scenario("file, async, live zstd level 3", 1, [](const std::filesystem::path &dir)
                                           {
                                             auto config = base_config();
                                             async_file(config, dir);
                                             config.files.back().compression = tinylog::Compression::zstd;
                                             tinylog::init(config); }));

  const bench::Register live_zstd_4(scenario("file, async, live zstd level 3", 4, [](const std::filesystem::path &dir)
                                             {
                                               auto config = base_config();
                                               async_file(config, dir);
                                               config.files.back().compression = tinylog::Compression::zstd;
                                               tinylog::init(config); }));

  // Throughput here includes waiting for the background compression to finish.
  const bench::Register rotate_zstd(bench::Scenario{
      "tinylog", "file, async, rotate 4 MiB + bg zstd 9 (until compressed)", 1,
      [](const std::filesystem::path &dir)
      {
        auto config = base_config();
        async_file(config, dir);
        auto &file = config.files.back();
        file.rotation.max_size = 4u << 20;
        file.rotation.max_files = 0;
        file.rotation.compress = tinylog::Compression::zstd;
        file.rotation.compress_level = 9;
        tinylog::init(config);
      },
      log_typical,
      []
      {
        tinylog::wait_idle();
        tinylog::shutdown();
      }});
}
