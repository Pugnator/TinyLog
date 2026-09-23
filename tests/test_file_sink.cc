#include "test_util.hpp"

#include <set>
#include <thread>

using tinylog::Compression;
using tinylog::Level;

namespace fs = std::filesystem;

namespace
{
  //! Logs "line <i> <padding>" for i in [first, first + count).
  void log_lines(int first, int count, std::size_t padding = 40)
  {
    const std::string pad(padding, 'x');
    for (int i = first; i < first + count; ++i)
      TLOG_INFO("line {} {}", i, pad);
  }

  //! Line numbers found in all files starting with `prefix`.
  std::multiset<int> numbers_in(const fs::path &dir, const std::string &prefix)
  {
    std::multiset<int> numbers;
    for (const auto &path : test::files_with_prefix(dir, prefix))
    {
      for (const auto &line : test::split_lines(test::read_text(path)))
      {
        std::istringstream in(line);
        std::string word;
        int n = -1;
        in >> word >> n;
        if (word == "line")
          numbers.insert(n);
      }
    }
    return numbers;
  }

  std::multiset<int> range(int first, int count)
  {
    std::multiset<int> out;
    for (int i = first; i < first + count; ++i)
      out.insert(i);
    return out;
  }

  bool has_zstd_magic(const fs::path &path)
  {
    std::ifstream in(path, std::ios::binary);
    unsigned char magic[4] = {};
    in.read(reinterpret_cast<char *>(magic), 4);
    return magic[0] == 0x28 && magic[1] == 0xB5 && magic[2] == 0x2F && magic[3] == 0xFD;
  }
}

TEST(FileSink, WritesAndAppendsAcrossRestarts)
{
  test::TempDir dir;
  const auto path = dir / "app.log";
  tinylog::init(test::file_config(path));
  TLOG_INFO("first");
  TLOG_WARN("second\n");
  tinylog::shutdown();
  tinylog::init(test::file_config(path));
  TLOG_INFO("third");
  tinylog::shutdown();
  EXPECT_EQ(test::split_lines(test::read_text(path)), (std::vector<std::string>{"first", "second", "third"}));
}

TEST(FileSink, TruncateStartsEmpty)
{
  test::TempDir dir;
  const auto path = dir / "app.log";
  tinylog::init(test::file_config(path));
  TLOG_INFO("old");
  tinylog::shutdown();
  auto config = test::file_config(path);
  config.files[0].truncate = true;
  tinylog::init(config);
  TLOG_INFO("new");
  tinylog::shutdown();
  EXPECT_EQ(test::split_lines(test::read_text(path)), std::vector<std::string>{"new"});
}

TEST(FileSink, CreatesMissingDirectories)
{
  test::TempDir dir;
  const auto path = dir / "a" / "b" / "app.log";
  tinylog::init(test::file_config(path));
  TLOG_INFO("deep");
  tinylog::shutdown();
  EXPECT_EQ(test::read_text(path), "deep\n");
}

TEST(FileSink, UnicodePath)
{
  test::TempDir dir;
  const auto path = dir / fs::path(u8"журнал") / fs::path(u8"日本.log");
  tinylog::init(test::file_config(path));
  TLOG_INFO("unicode");
  tinylog::shutdown();
  EXPECT_TRUE(fs::exists(path));
  EXPECT_EQ(test::read_text(path), "unicode\n");
}

TEST(FileSink, OpenFailureThrowsAndKeepsLogging)
{
  test::TempDir dir;
  std::ofstream(dir / "blocker", std::ios::binary) << "a file, not a directory";
  EXPECT_THROW(tinylog::init(test::file_config(dir / "blocker" / "app.log")), std::system_error);
  EXPECT_NO_THROW(TLOG_INFO("falls back to the console"));
}

TEST(FileSink, SizeRotationKeepsEveryLine)
{
  test::TempDir dir;
  auto config = test::file_config(dir / "app.log");
  config.files[0].rotation.max_size = 1000;
  config.files[0].rotation.max_files = 0; // keep all
  tinylog::init(config);
  log_lines(0, 300);
  tinylog::shutdown();

  const auto files = test::files_with_prefix(dir.path(), "app.");
  EXPECT_GT(files.size(), 10u);
  for (const auto &file : files)
    EXPECT_LE(fs::file_size(file), 1000u) << file;
  EXPECT_EQ(numbers_in(dir.path(), "app."), range(0, 300));
  EXPECT_EQ(tinylog::stats().rotations, files.size() - 1);
}

