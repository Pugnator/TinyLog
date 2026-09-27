#include "test_util.hpp"

#include <thread>

using tinylog::Level;
using tinylog::Mode;

namespace
{
  //! Records the channel name next to the text.
  struct ChannelCapture
  {
    std::mutex mutex;
    std::vector<std::pair<std::string, std::string>> lines; //!< (channel, text)

    std::shared_ptr<tinylog::Sink> sink()
    {
      return std::make_shared<tinylog::CallbackSink>([this](const tinylog::Record &r)
                                                     {
                                                       std::lock_guard<std::mutex> lock(mutex);
                                                       lines.emplace_back(std::string(r.channel), std::string(r.message)); });
    }
    std::vector<std::pair<std::string, std::string>> snapshot()
    {
      std::lock_guard<std::mutex> lock(mutex);
      return lines;
    }
  };

  tinylog::Config quiet_config(test::Capture &global, Mode mode = Mode::sync)
  {
    tinylog::Config config = test::config_for(global);
    config.mode = mode;
    return config;
  }
}

TEST(Channels, RecordsReachOnlyTheChannelsSinks)
{
  test::Capture global;
  tinylog::init(quiet_config(global));
  ChannelCapture own;
  tinylog::ChannelConfig config;
  config.sinks.push_back(own.sink());
  auto agent = tinylog::open_channel("agent-3", config);
  ASSERT_TRUE(agent);
  EXPECT_EQ(agent.name(), "agent-3");

  TLOG_INFO_TO(agent, "connected to {}", "server");
  TLOG_INFO("a global line");
  tinylog::flush();

  EXPECT_EQ(own.snapshot(), (std::vector<std::pair<std::string, std::string>>{{"agent-3", "connected to server"}}));
  EXPECT_EQ(global.texts(), (std::vector<std::string>{"a global line"}));
}

TEST(Channels, FindCloseAndReopen)
{
  test::Capture global;
  tinylog::init(quiet_config(global));
  ChannelCapture own;
  tinylog::ChannelConfig config;
  config.sinks.push_back(own.sink());
  auto opened = tinylog::open_channel("jobs", config);
  auto found = tinylog::find_channel("jobs");
  ASSERT_TRUE(found);
  TLOG_INFO_TO(found, "through the found handle");
  EXPECT_FALSE(tinylog::find_channel("nothing"));
  EXPECT_THROW(tinylog::open_channel("jobs", config), std::invalid_argument);

  EXPECT_TRUE(tinylog::close_channel("jobs"));
  EXPECT_FALSE(tinylog::close_channel("jobs"));
  EXPECT_FALSE(opened);
  TLOG_INFO_TO(opened, "after closing");
  EXPECT_EQ(own.snapshot().size(), 1u) << "the line logged before closing arrived, the one after did not";

  // The name is free again.
  auto again = tinylog::open_channel("jobs", config);
  EXPECT_TRUE(again);
  tinylog::close_channel("jobs");
}

TEST(Channels, ForwardAlsoDeliversToTheGlobalSinks)
{
  test::Capture global;
  tinylog::init(quiet_config(global));
  ChannelCapture own;
  tinylog::ChannelConfig config;
  config.sinks.push_back(own.sink());
  config.forward = true;
  auto channel = tinylog::open_channel("forwarded", config);
  TLOG_WARN_TO(channel, "both places");
  tinylog::flush();
  EXPECT_EQ(own.snapshot().size(), 1u);
  EXPECT_EQ(global.texts(), (std::vector<std::string>{"both places"}));
  tinylog::close_channel("forwarded");
}

TEST(Channels, LevelIsTheChannelsOwn)
{
  test::Capture global;
  tinylog::init(quiet_config(global));
  tinylog::set_level(Level::error);
  ChannelCapture own;
  tinylog::ChannelConfig config;
  config.sinks.push_back(own.sink());
  config.level = Level::debug;
  auto channel = tinylog::open_channel("verbose", config);

  EXPECT_TRUE(channel.should_log(Level::debug));
  EXPECT_FALSE(channel.should_log(Level::trace));
  TLOG_DEBUG_TO(channel, "kept despite the global level");
  channel.set_level(Level::warning);
  TLOG_INFO_TO(channel, "below the channel's new level");
  channel.set_level(Level::off);
  EXPECT_TRUE(channel) << "a channel at level off is still open";
  TLOG_FATAL_TO(channel, "off means nothing");
  tinylog::flush();
  EXPECT_EQ(own.snapshot().size(), 1u);
  tinylog::close_channel("verbose");
}

TEST(Channels, DisabledMacroDoesNotEvaluateArguments)
{
  test::Capture global;
  tinylog::init(quiet_config(global));
  tinylog::ChannelConfig config;
  config.level = Level::warning;
  auto channel = tinylog::open_channel("lazy", config);
  int calls = 0;
  auto expensive = [&]
  {
    ++calls;
    return 1;
  };
  TLOG_INFO_TO(channel, "{}", expensive());
  tinylog::Channel empty;
  TLOG_ERROR_TO(empty, "{}", expensive());
  EXPECT_EQ(calls, 0);
  tinylog::close_channel("lazy");
}

