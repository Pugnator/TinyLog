// tinylog_bench: latency (p50/p99/p99.9/max per call) and throughput of logging scenarios.
//
//   tinylog_bench [--quick] [--iterations N] [--filter TEXT] [--json FILE] [--markdown FILE]

#include "harness.hpp"

#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>

namespace bench
{
  std::vector<Scenario> &registry()
  {
    static std::vector<Scenario> scenarios;
    return scenarios;
  }
}

namespace
{
  std::string format_rate(double per_second)
  {
    char buffer[32];
    if (per_second >= 1e6)
      std::snprintf(buffer, sizeof(buffer), "%.2f M/s", per_second / 1e6);
    else
      std::snprintf(buffer, sizeof(buffer), "%.0f K/s", per_second / 1e3);
    return buffer;
  }

  std::string format_bytes(std::uint64_t bytes)
  {
    char buffer[32];
    if (bytes >= (1u << 20))
      std::snprintf(buffer, sizeof(buffer), "%.1f MiB", static_cast<double>(bytes) / (1u << 20));
    else
      std::snprintf(buffer, sizeof(buffer), "%.1f KiB", static_cast<double>(bytes) / 1024.0);
    return buffer;
  }

  std::string markdown(const std::vector<bench::Result> &results)
  {
    std::ostringstream out;
    out << "| library | scenario | threads | p50 ns | p99 ns | p99.9 ns | max ns | throughput | on disk |\n"
        << "|---|---|---:|---:|---:|---:|---:|---:|---:|\n";
    for (const auto &r : results)
    {
      out << "| " << r.library << " | " << r.name << " | " << r.threads << " | " << r.p50_ns << " | " << r.p99_ns
          << " | " << r.p999_ns << " | " << r.max_ns << " | " << format_rate(r.throughput) << " | "
          << format_bytes(r.bytes_on_disk) << " |\n";
    }
    return out.str();
  }

  std::string json(const std::vector<bench::Result> &results)
  {
    std::ostringstream out;
    out << "[\n";
    for (std::size_t i = 0; i < results.size(); ++i)
    {
      const auto &r = results[i];
      out << "  {\"library\":\"" << r.library << "\",\"scenario\":\"" << r.name << "\",\"threads\":" << r.threads
          << ",\"messages\":" << r.messages << ",\"p50_ns\":" << r.p50_ns << ",\"p99_ns\":" << r.p99_ns
          << ",\"p999_ns\":" << r.p999_ns << ",\"max_ns\":" << r.max_ns << ",\"msgs_per_sec\":" << r.throughput
          << ",\"bytes_on_disk\":" << r.bytes_on_disk << "}" << (i + 1 < results.size() ? "," : "") << "\n";
    }
    out << "]\n";
    return out.str();
  }
}

int main(int argc, char **argv)
{
  int iterations = 200000;
  std::string filter, json_path, markdown_path;
  for (int i = 1; i < argc; ++i)
  {
    const std::string arg = argv[i];
    if (arg == "--quick")
      iterations = 5000;
    else if (arg == "--iterations" && i + 1 < argc)
      iterations = std::atoi(argv[++i]);
    else if (arg == "--filter" && i + 1 < argc)
      filter = argv[++i];
    else if (arg == "--json" && i + 1 < argc)
      json_path = argv[++i];
    else if (arg == "--markdown" && i + 1 < argc)
      markdown_path = argv[++i];
    else
    {
      std::cerr << "usage: tinylog_bench [--quick] [--iterations N] [--filter TEXT] [--json FILE] [--markdown FILE]\n";
      return 2;
    }
  }

  const auto dir = std::filesystem::temp_directory_path() / "tinylog-bench";
  std::vector<bench::Result> results;
  for (const auto &scenario : bench::registry())
  {
    const auto label = scenario.library + " " + scenario.name;
    if (!filter.empty() && label.find(filter) == std::string::npos)
      continue;
    std::cerr << "running " << label << " x" << scenario.threads << " threads..." << std::endl;
    results.push_back(bench::run(scenario, iterations, dir));
  }

  const auto table = markdown(results);
  std::cout << "\n" << iterations << " messages per thread\n\n" << table;
  if (!markdown_path.empty())
    std::ofstream(markdown_path) << table;
  if (!json_path.empty())
    std::ofstream(json_path) << json(results);
  return 0;
}