TEST(FileSink, RetentionKeepsNewestFiles)
{
  test::TempDir dir;
  auto config = test::file_config(dir / "app.log");
  config.files[0].rotation.max_size = 1000;
  config.files[0].rotation.max_files = 3;
  tinylog::init(config);
  log_lines(0, 300);
  tinylog::shutdown();

  const auto files = test::files_with_prefix(dir.path(), "app.");
  EXPECT_EQ(files.size(), 4u); // active + 3 rotated
  const auto numbers = numbers_in(dir.path(), "app.");
  ASSERT_FALSE(numbers.empty());
  EXPECT_EQ(*numbers.rbegin(), 299);
  // What is kept is a contiguous run ending with the newest line.
  EXPECT_EQ(numbers, range(*numbers.begin(), static_cast<int>(numbers.size())));
}

TEST(FileSink, RotatedFilesAreCompressedInBackground)
{
  test::TempDir dir;
  auto config = test::file_config(dir / "app.log");
  config.files[0].rotation.max_size = 4000;
  config.files[0].rotation.max_files = 0;
  config.files[0].rotation.compress = Compression::zstd;
  config.files[0].rotation.compress_level = 3;
  tinylog::init(config);
  log_lines(0, 500);
  tinylog::wait_idle();

  const auto files = test::files_with_prefix(dir.path(), "app.");
  int compressed = 0;
  for (const auto &file : files)
  {
    if (file.filename() == "app.log")
      continue;
    EXPECT_EQ(file.extension(), ".zst") << file;
    EXPECT_TRUE(has_zstd_magic(file)) << file;
    ++compressed;
  }
  EXPECT_GT(compressed, 3);
  tinylog::shutdown();
  EXPECT_EQ(numbers_in(dir.path(), "app."), range(0, 500));
}

TEST(FileSink, CompressionWithRetention)
{
  test::TempDir dir;
  auto config = test::file_config(dir / "app.log");
  config.files[0].rotation.max_size = 2000;
  config.files[0].rotation.max_files = 2;
  config.files[0].rotation.compress = Compression::zstd;
  tinylog::init(config);
  log_lines(0, 400);
  tinylog::wait_idle();
  tinylog::shutdown();
  const auto files = test::files_with_prefix(dir.path(), "app.");
  EXPECT_EQ(files.size(), 3u);
  const auto numbers = numbers_in(dir.path(), "app.");
  EXPECT_EQ(*numbers.rbegin(), 399);
  EXPECT_EQ(numbers, range(*numbers.begin(), static_cast<int>(numbers.size())));
}

TEST(FileSink, LiveCompressionRoundTrip)
{
  test::TempDir dir;
  auto config = test::file_config(dir / "app.log");
  config.files[0].compression = Compression::zstd;
  tinylog::init(config);
  log_lines(0, 2000);
  tinylog::shutdown();

  const auto path = dir / "app.log.zst";
  ASSERT_TRUE(fs::exists(path));
  EXPECT_TRUE(has_zstd_magic(path));
  tinylog::ReadResult result;
  const auto text = tinylog::read_log(path, &result);
  EXPECT_FALSE(result.truncated);
  EXPECT_FALSE(result.corrupt);
  EXPECT_EQ(numbers_in(dir.path(), "app."), range(0, 2000));
  // Repetitive log text compresses well.
  EXPECT_LT(fs::file_size(path) * 5, text.size());
}

TEST(FileSink, LiveCompressionAppendsFramesAcrossRestarts)
{
  test::TempDir dir;
  auto config = test::file_config(dir / "app.log.zst");
  config.files[0].compression = Compression::zstd;
  config.mode = tinylog::Mode::async;
  for (int run = 0; run < 3; ++run)
  {
    tinylog::init(config);
    log_lines(run * 100, 100);
    tinylog::shutdown();
  }
  tinylog::ReadResult result;
  tinylog::read_log(dir / "app.log.zst", &result);
  EXPECT_FALSE(result.truncated);
  EXPECT_EQ(numbers_in(dir.path(), "app."), range(0, 300));
}

