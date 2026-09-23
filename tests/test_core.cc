#include "test_util.hpp"

#include <thread>

using tinylog::Level;
using tinylog::Mode;

TEST(Levels, ThresholdMask)
{
  EXPECT_EQ(tinylog::mask_from(Level::trace), 0x7Fu);
  EXPECT_EQ(tinylog::mask_from(Level::info), 0x7Cu);
  EXPECT_EQ(tinylog::mask_from(Level::fatal), 0x40u);
  EXPECT_EQ(tinylog::mask_from(Level::off), 0u);
  EXPECT_EQ(tinylog::level_bit(Level::off), 0u);
}

TEST(Levels, ParseAndPrint)
{
  EXPECT_EQ(tinylog::parse_level("WARN"), Level::warning);
  EXPECT_EQ(tinylog::parse_level(" error "), Level::error);
  EXPECT_EQ(tinylog::parse_level("verbose"), Level::trace);
  EXPECT_EQ(tinylog::parse_level("3"), Level::warning);
  EXPECT_EQ(tinylog::parse_level("bogus"), std::nullopt);
  EXPECT_EQ(tinylog::to_string(Level::critical), "critical");
}

TEST(Levels, SetEnableDisable)
{
  test::Capture capture;
  tinylog::init(test::config_for(capture));
  tinylog::set_level(Level::warning);
  EXPECT_EQ(tinylog::level(), Level::warning);
  EXPECT_FALSE(tinylog::should_log(Level::info));
  EXPECT_TRUE(tinylog::should_log(Level::error));

  tinylog::enable(Level::debug);
  EXPECT_TRUE(tinylog::should_log(Level::debug));
  EXPECT_FALSE(tinylog::should_log(Level::info));
  tinylog::disable(Level::error);
  EXPECT_FALSE(tinylog::should_log(Level::error));

  TLOG_INFO("dropped");
  TLOG_DEBUG("kept debug");
  TLOG_ERROR("dropped error");
  TLOG_WARN("kept warning");
  EXPECT_EQ(capture.texts(), (std::vector<std::string>{"kept debug", "kept warning"}));
}

TEST(Core, DisabledMacroDoesNotEvaluateArguments)
{
  test::Capture capture;
  tinylog::init(test::config_for(capture));
  tinylog::set_level(Level::info);
  int calls = 0;
  auto expensive = [&]
  {
    ++calls;
    return 42;
  };
  TLOG_DEBUG("value {}", expensive());
  EXPECT_EQ(calls, 0);
  TLOG_INFO("value {}", expensive());
  EXPECT_EQ(calls, 1);
  EXPECT_EQ(capture.texts(), std::vector<std::string>{"value 42"});
}

TEST(Core, FormatsArguments)
{
  test::Capture capture;
  tinylog::init(test::config_for(capture));
  TLOG_INFO("{} + {} = {:.1f} [{:>4}] {}", 1, 2, 3.0, "x", std::string("str"));
  EXPECT_EQ(capture.texts(), std::vector<std::string>{"1 + 2 = 3.0 [   x] str"});
}

TEST(Core, RuntimeFormatErrorIsLoggedNotThrown)
{
  test::Capture capture;
  tinylog::init(test::config_for(capture));
  std::string format = "{} and {}";
  EXPECT_NO_THROW(tinylog::log_runtime(Level::info, {}, format, 1));
  const auto texts = capture.texts();
  ASSERT_EQ(texts.size(), 1u);
  EXPECT_EQ(texts[0].rfind("[format error", 0), 0u) << texts[0];
  EXPECT_NE(texts[0].find("{} and {}"), std::string::npos);
}

TEST(Core, SourceLocationIsCaptured)
{
  std::vector<tinylog::SourceLocation> seen;
  tinylog::Config config;
  config.console.reset();
  config.use_env = false;
  config.sinks.push_back(std::make_shared<tinylog::CallbackSink>([&](const tinylog::Record &r)
                                                                 { seen.push_back(r.where); }));
  tinylog::init(config);
  const auto line = __LINE__ + 1;
  TLOG_INFO("here");
  ASSERT_EQ(seen.size(), 1u);
  EXPECT_EQ(seen[0].line, static_cast<std::uint32_t>(line));
  EXPECT_NE(std::string(seen[0].file).find("test_core.cc"), std::string::npos);
}

