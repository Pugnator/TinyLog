// The pre-0.2 API must keep compiling and working for existing users.
#include <log.hpp>

#include "test_util.hpp"

TEST(Compat, FileBackendWithRotationConfig)
{
  test::TempDir dir;
  const auto path = (dir / "legacy.txt").string();
  // Designated initializers as used by existing code.
  RotationConfig rotation{.max_file_size = 5 * 1024 * 1024, .max_backup_count = 5, .compress = true};
  Log::get().configure(TraceType::file, path, rotation);
  Log::get().set_level(TraceSeverity::debug);

  LOG("plain {}\n", 1);
  LOG_DEBUG("debug {}\n", 2);
  LOG_WARNING("warning {}", 3);
  LOG_ERROR("error {}", 4);
  LOG_CALL("verbose is off"); // verbose was never enabled
  Log::get().log(TraceSeverity::critical, std::string("runtime format {}"), 5);
  try
  {
    throw std::runtime_error("boom");
  }
  catch (const std::exception &e)
  {
    LOG_EXCEPTION("while testing", e);
  }
  tinylog::shutdown();

  const auto lines = test::split_lines(test::read_text(path));
  ASSERT_EQ(lines.size(), 6u);
  EXPECT_NE(lines[0].find("[info] plain 1"), std::string::npos) << lines[0];
  EXPECT_NE(lines[1].find("[debug] debug 2"), std::string::npos) << lines[1];
  EXPECT_NE(lines[2].find("[warning] warning 3"), std::string::npos);
  EXPECT_NE(lines[3].find("[error] error 4"), std::string::npos);
  EXPECT_NE(lines[4].find("[critical] runtime format 5"), std::string::npos);
  EXPECT_NE(lines[5].find("while testing: boom at"), std::string::npos) << lines[5];
}

TEST(Compat, LevelsAreBits)
{
  test::Capture capture;
  auto config = test::config_for(capture);
  tinylog::init(config);
  Log::get().reset_levels();
  LOG_INFO("dropped");
  Log::get().set_level(TraceSeverity::error).set_level(TraceSeverity::debug);
  LOG_INFO("dropped too");
  LOG_DEBUG("debug");
  LOG_ERROR("error");
  Log::get().clear_level(TraceSeverity::debug);
  LOG_DEBUG("dropped again");
  EXPECT_EQ(capture.texts(), (std::vector<std::string>{"debug", "error"}));
}

TEST(Compat, ConfigureKeepsLevels)
{
  test::TempDir dir;
  Log::get().reset_levels();
  Log::get().set_level(TraceSeverity::warning);
  Log::get().configure(TraceType::devnull);
  EXPECT_TRUE(tinylog::should_log(tinylog::Level::warning));
  EXPECT_FALSE(tinylog::should_log(tinylog::Level::info));
  EXPECT_NO_THROW(LOG_WARNING("to nowhere"));
  Log::get().configure(TraceType::console);
}

TEST(Compat, InvalidPathThrows)
{
  test::TempDir dir;
  std::ofstream(dir / "blocker") << "x";
  EXPECT_THROW(Log::get().configure(TraceType::file, (dir / "blocker" / "x.log").string()), std::exception);
}