TEST(FileSink, LiveCompressionSmallFramesStayDecodable)
{
  test::TempDir dir;
  auto config = test::file_config(dir / "app.log");
  config.files[0].compression = Compression::zstd;
  config.files[0].frame_size = 1024; // many frames
  config.files[0].buffer_size = 100;
  tinylog::init(config);
  log_lines(0, 1000);
  tinylog::shutdown();
  EXPECT_EQ(numbers_in(dir.path(), "app."), range(0, 1000));
}

TEST(FileSink, RecoversFromCrashInsideAFrame)
{
  test::TempDir dir;
  auto config = test::file_config(dir / "live.log");
  config.files[0].compression = Compression::zstd;
  config.files[0].compressed_flush_interval = std::chrono::milliseconds(0); // every record reaches the file
  tinylog::init(config);
  log_lines(0, 100);
  tinylog::flush();

  // Snapshot the file while its frame is still open, as a crash would leave it,
  // and cut a few bytes off the end: a half-written block.
  const auto crashed = dir / "app.log.zst";
  fs::copy_file(dir / "live.log.zst", crashed);
  fs::resize_file(crashed, fs::file_size(crashed) - 3);
  tinylog::shutdown();
  {
    tinylog::ReadResult result;
    tinylog::read_log(crashed, &result);
    EXPECT_TRUE(result.truncated);
  }

  auto reopen = test::file_config(dir / "app.log");
  reopen.files[0].compression = Compression::zstd;
  tinylog::init(reopen);
  TLOG_INFO("line 1000 after restart");
  tinylog::shutdown();

  tinylog::ReadResult result;
  const auto lines = test::split_lines(tinylog::read_log(crashed, &result));
  EXPECT_FALSE(result.truncated) << result.error;
  EXPECT_FALSE(result.corrupt) << result.error;
  const auto numbers = numbers_in(dir.path(), "app.");
  EXPECT_GE(numbers.size(), 100u); // at most the last, partly written record is lost
  EXPECT_EQ(*numbers.rbegin(), 1000);
  for (int i = 0; i < 99; ++i)
    EXPECT_EQ(numbers.count(i), 1u) << i;
}

TEST(FileSink, ForeignDataIsMovedAsideNotDestroyed)
{
  test::TempDir dir;
  std::ofstream(dir / "app.log.zst", std::ios::binary) << "plain text that is not zstd\n";
  auto config = test::file_config(dir / "app.log");
  config.files[0].compression = Compression::zstd;
  tinylog::init(config);
  TLOG_INFO("fresh");
  tinylog::shutdown();
  EXPECT_EQ(test::read_text(dir / "app.log.zst"), "fresh\n");
  bool found = false;
  for (const auto &file : test::files_with_prefix(dir.path(), "app.log.zst."))
  {
    if (file.extension() == ".corrupt")
    {
      found = true;
      std::ifstream in(file);
      std::string line;
      std::getline(in, line);
      EXPECT_EQ(line, "plain text that is not zstd");
    }
  }
  EXPECT_TRUE(found);
}

TEST(FileSink, CompressedLiveLogRotates)
{
  test::TempDir dir;
  auto config = test::file_config(dir / "app.log");
  config.files[0].compression = Compression::zstd;
  config.files[0].rotation.max_size = 2000;
  config.files[0].rotation.max_files = 0;
  config.files[0].buffer_size = 256;
  config.files[0].frame_size = 16 * 1024;
  tinylog::init(config);
  // Varied content so it does not compress to nothing.
  for (int i = 0; i < 3000; ++i)
    TLOG_INFO("line {} {:x} {}", i, static_cast<unsigned>(i) * 2654435761u, i * 7919);
  tinylog::shutdown();
  const auto files = test::files_with_prefix(dir.path(), "app.");
  EXPECT_GT(files.size(), 2u);
  for (const auto &file : files)
  {
    EXPECT_EQ(file.extension(), ".zst") << file;
    tinylog::ReadResult result;
    tinylog::read_log(file, &result);
    EXPECT_FALSE(result.truncated) << file;
  }
  EXPECT_EQ(numbers_in(dir.path(), "app."), range(0, 3000));
}

