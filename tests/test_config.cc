#include "test_util.hpp"

using namespace std::chrono_literals;
using tinylog::Compression;
using tinylog::Level;

TEST(Config, EmptyStringGivesDefaults)
{
  const auto config = tinylog::parse_config("");
  EXPECT_EQ(config.level, Level::info);
  EXPECT_EQ(config.mode, tinylog::Mode::sync);
  ASSERT_TRUE(config.console.has_value());
  EXPECT_TRUE(config.files.empty());
}

TEST(Config, FullString)
{
  const auto config = tinylog::parse_config(
      "level=debug; mode=async; queue=8M; overflow=drop; flush=warning; flush_interval=250ms\n"
      "console=stderr; console.color=never; timestamp=us; thread=on\n"
      "# a comment line\n"
      "file=logs/app.log; file.level=info; file.compress=zstd; file.compress_level=5; file.frame=1M;"
      "rotate.size=10MiB; rotate.interval=1d; rotate.keep=7; rotate.on_open=yes\n"
      "file=logs/errors.jsonl; file.level=error; file.format=json; rotate.compress=zstd; rotate.compress_level=12");

  EXPECT_EQ(config.level, Level::debug);
  EXPECT_EQ(config.mode, tinylog::Mode::async);
  EXPECT_EQ(config.queue_capacity, 8u << 20);
  EXPECT_EQ(config.overflow, tinylog::OverflowPolicy::drop);
  EXPECT_EQ(config.flush_level, Level::warning);
  EXPECT_EQ(config.flush_interval, 250ms);
  ASSERT_TRUE(config.console);
  EXPECT_EQ(config.console->stream, tinylog::ConsoleStream::err);
  EXPECT_EQ(config.console->color, tinylog::ColorMode::never);
  EXPECT_EQ(config.console->layout.timestamp, tinylog::TimestampPrecision::micros);
  EXPECT_TRUE(config.console->layout.show_thread);

  ASSERT_EQ(config.files.size(), 2u);
  const auto &app = config.files[0];
  EXPECT_EQ(app.path, std::filesystem::path("logs/app.log"));
  EXPECT_EQ(app.min_level, Level::info);
  EXPECT_EQ(app.compression, Compression::zstd);
  EXPECT_EQ(app.compression_level, 5);
  EXPECT_EQ(app.frame_size, 1u << 20);
  EXPECT_EQ(app.rotation.max_size, 10u << 20);
  EXPECT_EQ(app.rotation.interval, std::chrono::hours(24));
  EXPECT_EQ(app.rotation.max_files, 7u);
  EXPECT_TRUE(app.rotation.on_open);
  // Layout keys given before file= are inherited.
  EXPECT_EQ(app.layout.timestamp, tinylog::TimestampPrecision::micros);

  const auto &errors = config.files[1];
  EXPECT_EQ(errors.min_level, Level::error);
  EXPECT_EQ(errors.layout.format, tinylog::Format::json);
  EXPECT_EQ(errors.rotation.compress, Compression::zstd);
  EXPECT_EQ(errors.rotation.compress_level, 12);
  EXPECT_EQ(errors.compression, Compression::none);
}

TEST(Config, ConsoleOff)
{
  EXPECT_FALSE(tinylog::parse_config("console=off").console.has_value());
}

TEST(Config, ErrorsNameTheKey)
{
  auto message_of = [](const char *text) -> std::string
  {
    try
    {
      tinylog::parse_config(text);
    }
    catch (const std::invalid_argument &e)
    {
      return e.what();
    }
    return "no exception";
  };
  EXPECT_NE(message_of("level=loud").find("'level'"), std::string::npos);
  EXPECT_NE(message_of("rotate.size=10M").find("file="), std::string::npos);
  EXPECT_NE(message_of("nonsense=1").find("unknown key 'nonsense'"), std::string::npos);
  EXPECT_NE(message_of("file=a.log;rotate.size=ten").find("'rotate.size'"), std::string::npos);
  EXPECT_NE(message_of("file=a.log;rotate.keep=-1").find("'rotate.keep'"), std::string::npos);
  EXPECT_NE(message_of("justakey").find("key=value"), std::string::npos);
}

TEST(Config, EnvironmentConfigAddsFileSink)
{
  test::TempDir dir;
  const auto path = dir / "env.log";
  const std::string settings = "console=off;timestamp=none;show_level=false;file=" + path.string();
  test::set_env("TINYLOG_CONFIG", settings.c_str());
  tinylog::Config config;
  tinylog::init(config);
  test::set_env("TINYLOG_CONFIG", nullptr);
  TLOG_INFO("from env");
  tinylog::shutdown();
  EXPECT_EQ(test::read_text(path), "from env\n");
}

TEST(Config, BadEnvironmentConfigFailsInit)
{
  test::set_env("TINYLOG_CONFIG", "level=nope");
  EXPECT_THROW(tinylog::init(), std::invalid_argument);
  test::set_env("TINYLOG_CONFIG", nullptr);
}
