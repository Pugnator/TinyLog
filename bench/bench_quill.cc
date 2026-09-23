// The same scenarios with Quill (deferred formatting on a backend thread), for comparison.
#define QUILL_DISABLE_NON_PREFIXED_MACROS
#include "harness.hpp"

#include <quill/Backend.h>
#include <quill/Frontend.h>
#include <quill/LogMacros.h>
#include <quill/Logger.h>
#include <quill/sinks/FileSink.h>

#include <mutex>

namespace
{
  quill::Logger *g_logger = nullptr;
  int g_run = 0;

  void async_file(const std::filesystem::path &dir)
  {
    static std::once_flag started;
    std::call_once(started, []
                   { quill::Backend::start(); });
    // Sinks are looked up by file name: a fresh name per run.
    const auto path = (dir / ("bench-" + std::to_string(++g_run) + ".log")).string();
    auto sink = quill::Frontend::create_or_get_sink<quill::FileSink>(path, quill::FileSinkConfig{}, quill::FileEventNotifier{});
    quill::PatternFormatterOptions options;
    options.format_pattern = "[%(time)] [%(log_level)] %(message)";
    options.timestamp_pattern = "%Y-%m-%d %H:%M:%S.%Qms";
    g_logger = quill::Frontend::create_or_get_logger("bench" + std::to_string(g_run), std::move(sink), options);
  }

  void log_typical(int i, int t)
  {
    QUILL_LOG_INFO(g_logger, "iteration {} thread {} value {:.3f} name {}", i, t, i * 0.5, "benchmark");
  }

  void finish()
  {
    g_logger->flush_log();
    quill::Frontend::remove_logger_blocking(g_logger);
    g_logger = nullptr;
  }

  const bench::Register disabled({"quill", "disabled level", 1, async_file, [](int i, int t)
                                  { QUILL_LOG_DEBUG(g_logger, "iteration {} thread {} value {:.3f}", i, t, i * 0.5); },
                                  finish});
  const bench::Register async_1({"quill", "file, async", 1, async_file, log_typical, finish});
  const bench::Register async_4({"quill", "file, async", 4, async_file, log_typical, finish});
}
