#pragma once

#include <tinylog/reader.hpp>
#include <tinylog/tinylog.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace test
{
  namespace fs = std::filesystem;

  //! A fresh directory under the system temp dir, removed afterwards.
  class TempDir
  {
  public:
    TempDir()
    {
      std::random_device rd;
      const auto name = "tinylog-test-" + std::to_string(rd()) + std::to_string(rd());
      path_ = fs::temp_directory_path() / name;
      fs::create_directories(path_);
    }
    ~TempDir()
    {
      tinylog::shutdown(); // release file handles before deleting (Windows)
      std::error_code ec;
      fs::remove_all(path_, ec);
    }
    const fs::path &path() const { return path_; }
    fs::path operator/(const fs::path &name) const { return path_ / name; }

  private:
    fs::path path_;
  };

  //! Records every message it receives.
  class Capture
  {
  public:
    struct Line
    {
      tinylog::Level level;
      std::string text;
      std::uint64_t thread;
    };

    std::shared_ptr<tinylog::Sink> sink()
    {
      return std::make_shared<tinylog::CallbackSink>([this](const tinylog::Record &r)
                                                     {
                                                       std::lock_guard<std::mutex> lock(mutex_);
                                                       lines_.push_back({r.level, std::string(r.message), r.thread_id}); });
    }
    std::vector<Line> lines()
    {
      std::lock_guard<std::mutex> lock(mutex_);
      return lines_;
    }
    std::vector<std::string> texts()
    {
      std::vector<std::string> out;
      for (auto &line : lines())
        out.push_back(line.text);
      return out;
    }

  private:
    std::mutex mutex_;
    std::vector<Line> lines_;
  };

  //! A config writing only to `capture`, ignoring the environment.
  inline tinylog::Config config_for(Capture &capture, tinylog::Mode mode = tinylog::Mode::sync)
  {
    tinylog::Config config;
    config.console.reset();
    config.use_env = false;
    config.level = tinylog::Level::trace;
    config.mode = mode;
    config.sinks.push_back(capture.sink());
    return config;
  }

  //! A config with one file sink and no console.
  inline tinylog::Config file_config(const fs::path &path)
  {
    tinylog::Config config;
    config.console.reset();
    config.use_env = false;
    config.level = tinylog::Level::trace;
    auto &file = config.files.emplace_back();
    file.path = path;
    file.layout.timestamp = tinylog::TimestampPrecision::none;
    file.layout.show_level = false;
    return config;
  }

  inline std::string read_text(const fs::path &path)
  {
    tinylog::ReadResult result;
    auto text = tinylog::read_log(path, &result);
    EXPECT_FALSE(result.corrupt) << path << ": " << result.error;
    return text;
  }

  inline std::vector<std::string> split_lines(const std::string &text)
  {
    std::vector<std::string> lines;
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);)
      lines.push_back(line);
    return lines;
  }

  //! Files in `dir` whose names start with `prefix`, sorted by name.
  inline std::vector<fs::path> files_with_prefix(const fs::path &dir, const std::string &prefix)
  {
    std::vector<fs::path> out;
    for (const auto &entry : fs::directory_iterator(dir))
    {
      if (entry.path().filename().string().rfind(prefix, 0) == 0)
        out.push_back(entry.path());
    }
    std::sort(out.begin(), out.end());
    return out;
  }

  inline void set_env(const char *name, const char *value)
  {
#if defined(_WIN32)
    _putenv_s(name, value == nullptr ? "" : value);
#else
    if (value == nullptr)
      unsetenv(name);
    else
      setenv(name, value, 1);
#endif
  }
}
