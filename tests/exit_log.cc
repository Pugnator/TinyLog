// Logging after main() returns (from an atexit handler) must neither touch
// freed memory nor lose the line.
//
// exit() destroys the main thread's thread_local objects before it runs the
// atexit handlers. vlog() formats into per-thread buffers, so a line logged
// from a handler used to be written into freed memory: a heap
// use-after-free, which AddressSanitizer reports and which corrupted the
// heap of a real application about one exit in six.
#include <tinylog/tinylog.hpp>

#include <cstdlib>

namespace
{
  void logAfterMain()
  {
    TLOG_INFO("logged after main {}", 42);
  }
} // namespace

int main()
{
  tinylog::Config config;
  config.use_env = false;
  config.mode = tinylog::Mode::sync;
  config.console = tinylog::ConsoleSinkConfig{};
  tinylog::init(config);

  TLOG_INFO("logged in main {}", 1); // the thread's buffers now exist
  std::atexit(logAfterMain);
  return 0;
}