TEST(FileSink, TimeRotationOnIntervalBoundaries)
{
  test::TempDir dir;
  tinylog::FileSinkConfig config;
  config.path = dir / "app.log";
  config.layout.timestamp = tinylog::TimestampPrecision::none;
  config.layout.show_level = false;
  config.rotation.interval = std::chrono::hours(1);
  config.rotation.max_files = 0;
  config.layout.utc = true;
  auto sink = tinylog::make_file_sink(config);

  using namespace std::chrono;
  const auto now = system_clock::now();
  const auto hour_start = time_point_cast<hours>(now);
  auto record = [](system_clock::time_point t, std::string_view text)
  {
    tinylog::Record r;
    r.time = t;
    r.message = text;
    return r;
  };
  // Strictly inside this hour, then the next hour, then the one after.
  sink->write(record(std::max<system_clock::time_point>(now, hour_start + minutes(1)), "a"));
  sink->write(record(hour_start + hours(1) + seconds(5), "b"));
  sink->write(record(hour_start + hours(2) + seconds(5), "c"));
  sink->flush();
  sink.reset();

  const auto files = test::files_with_prefix(dir.path(), "app.");
  ASSERT_EQ(files.size(), 3u);
  EXPECT_EQ(test::read_text(dir / "app.log"), "c\n");
  std::string rotated;
  for (const auto &file : files)
  {
    if (file.filename() != "app.log")
      rotated += test::read_text(file);
  }
  EXPECT_EQ(rotated, "a\nb\n");
}

TEST(FileSink, RotateOnOpen)
{
  test::TempDir dir;
  std::ofstream(dir / "app.log", std::ios::binary) << "previous run\n";
  auto config = test::file_config(dir / "app.log");
  config.files[0].rotation.on_open = true;
  tinylog::init(config);
  TLOG_INFO("this run");
  tinylog::shutdown();
  EXPECT_EQ(test::read_text(dir / "app.log"), "this run\n");
  EXPECT_EQ(test::files_with_prefix(dir.path(), "app.").size(), 2u);
}

TEST(FileSink, LeftoversFromAnInterruptedRunAreCompressed)
{
  test::TempDir dir;
  std::ofstream(dir / "app.20200101-000000.log", std::ios::binary) << "line 1 old rotated file\n";
  std::ofstream(dir / "app.20200101-000001.log.zst.tmp", std::ios::binary) << "half-written";
  auto config = test::file_config(dir / "app.log");
  config.files[0].rotation.compress = Compression::zstd;
  config.files[0].rotation.max_files = 0;
  tinylog::init(config);
  tinylog::wait_idle();
  tinylog::shutdown();
  EXPECT_FALSE(fs::exists(dir / "app.20200101-000000.log"));
  EXPECT_FALSE(fs::exists(dir / "app.20200101-000001.log.zst.tmp"));
  ASSERT_TRUE(fs::exists(dir / "app.20200101-000000.log.zst"));
  EXPECT_EQ(test::read_text(dir / "app.20200101-000000.log.zst"), "line 1 old rotated file\n");
}

TEST(FileSink, JsonFormat)
{
  test::TempDir dir;
  auto config = test::file_config(dir / "app.jsonl");
  config.files[0].layout.format = tinylog::Format::json;
  tinylog::init(config);
  TLOG_ERROR("quote \" here");
  tinylog::shutdown();
  const auto lines = test::split_lines(test::read_text(dir / "app.jsonl"));
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(lines[0].rfind("{\"ts\":\"", 0), 0u);
  EXPECT_NE(lines[0].find("\"level\":\"error\""), std::string::npos);
  EXPECT_NE(lines[0].find("\"msg\":\"quote \\\" here\"}"), std::string::npos);
}

TEST(FileSink, AsyncFileUnderLoad)
{
  test::TempDir dir;
  auto config = test::file_config(dir / "app.log");
  config.mode = tinylog::Mode::async;
  config.flush_level = Level::error;
  config.files[0].rotation.max_size = 64 * 1024;
  config.files[0].rotation.max_files = 0;
  tinylog::init(config);
  std::vector<std::thread> threads;
  for (int t = 0; t < 4; ++t)
    threads.emplace_back([t]
                         { log_lines(t * 5000, 5000); });
  for (auto &thread : threads)
    thread.join();
  tinylog::shutdown();
  EXPECT_EQ(numbers_in(dir.path(), "app."), range(0, 20000));
}
