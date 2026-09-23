#pragma once

/*! \file Tiny benchmark harness: per-call latency percentiles and end-to-end throughput. */

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace bench
{
  struct Scenario
  {
    std::string library;
    std::string name;
    int threads = 1;
    //! Called once before timing; configures the logger.
    std::function<void(const std::filesystem::path &dir)> setup;
    //! One log call; `i` is the iteration, `t` the thread index.
    std::function<void(int i, int t)> log;
    //! Called after the timed loop; must deliver everything (flush / join backend).
    std::function<void()> teardown;
  };

  struct Result
  {
    std::string library;
    std::string name;
    int threads = 0;
    std::uint64_t messages = 0;
    double p50_ns = 0, p99_ns = 0, p999_ns = 0, max_ns = 0;
    //! Messages per second including the final flush.
    double throughput = 0;
    std::uint64_t bytes_on_disk = 0;
  };

  std::vector<Scenario> &registry();

  struct Register
  {
    explicit Register(Scenario scenario) { registry().push_back(std::move(scenario)); }
  };

  inline std::uint64_t directory_size(const std::filesystem::path &dir)
  {
    std::uint64_t total = 0;
    std::error_code ec;
    for (const auto &entry : std::filesystem::recursive_directory_iterator(dir, ec))
    {
      if (entry.is_regular_file(ec))
        total += entry.file_size(ec);
    }
    return total;
  }

  inline Result run(const Scenario &scenario, int iterations, const std::filesystem::path &dir)
  {
    using clock = std::chrono::steady_clock;
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir);
    scenario.setup(dir);

    std::vector<std::vector<std::uint32_t>> latencies(static_cast<std::size_t>(scenario.threads));
    const auto start = clock::now();
    std::vector<std::thread> threads;
    for (int t = 0; t < scenario.threads; ++t)
    {
      threads.emplace_back([&, t]
                           {
                             auto &samples = latencies[static_cast<std::size_t>(t)];
                             samples.reserve(static_cast<std::size_t>(iterations));
                             for (int i = 0; i < iterations; ++i)
                             {
                               const auto before = clock::now();
                               scenario.log(i, t);
                               const auto after = clock::now();
                               const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(after - before).count();
                               samples.push_back(static_cast<std::uint32_t>(std::min<long long>(ns, UINT32_MAX)));
                             } });
    }
    for (auto &thread : threads)
      thread.join();
    scenario.teardown();
    const auto elapsed = std::chrono::duration<double>(clock::now() - start).count();

    std::vector<std::uint32_t> all;
    for (auto &samples : latencies)
      all.insert(all.end(), samples.begin(), samples.end());
    std::sort(all.begin(), all.end());
    auto percentile = [&](double p)
    {
      if (all.empty())
        return 0.0;
      const auto index = static_cast<std::size_t>(p * static_cast<double>(all.size() - 1));
      return static_cast<double>(all[index]);
    };

    Result result;
    result.library = scenario.library;
    result.name = scenario.name;
    result.threads = scenario.threads;
    result.messages = all.size();
    result.p50_ns = percentile(0.50);
    result.p99_ns = percentile(0.99);
    result.p999_ns = percentile(0.999);
    result.max_ns = all.empty() ? 0.0 : all.back();
    result.throughput = elapsed > 0 ? static_cast<double>(all.size()) / elapsed : 0.0;
    result.bytes_on_disk = directory_size(dir);
    std::filesystem::remove_all(dir, ec);
    return result;
  }
}
