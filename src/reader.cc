#include <tinylog/reader.hpp>

#include "platform.hpp"
#include "zstd_util.hpp"

#include <vector>

namespace tinylog
{
  ReadResult read_log(const std::filesystem::path &path, const std::function<void(std::string_view)> &on_text)
  {
    platform::File file;
    file.open(path, platform::File::Mode::read);
    const std::uint64_t size = file.size();

    unsigned char magic[4] = {};
    const bool compressed = size >= 4 && file.read_at(0, magic, 4) == 4 &&
                            ((magic[0] == 0x28 && magic[1] == 0xB5 && magic[2] == 0x2F && magic[3] == 0xFD) ||
                             ((magic[0] & 0xF0) == 0x50 && magic[1] == 0x2A && magic[2] == 0x4D && magic[3] == 0x18));

    std::uint64_t position = 0;
    auto read = [&](char *buffer, std::size_t capacity) -> std::size_t
    {
      const auto got = file.read_at(position, buffer, capacity);
      position += got;
      return got;
    };

    ReadResult result;
    if (!compressed)
    {
      std::vector<char> buffer(1u << 16);
      while (const auto got = read(buffer.data(), buffer.size()))
        on_text(std::string_view(buffer.data(), got));
      return result;
    }
    std::string error;
    if (!detail::zstd::decompress(read, on_text, &error))
    {
      if (error.rfind("truncated", 0) == 0)
        result.truncated = true;
      else
        result.corrupt = true;
      result.error = error;
    }
    return result;
  }

  std::string read_log(const std::filesystem::path &path, ReadResult *result)
  {
    std::string text;
    auto outcome = read_log(path, [&](std::string_view chunk)
                            { text.append(chunk); });
    if (result != nullptr)
      *result = std::move(outcome);
    return text;
  }
}
