#include "test_util.hpp"

using tinylog::Layout;
using tinylog::Level;
using tinylog::Record;

namespace
{
  // 2026-09-23 10:15:02.123456 UTC
  Record sample(std::string_view message, Level level = Level::warning)
  {
    using namespace std::chrono;
    Record record;
    record.level = level;
    record.time = system_clock::time_point(seconds(1790158502) + microseconds(123456));
    record.thread_id = 4242;
    record.where = {"/src/app/main.cc", "main", 17};
    record.message = message;
    return record;
  }

  std::string render(const Record &record, const Layout &layout)
  {
    std::string out;
    tinylog::format_record(record, layout, out);
    return out;
  }
}

TEST(Format, DefaultTextLayoutUtc)
{
  Layout layout;
  layout.utc = true;
  EXPECT_EQ(render(sample("hello"), layout), "[2026-09-23 10:15:02.123] [warning] hello\n");
}

TEST(Format, Precisions)
{
  Layout layout;
  layout.utc = true;
  layout.show_level = false;
  layout.timestamp = tinylog::TimestampPrecision::seconds;
  EXPECT_EQ(render(sample("x"), layout), "[2026-09-23 10:15:02] x\n");
  layout.timestamp = tinylog::TimestampPrecision::micros;
  EXPECT_EQ(render(sample("x"), layout), "[2026-09-23 10:15:02.123456] x\n");
  layout.timestamp = tinylog::TimestampPrecision::none;
  EXPECT_EQ(render(sample("x"), layout), "x\n");
}

TEST(Format, ThreadAndSource)
{
  Layout layout;
  layout.timestamp = tinylog::TimestampPrecision::none;
  layout.show_thread = true;
  layout.show_source = true;
  EXPECT_EQ(render(sample("m", Level::info), layout), "[info] [4242] [main.cc:17] m\n");
}

TEST(Format, TrailingNewlineIsNotDoubled)
{
  Layout layout;
  layout.timestamp = tinylog::TimestampPrecision::none;
  layout.show_level = false;
  EXPECT_EQ(render(sample("line\n"), layout), "line\n");
  EXPECT_EQ(render(sample("line\r\n"), layout), "line\n");
  EXPECT_EQ(render(sample(""), layout), "\n");
}

TEST(Format, LocalTimeMatchesLocaltime)
{
  Layout layout;
  layout.show_level = false;
  layout.timestamp = tinylog::TimestampPrecision::seconds;
  const auto record = sample("x");
  const std::time_t t = std::chrono::system_clock::to_time_t(record.time);
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  char expected[64];
  std::strftime(expected, sizeof(expected), "[%Y-%m-%d %H:%M:%S] x\n", &tm);
  EXPECT_EQ(render(record, layout), expected);
}

TEST(Format, JsonLines)
{
  Layout layout;
  layout.format = tinylog::Format::json;
  EXPECT_EQ(render(sample("say \"hi\"\\\n\t\x01"), layout),
            "{\"ts\":\"2026-09-23T10:15:02.123Z\",\"level\":\"warning\",\"thread\":4242,"
            "\"file\":\"main.cc\",\"line\":17,\"func\":\"main\","
            "\"msg\":\"say \\\"hi\\\"\\\\\\n\\t\\u0001\"}\n");
}

TEST(Format, Utf8PassesThrough)
{
  Layout layout;
  layout.timestamp = tinylog::TimestampPrecision::none;
  layout.show_level = false;
  const std::string text = "\xE6\x97\xA5\xE6\x9C\xAC \xD0\xB6\xD1\x83\xD1\x80\xD0\xBD\xD0\xB0\xD0\xBB";
  EXPECT_EQ(render(sample(text), layout), text + "\n");
}
