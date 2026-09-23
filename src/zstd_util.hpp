#pragma once

/*! \file zstd helpers: appendable frame writer, crash recovery, file compression. Internal. */

#include "platform.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

struct ZSTD_CCtx_s;

namespace tinylog::detail::zstd
{
  /**
   * @brief Compresses a stream of log text into self-contained zstd frames.
   *
   * flush() makes all input so far decodable without ending the frame
   * (a block boundary). A frame is ended once `frame_size` input bytes went
   * into it, and by end_frame(); a later write starts a new frame. A file of
   * such frames decodes with `zstd -d` to the concatenated text.
   */
  class FrameWriter
  {
  public:
    FrameWriter(int level, std::size_t frame_size);
    ~FrameWriter();
    FrameWriter(const FrameWriter &) = delete;
    FrameWriter &operator=(const FrameWriter &) = delete;

    /**
     * @brief Compresses `data`, appending compressed bytes to `out`.
     * @return Offset in `out` where the open (or next) frame begins if that
     *         changed during this call, else std::string::npos.
     */
    std::size_t write(std::string_view data, std::string &out);
    //! Emits everything buffered so far; the frame stays open.
    void flush(std::string &out);
    //! Ends the current frame (no-op when none is open).
    void end_frame(std::string &out);
    //! Abandons the current frame after a write error; the next write starts a new one.
    void reset() noexcept;

  private:
    void drive(std::string_view data, int directive, std::string &out);

    ZSTD_CCtx_s *cctx_ = nullptr;
    std::size_t frame_size_;
    std::size_t frame_input_ = 0;
    bool frame_open_ = false;
    std::vector<char> chunk_;
  };

  enum class ScanStatus
  {
    complete,  //!< Only whole frames.
    truncated, //!< The last frame is incomplete (e.g. the writer crashed).
    foreign,   //!< Data that is not zstd follows `valid_end`.
  };

  struct ScanResult
  {
    ScanStatus status = ScanStatus::complete;
    //! End of the last complete frame.
    std::uint64_t valid_end = 0;
  };

  //! Walks frame and block headers (no decompression) to find where valid data ends.
  ScanResult scan_frames(platform::File &file, std::uint64_t size);

  //! Decompresses what can be decoded of the frame starting at `offset` (the damaged tail).
  std::string salvage(platform::File &file, std::uint64_t offset, std::uint64_t size);

  /**
   * @brief Compresses `source` into `target` via a temporary file and an atomic rename.
   *
   * The source is removed only after the target is complete. Returns false if
   * `stop` was raised (the temporary file is removed); throws on I/O errors.
   */
  bool compress_file(const std::filesystem::path &source, const std::filesystem::path &target, int level,
                     const std::atomic<bool> &stop);

  /**
   * @brief Streams the decompressed content of `data` (concatenated frames) to `sink`.
   * @return false if the input ends inside a frame or is corrupt (what decoded is still delivered).
   */
  bool decompress(std::function<std::size_t(char *, std::size_t)> read,
                  const std::function<void(std::string_view)> &sink, std::string *error);
}
