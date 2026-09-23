#include <tinylog/compat.hpp>
#include <tinylog/reader.hpp>
#include <tinylog/tinylog.hpp>

#include <cstdio>
#include <filesystem>

int main()
{
  const auto path = std::filesystem::temp_directory_path() / "tinylog-consumer.log";
  std::filesystem::remove(path);
  tinylog::Config config = tinylog::parse_config("console=off; timestamp=none; file=" + path.string());
  {
    tinylog::Guard guard(config);
    TLOG_INFO("consumer built against tinylog {}", tinylog::version());
    LOG_WARNING("legacy macro");
  }
  const auto text = tinylog::read_log(path);
  std::fputs(text.c_str(), stdout);
  return text.find("legacy macro") != std::string::npos ? 0 : 1;
}
