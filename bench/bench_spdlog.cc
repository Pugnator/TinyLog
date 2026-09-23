// The same scenarios with spdlog, for comparison (TINYLOG_BENCH_COMPARE=ON).
#include "harness.hpp"

#include <spdlog/async.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/spdlog.h>

namespace
{
  std::shared_ptr<spdlog::logger> g_logger;

  void configure(std::shared_ptr<spdlog::logger> logger)
  {
    logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
    logger->set_level(spdlog::level::info);
    logger->flush_on(spdlog::level::err);
    g_logger = std::move(logger);
  }

  void log_typical(int i, int t)
  {
    g_logger->info("iteration {} thread {} value {:.3f} name {}", i, t, i * 0.5, "benchmark");
  }

  void finish()
  {
    g_logger->flush();
    g_logger.reset();
    spdlog::drop_all();
    spdlog::shutdown();
  }

  void sync_file(const std::filesystem::path &dir)
  {
    configure(spdlog::basic_logger_mt("bench", (dir / "bench.log").string()));
  }

  void async_file(const std::filesystem::path &dir)
  {
    spdlog::init_thread_pool(1u << 17, 1);
    configure(spdlog::basic_logger_mt<spdlog::async_factory>("bench", (dir / "bench.log").string()));
  }

  const bench::Register disabled({"spdlog", "disabled level", 1, sync_file, [](int i, int t)
                                  { g_logger->debug("iteration {} thread {} value {:.3f}", i, t, i * 0.5); },
                                  finish});
  const bench::Register sync_1({"spdlog", "file, sync, buffered", 1, sync_file, log_typical, finish});
  const bench::Register sync_4({"spdlog", "file, sync, buffered", 4, sync_file, log_typical, finish});
  const bench::Register async_1({"spdlog", "file, async", 1, async_file, log_typical, finish});
  const bench::Register async_4({"spdlog", "file, async", 4, async_file, log_typical, finish});
}
