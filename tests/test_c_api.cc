#include <tinylog/tinylog.h>

#include "test_util.hpp"

#include <cstring>

TEST(CApi, InitWriteShutdown)
{
  test::TempDir dir;
  const std::string settings = "console=off; level=debug; timestamp=none; file=" + (dir / "c.log").string();
  ASSERT_EQ(tinylog_init(settings.c_str()), 0) << tinylog_last_error();
  EXPECT_EQ(tinylog_should_log(1), 1);
  EXPECT_EQ(tinylog_should_log(0), 0);
  const char *message = "hello from C";
  tinylog_write(2, __FILE__, __LINE__, "test", message, std::strlen(message));
  tinylog_set_level(4);
  tinylog_write(3, nullptr, 0, nullptr, "dropped", 7);
  tinylog_flush();
  tinylog_shutdown();
  EXPECT_EQ(test::read_text(dir / "c.log"), "[info] hello from C\n");
}

TEST(CApi, ErrorsAreReported)
{
  EXPECT_EQ(tinylog_init("level=nope"), -1);
  EXPECT_NE(std::string(tinylog_last_error()).find("level"), std::string::npos);
  EXPECT_STREQ(tinylog_version(), TINYLOG_VERSION_STRING);
}