namespace
{
  void hammer(Mode mode, int threads, int per_thread)
  {
    test::Capture capture;
    tinylog::init(test::config_for(capture, mode));
    std::vector<std::thread> workers;
    for (int t = 0; t < threads; ++t)
    {
      workers.emplace_back([t, per_thread]
                           {
                             for (int i = 0; i < per_thread; ++i)
                               TLOG_INFO("{} {}", t, i); });
    }
    for (auto &w : workers)
      w.join();
    tinylog::flush();

    const auto lines = capture.lines();
    ASSERT_EQ(lines.size(), static_cast<std::size_t>(threads * per_thread));
    // Every message intact, and each thread's messages in the order it logged them.
    std::vector<int> next(static_cast<std::size_t>(threads), 0);
    for (const auto &line : lines)
    {
      std::istringstream in(line.text);
      int t = -1, i = -1;
      in >> t >> i;
      ASSERT_TRUE(t >= 0 && t < threads) << line.text;
      ASSERT_EQ(i, next[static_cast<std::size_t>(t)]++) << "thread " << t;
    }
    EXPECT_EQ(tinylog::stats().logged, static_cast<std::uint64_t>(threads * per_thread));
    EXPECT_EQ(tinylog::stats().dropped, 0u);
  }
}

TEST(Core, SyncManyThreadsLoseNothing) { hammer(Mode::sync, 8, 5000); }
TEST(Core, AsyncManyThreadsLoseNothing) { hammer(Mode::async, 8, 5000); }

TEST(Core, AsyncBlockPolicyWithTinyQueueLosesNothing)
{
  test::Capture capture;
  auto config = test::config_for(capture, Mode::async);
  config.queue_capacity = 4096;
  config.overflow = tinylog::OverflowPolicy::block;
  tinylog::init(config);
  for (int i = 0; i < 20000; ++i)
    TLOG_INFO("message number {} with some padding to fill the queue", i);
  tinylog::flush();
  EXPECT_EQ(capture.lines().size(), 20000u);
}

TEST(Core, AsyncDropPolicyCountsDrops)
{
  std::atomic<int> delivered{0};
  tinylog::Config config;
  config.console.reset();
  config.use_env = false;
  config.mode = Mode::async;
  config.queue_capacity = 4096;
  config.overflow = tinylog::OverflowPolicy::drop;
  config.sinks.push_back(std::make_shared<tinylog::CallbackSink>([&](const tinylog::Record &)
                                                                 {
                                                                   std::this_thread::sleep_for(std::chrono::microseconds(50));
                                                                   ++delivered; }));
  tinylog::init(config);
  constexpr int total = 20000;
  for (int i = 0; i < total; ++i)
    TLOG_INFO("message number {} with some padding to fill the queue", i);
  tinylog::flush();
  const auto stats = tinylog::stats();
  EXPECT_GT(stats.dropped, 0u);
  EXPECT_EQ(static_cast<std::uint64_t>(delivered.load()) + stats.dropped, static_cast<std::uint64_t>(total));
}

namespace
{
  struct CountingSink : tinylog::Sink
  {
    std::atomic<int> writes{0};
    std::atomic<int> flushes{0};
    void write(const tinylog::Record &) override { ++writes; }
    void flush() override { ++flushes; }
  };
}

TEST(Core, FlushLevelControlsFlushing)
{
  auto sink = std::make_shared<CountingSink>();
  tinylog::Config config;
  config.console.reset();
  config.use_env = false;
  config.flush_level = Level::error;
  config.flush_interval = std::chrono::milliseconds(0);
  config.sinks.push_back(sink);
  tinylog::init(config);
  const int before = sink->flushes;
  TLOG_INFO("no flush");
  TLOG_WARN("no flush");
  EXPECT_EQ(sink->flushes, before);
  TLOG_ERROR("flush");
  EXPECT_EQ(sink->flushes, before + 1);
}

