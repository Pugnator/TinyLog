// Links the shared library (DLL): every public entry point must be exported.
#include <log.hpp>
#include <tinylog/reader.hpp>
#include <tinylog/tinylog.h>
#include <tinylog/tinylog.hpp>

#include <cstdio>
#include <filesystem>

#define CHECK(condition)                                                    \
  do                                                                        \
  {                                                                         \
    if (!(condition))                                                       \
    {                                                                       \
      std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, \
                   #condition);                                             \
      return 1;                                                             \
    }                                                                       \
  } while (0)

int main()
{
  namespace fs = std::filesystem;
  const fs::path dir = fs::temp_directory_path() / "tinylog-shared-smoke";
  std::error_code ec;
  fs::remove_all(dir, ec);

  CHECK(tinylog::version() == TINYLOG_VERSION_STRING);

  tinylog::Config config;
  config.console.reset();
  config.use_env = false;
  config.mode = tinylog::Mode::async;
  config.level = tinylog::Level::debug;
  auto &file = config.files.emplace_back();
  file.path = dir / "smoke.log";
  file.compression = tinylog::Compression::zstd;
  int callbacks = 0;
  config.sinks.push_back(std::make_shared<tinylog::CallbackSink>([&](const tinylog::Record &)
                                                                 { ++callbacks; }));
  tinylog::init(config);
  TLOG_DEBUG("shared {}", 1);
  LOG_INFO("legacy {}", 2); // the compat facade shares the DLL's singleton
  tinylog_write(3, nullptr, 0, nullptr, "c api", 5);
  CHECK(tinylog::should_log(tinylog::Level::debug));
  tinylog::shutdown();
  CHECK(callbacks == 3);
  CHECK(tinylog::stats().logged == 3);

  tinylog::ReadResult result;
  const auto text = tinylog::read_log(dir / "smoke.log.zst", &result);
  CHECK(!result.truncated && !result.corrupt);
  CHECK(text.find("shared 1") != std::string::npos);
  CHECK(text.find("legacy 2") != std::string::npos);
  CHECK(text.find("c api") != std::string::npos);

  const auto parsed = tinylog::parse_config("level=warning");
  CHECK(parsed.level == tinylog::Level::warning);
  fs::remove_all(dir, ec);
  std::puts("shared smoke test passed");
  return 0;
}
