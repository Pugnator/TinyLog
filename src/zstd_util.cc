#include "zstd_util.hpp"

#include <zstd.h>

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace tinylog::detail::zstd
{
  namespace
  {
    constexpr std::uint32_t frame_magic = 0xFD2FB528u;
    constexpr std::uint32_t skippable_magic = 0x184D2A50u;
    constexpr std::uint32_t skippable_mask = 0xFFFFFFF0u;

    void check(std::size_t code, const char *what)
    {
      if (ZSTD_isError(code))
        throw std::runtime_error(std::string(what) + ": " + ZSTD_getErrorName(code));
    }

    std::uint32_t read_le32(const unsigned char *p)
    {
      return static_cast<std::uint32_t>(p[0]) | static_cast<std::uint32_t>(p[1]) << 8 |
             static_cast<std::uint32_t>(p[2]) << 16 | static_cast<std::uint32_t>(p[3]) << 24;
    }

    //! Random access over a file through a sliding window, for header walking.
    class WindowReader
    {
    public:
      WindowReader(platform::File &file, std::uint64_t size) : file_(file), size_(size) {}

      //! Pointer to `count` bytes at `offset`, or nullptr past the end of the file.
      const unsigned char *at(std::uint64_t offset, std::size_t count)
      {
        if (offset + count > size_)
          return nullptr;
        if (offset < start_ || offset + count > start_ + data_.size())
        {
          const auto length = static_cast<std::size_t>(std::min<std::uint64_t>(window, size_ - offset));
          data_.resize(length);
          std::size_t got = 0;
          while (got < length)
          {
            const auto n = file_.read_at(offset + got, data_.data() + got, length - got);
            if (n == 0)
              break;
            got += n;
          }
          data_.resize(got);
          start_ = offset;
          if (got < count)
            return nullptr;
        }
        return data_.data() + (offset - start_);
      }

    private:
      static constexpr std::size_t window = 1u << 20;
      platform::File &file_;
      std::uint64_t size_;
      std::uint64_t start_ = 0;
      std::vector<unsigned char> data_;
    };
  }

  // ---------------------------------------------------------------------------
  // FrameWriter
  // ---------------------------------------------------------------------------

  FrameWriter::FrameWriter(int level, std::size_t frame_size)
      : cctx_(ZSTD_createCCtx()), frame_size_(frame_size == 0 ? (4u << 20) : frame_size),
        chunk_(ZSTD_CStreamOutSize())
  {
    if (cctx_ == nullptr)
      throw std::bad_alloc();
    level = std::clamp(level, ZSTD_minCLevel(), ZSTD_maxCLevel());
    check(ZSTD_CCtx_setParameter(cctx_, ZSTD_c_compressionLevel, level), "zstd level");
    check(ZSTD_CCtx_setParameter(cctx_, ZSTD_c_checksumFlag, 1), "zstd checksum");
  }

  FrameWriter::~FrameWriter()
  {
    ZSTD_freeCCtx(cctx_);
  }

  void FrameWriter::drive(std::string_view data, int directive, std::string &out)
  {
    const auto mode = static_cast<ZSTD_EndDirective>(directive);
    ZSTD_inBuffer in{data.data(), data.size(), 0};
    for (;;)
    {
      ZSTD_outBuffer output{chunk_.data(), chunk_.size(), 0};
      const std::size_t remaining = ZSTD_compressStream2(cctx_, &output, &in, mode);
      check(remaining, "zstd compression");
      out.append(chunk_.data(), output.pos);
      const bool input_done = in.pos == in.size;
      if (mode == ZSTD_e_continue ? input_done : (input_done && remaining == 0))
        break;
    }
  }

  std::size_t FrameWriter::write(std::string_view data, std::string &out)
  {
    std::size_t start = std::string::npos;
    while (!data.empty())
    {
      if (!frame_open_)
        start = out.size();
      // Split so a frame ends close to frame_size_ input bytes.
      const std::size_t room = frame_size_ - frame_input_;
      const auto part = data.substr(0, std::min(room, data.size()));
      drive(part, ZSTD_e_continue, out);
      frame_open_ = true;
      frame_input_ += part.size();
      data.remove_prefix(part.size());
      if (frame_input_ >= frame_size_)
      {
        end_frame(out);
        start = out.size();
      }
    }
    return start;
  }

  void FrameWriter::flush(std::string &out)
  {
    if (frame_open_)
      drive({}, ZSTD_e_flush, out);
  }

  void FrameWriter::end_frame(std::string &out)
  {
    if (!frame_open_)
      return;
    drive({}, ZSTD_e_end, out);
    frame_open_ = false;
    frame_input_ = 0;
  }

  void FrameWriter::reset() noexcept
  {
    ZSTD_CCtx_reset(cctx_, ZSTD_reset_session_only);
    frame_open_ = false;
    frame_input_ = 0;
  }

  // ---------------------------------------------------------------------------
  // Recovery
  // ---------------------------------------------------------------------------

  ScanResult scan_frames(platform::File &file, std::uint64_t size)
  {
    WindowReader reader(file, size);
    std::uint64_t offset = 0;
    while (offset < size)
    {
      const unsigned char *magic_bytes = reader.at(offset, 4);
      if (magic_bytes == nullptr)
        return {ScanStatus::truncated, offset};
      const std::uint32_t magic = read_le32(magic_bytes);

      if ((magic & skippable_mask) == skippable_magic)
      {
        const unsigned char *length = reader.at(offset + 4, 4);
        if (length == nullptr || offset + 8 + read_le32(length) > size)
          return {ScanStatus::truncated, offset};
        offset += 8 + read_le32(length);
        continue;
      }
      if (magic != frame_magic)
        return {ScanStatus::foreign, offset};

      // Frame header: descriptor, optional window byte, dictionary id, content size.
      const unsigned char *descriptor = reader.at(offset + 4, 1);
      if (descriptor == nullptr)
        return {ScanStatus::truncated, offset};
      const unsigned fhd = *descriptor;
      const bool single_segment = (fhd & 0x20u) != 0;
      const bool checksum = (fhd & 0x04u) != 0;
      static constexpr unsigned dict_sizes[] = {0, 1, 2, 4};
      static constexpr unsigned fcs_sizes[] = {0, 2, 4, 8};
      unsigned fcs = fcs_sizes[fhd >> 6];
      if ((fhd >> 6) == 0 && single_segment)
        fcs = 1;
      std::uint64_t cursor = offset + 4 + 1 + (single_segment ? 0 : 1) + dict_sizes[fhd & 3u] + fcs;

      bool last = false;
      while (!last)
      {
        const unsigned char *header = reader.at(cursor, 3);
        if (header == nullptr)
          return {ScanStatus::truncated, offset};
        const std::uint32_t bits = static_cast<std::uint32_t>(header[0]) |
                                   static_cast<std::uint32_t>(header[1]) << 8 |
                                   static_cast<std::uint32_t>(header[2]) << 16;
        last = (bits & 1u) != 0;
        const unsigned type = (bits >> 1) & 3u;
        const std::uint32_t block_size = bits >> 3;
        if (type == 3)
          return {ScanStatus::foreign, offset};
        cursor += 3 + (type == 1 ? 1 : block_size);
        if (cursor > size)
          return {ScanStatus::truncated, offset};
      }
      if (checksum)
        cursor += 4;
      if (cursor > size)
        return {ScanStatus::truncated, offset};
      offset = cursor;
    }
    return {ScanStatus::complete, offset};
  }

  std::string salvage(platform::File &file, std::uint64_t offset, std::uint64_t size)
  {
    std::string recovered;
    std::uint64_t position = offset;
    auto read = [&](char *buffer, std::size_t capacity) -> std::size_t
    {
      if (position >= size)
        return 0;
      const auto want = static_cast<std::size_t>(std::min<std::uint64_t>(capacity, size - position));
      const auto got = file.read_at(position, buffer, want);
      position += got;
      return got;
    };
    decompress(read, [&](std::string_view chunk)
               { recovered.append(chunk); }, nullptr);
    return recovered;
  }

  // ---------------------------------------------------------------------------
  // Whole-file compression and decompression
  // ---------------------------------------------------------------------------

  bool compress_file(const std::filesystem::path &source, const std::filesystem::path &target, int level,
                     const std::atomic<bool> &stop)
  {
    platform::File input;
    input.open(source, platform::File::Mode::read);
    const std::uint64_t size = input.size();

    auto temporary = target;
    temporary += ".tmp";
    platform::File output;
    output.open(temporary, platform::File::Mode::append, true);

    struct Context
    {
      ZSTD_CCtx *cctx = ZSTD_createCCtx();
      ~Context() { ZSTD_freeCCtx(cctx); }
    } context;
    if (context.cctx == nullptr)
      throw std::bad_alloc();
    level = std::clamp(level, ZSTD_minCLevel(), ZSTD_maxCLevel());
    check(ZSTD_CCtx_setParameter(context.cctx, ZSTD_c_compressionLevel, level), "zstd level");
    check(ZSTD_CCtx_setParameter(context.cctx, ZSTD_c_checksumFlag, 1), "zstd checksum");
    // A known size lets zstd shrink its window (and memory) for small files.
    check(ZSTD_CCtx_setPledgedSrcSize(context.cctx, size), "zstd size");

    std::vector<char> in_buffer(ZSTD_CStreamInSize() * 8);
    std::vector<char> out_buffer(ZSTD_CStreamOutSize());
    std::uint64_t consumed = 0;
    bool aborted = false;
    for (;;)
    {
      if (stop.load(std::memory_order_relaxed))
      {
        aborted = true;
        break;
      }
      std::size_t got = 0;
      if (consumed < size)
      {
        got = input.read_at(consumed, in_buffer.data(),
                            static_cast<std::size_t>(std::min<std::uint64_t>(in_buffer.size(), size - consumed)));
        if (got == 0)
          throw std::runtime_error("log file shrank while it was being compressed");
        consumed += got;
      }
      const bool last = consumed >= size;
      ZSTD_inBuffer in{in_buffer.data(), got, 0};
      for (;;)
      {
        ZSTD_outBuffer out{out_buffer.data(), out_buffer.size(), 0};
        const std::size_t remaining = ZSTD_compressStream2(context.cctx, &out, &in, last ? ZSTD_e_end : ZSTD_e_continue);
        check(remaining, "zstd compression");
        output.write(out_buffer.data(), out.pos);
        if (last ? remaining == 0 : in.pos == in.size)
          break;
      }
      if (last)
        break;
    }
    output.close();
    input.close();

    std::error_code ec;
    if (aborted)
    {
      std::filesystem::remove(temporary, ec);
      return false;
    }
    ec = platform::rename_file(temporary, target);
    if (ec)
    {
      std::filesystem::remove(temporary, ec);
      throw std::system_error(ec, "cannot rename '" + temporary.string() + "'");
    }
    std::filesystem::remove(source, ec);
    return true;
  }

  bool decompress(std::function<std::size_t(char *, std::size_t)> read,
                  const std::function<void(std::string_view)> &sink, std::string *error)
  {
    struct Context
    {
      ZSTD_DCtx *dctx = ZSTD_createDCtx();
      ~Context() { ZSTD_freeDCtx(dctx); }
    } context;
    if (context.dctx == nullptr)
      throw std::bad_alloc();

    std::vector<char> in_buffer(ZSTD_DStreamInSize());
    std::vector<char> out_buffer(ZSTD_DStreamOutSize());
    std::size_t last_result = 0;
    for (;;)
    {
      const std::size_t got = read(in_buffer.data(), in_buffer.size());
      if (got == 0)
        break;
      ZSTD_inBuffer in{in_buffer.data(), got, 0};
      bool output_full = false;
      // A full output buffer may mean more output is pending for consumed input.
      while (in.pos < in.size || output_full)
      {
        ZSTD_outBuffer out{out_buffer.data(), out_buffer.size(), 0};
        last_result = ZSTD_decompressStream(context.dctx, &out, &in);
        if (ZSTD_isError(last_result))
        {
          if (error != nullptr)
            *error = ZSTD_getErrorName(last_result);
          return false;
        }
        if (out.pos > 0)
          sink(std::string_view(out_buffer.data(), out.pos));
        output_full = out.pos == out.size;
      }
    }
    if (last_result != 0)
    {
      if (error != nullptr)
        *error = "truncated: the last frame is incomplete";
      return false;
    }
    return true;
  }
}