TEST(Core, SinkMinLevelFilters)
{
  auto sink = std::make_shared<CountingSink>();
  sink->min_level = Level::error;
  tinylog::Config config;
  config.console.reset();
  config.use_env = false;
  config.sinks.push_back(sink);
  tinylog::init(config);
  TLOG_INFO("filtered");
  TLOG_ERROR("passes");
  EXPECT_EQ(sink->writes, 1);
}

TEST(Core, SinkThatLogsDoesNotDeadlock)
{
  int writes = 0;
  tinylog::Config config;
  config.console.reset();
  config.use_env = false;
  config.sinks.push_back(std::make_shared<tinylog::CallbackSink>([&](const tinylog::Record &)
                                                                 {
                                                                   ++writes;
                                                                   TLOG_ERROR("from inside a sink"); }));
  tinylog::init(config);
  TLOG_INFO("outer");
  EXPECT_EQ(writes, 1);
  EXPECT_GE(tinylog::stats().errors, 1u);
}

TEST(Core, ThrowingSinkIsContained)
{
  test::Capture capture;
  auto config = test::config_for(capture);
  config.sinks.insert(config.sinks.begin(), std::make_shared<tinylog::CallbackSink>([](const tinylog::Record &)
                                                                                    { throw std::runtime_error("sink failure"); }));
  tinylog::init(config);
  EXPECT_NO_THROW(TLOG_INFO("still delivered"));
  EXPECT_EQ(capture.texts(), std::vector<std::string>{"still delivered"});
  EXPECT_EQ(tinylog::stats().errors, 1u);
}

TEST(Core, LoggingAfterShutdownIsSynchronous)
{
  test::Capture capture;
  tinylog::init(test::config_for(capture, Mode::async));
  TLOG_INFO("before");
  tinylog::shutdown();
  TLOG_INFO("after");
  EXPECT_EQ(capture.texts(), (std::vector<std::string>{"before", "after"}));
}

TEST(Core, ReinitSwitchesSinks)
{
  test::Capture first, second;
  tinylog::init(test::config_for(first, Mode::async));
  TLOG_INFO("one");
  tinylog::init(test::config_for(second, Mode::sync));
  TLOG_INFO("two");
  EXPECT_EQ(first.texts(), std::vector<std::string>{"one"});
  EXPECT_EQ(second.texts(), std::vector<std::string>{"two"});
}

TEST(Core, AddSinkAtRuntime)
{
  test::Capture first, second;
  tinylog::init(test::config_for(first));
  TLOG_INFO("one");
  tinylog::add_sink(second.sink());
  TLOG_INFO("two");
  EXPECT_EQ(first.texts(), (std::vector<std::string>{"one", "two"}));
  EXPECT_EQ(second.texts(), std::vector<std::string>{"two"});
}

TEST(Core, WritePreformatted)
{
  test::Capture capture;
  tinylog::init(test::config_for(capture));
  tinylog::write(Level::warning, "{not a format}");
  const auto lines = capture.lines();
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(lines[0].text, "{not a format}");
  EXPECT_EQ(lines[0].level, Level::warning);
}

TEST(Core, Version)
{
  EXPECT_EQ(tinylog::version(), TINYLOG_VERSION_STRING);
  EXPECT_EQ(tinylog::version_number(), TINYLOG_VERSION_NUMBER);
}

TEST(Core, EnvironmentOverridesConfig)
{
  test::set_env("TINYLOG_LEVEL", "error");
  test::Capture capture;
  auto config = test::config_for(capture);
  config.use_env = true;
  tinylog::init(config);
  test::set_env("TINYLOG_LEVEL", nullptr);
  TLOG_WARN("dropped");
  TLOG_ERROR("kept");
  EXPECT_EQ(capture.texts(), std::vector<std::string>{"kept"});
}
