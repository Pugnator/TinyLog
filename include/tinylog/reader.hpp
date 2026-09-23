#pragma once

/*! \file Reading logs back, including zstd-compressed and crash-truncated ones. */

#include <tinylog/export.hpp>

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

namespace tinylog
{
  struct ReadResult
  {
    //! The input ended inside a zstd frame (a live or crashed log); all complete blocks were delivered.
    bool truncated = false;
    //! Decoding stopped at corrupt data; `error` says why.
    bool corrupt = false;
    std::string error;
  };

  /**
   * @brief Streams the text of a log file to `on_text` in chunks.
   *
   * Plain files are passed through; `.zst` files (detected by content, not
   * name) are decompressed, including files of many appended frames and a
   * last frame that is still being written. Unlike `zstd -d`, a truncated
   * tail is not an error: everything decodable is delivered.
   *
   * @throws std::system_error if the file cannot be read.
   */
  TINYLOG_API ReadResult read_log(const std::filesystem::path &path,
                                  const std::function<void(std::string_view)> &on_text);

  //! Convenience overload returning the whole text.
  TINYLOG_API std::string read_log(const std::filesystem::path &path, ReadResult *result = nullptr);
}