TEST(Channels, AsyncManyThreadsManyChannelsLoseNothing)
{
  test::Capture global;
  tinylog::init(quiet_config(global, Mode::async));
  constexpr int channels = 4;
  constexpr int threads = 8;
  constexpr int per_thread = 2000;
  std::vector<ChannelCapture> captures(channels);
  std::vector<tinylog::Channel> handles;
  for (int c = 0; c < channels; ++c)
  {
    tinylog::ChannelConfig config;
    config.sinks.push_back(captures[static_cast<std::size_t>(c)].sink());
    handles.push_back(tinylog::open_channel("c" + std::to_string(c), config));
  }
  std::vector<std::thread> workers;
  for (int t = 0; t < threads; ++t)
  {
    workers.emplace_back([&, t]
                         {
                           for (int i = 0; i < per_thread; ++i)
                             TLOG_INFO_TO(handles[static_cast<std::size_t>((t + i) % channels)], "{} {}", t, i); });
  }
  for (auto &worker : workers)
    worker.join();
  tinylog::flush();
  std::size_t total = 0;
  for (auto &capture : captures)
    total += capture.snapshot().size();
  EXPECT_EQ(total, static_cast<std::size_t>(threads * per_thread));
  EXPECT_TRUE(global.texts().empty());
  for (int c = 0; c < channels; ++c)
    tinylog::close_channel("c" + std::to_string(c));
}

TEST(Channels, CloseDeliversWhatWasQueued)
{
  test::Capture global;
  tinylog::init(quiet_config(global, Mode::async));
  ChannelCapture own;
  tinylog::ChannelConfig config;
  config.sinks.push_back(own.sink());
  auto channel = tinylog::open_channel("queued", config);
  for (int i = 0; i < 1000; ++i)
    TLOG_INFO_TO(channel, "line {}", i);
  tinylog::close_channel("queued");
  EXPECT_EQ(own.snapshot().size(), 1000u);
}

TEST(Channels, SurviveReinitAndShutdown)
{
  test::Capture first;
  tinylog::init(quiet_config(first, Mode::async));
  ChannelCapture own;
  tinylog::ChannelConfig config;
  config.sinks.push_back(own.sink());
  auto channel = tinylog::open_channel("lasting", config);
  TLOG_INFO_TO(channel, "before init");
  test::Capture second;
  tinylog::init(quiet_config(second));
  TLOG_INFO_TO(channel, "after init");
  tinylog::shutdown();
  TLOG_INFO_TO(channel, "after shutdown");
  EXPECT_EQ(own.snapshot().size(), 3u);
  tinylog::close_channel("lasting");
}

TEST(Channels, TextAndJsonNameTheChannel)
{
  test::TempDir dir;
  test::Capture global;
  tinylog::init(quiet_config(global));
  tinylog::ChannelConfig config;
  auto &text = config.files.emplace_back();
  text.path = dir / "text.log";
  auto &json = config.files.emplace_back();
  json.path = dir / "json.log";
  json.layout.format = tinylog::Format::json;
  auto channel = tinylog::open_channel("agent-7", config);
  TLOG_INFO_TO(channel, "hello");
  tinylog::close_channel("agent-7");

  const auto text_log = tinylog::read_log(dir / "text.log");
  EXPECT_NE(text_log.find("[info] [agent-7] hello"), std::string::npos) << text_log;
  const auto json_log = tinylog::read_log(dir / "json.log");
  EXPECT_NE(json_log.find("\"level\":\"info\",\"channel\":\"agent-7\""), std::string::npos) << json_log;

  // A global record carries no channel; the flag can hide a channel's name.
  tinylog::Record record;
  record.message = "plain";
  std::string out;
  tinylog::Layout layout;
  layout.timestamp = tinylog::TimestampPrecision::none;
  tinylog::format_record(record, layout, out);
  EXPECT_EQ(out, "[info] plain\n");
  record.channel = "named";
  layout.show_channel = false;
  out.clear();
  tinylog::format_record(record, layout, out);
  EXPECT_EQ(out, "[info] plain\n");
}

TEST(Channels, FileChannelRotatesWithinItsLimit)
{
  test::TempDir dir;
  test::Capture global;
  tinylog::init(quiet_config(global, Mode::async));
  tinylog::ChannelConfig config;
  auto &file = config.files.emplace_back();
  file.path = dir / "agent.log";
  file.rotation.max_size = 64u << 10;
  file.rotation.max_files = 3;
  file.rotation.compress = tinylog::Compression::zstd;
  auto channel = tinylog::open_channel("rotating", config);
  const std::string filler(500, 'x');
  for (int i = 0; i < 2000; ++i)
    TLOG_INFO_TO(channel, "{} {}", i, filler);
  TLOG_INFO_TO(channel, "the last line");
  tinylog::close_channel("rotating");

  std::size_t files = 0;
  for (const auto &entry : std::filesystem::directory_iterator(dir.path()))
  {
    ++files;
    EXPECT_LE(entry.file_size(), 64u << 10) << entry.path();
  }
  EXPECT_GT(files, 1u) << "it rotated";
  EXPECT_LE(files, 4u) << "the active file and three rotated ones";
  EXPECT_NE(tinylog::read_log(dir / "agent.log").find("the last line"), std::string::npos);
}

TEST(Channels, ConfigStringKey)
{
  const auto config = tinylog::parse_config("channel=off");
  ASSERT_TRUE(config.console);
  EXPECT_FALSE(config.console->layout.show_channel);
}
