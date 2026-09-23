// tinylog-cat: prints log files, decompressing zstd logs (also live or
// crash-truncated ones, which `zstd -d` rejects).
//
//   tinylog-cat app.log.zst app.20260922-000000.log.zst > all.txt

#include <tinylog/reader.hpp>
#include <tinylog/tinylog.hpp>

#include <cstdio>
#include <cstring>
#include <exception>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

int main(int argc, char **argv)
{
  if (argc < 2 || std::strcmp(argv[1], "--help") == 0 || std::strcmp(argv[1], "-h") == 0)
  {
    std::fprintf(stderr, "usage: tinylog-cat [--version] FILE...\n"
                         "Prints plain or zstd-compressed tinylog files to stdout.\n");
    return argc < 2 ? 2 : 0;
  }
  if (std::strcmp(argv[1], "--version") == 0)
  {
    std::printf("tinylog-cat %.*s\n", static_cast<int>(tinylog::version().size()), tinylog::version().data());
    return 0;
  }
#if defined(_WIN32)
  _setmode(_fileno(stdout), _O_BINARY);
#endif
  int status = 0;
  for (int i = 1; i < argc; ++i)
  {
    try
    {
      const auto result = tinylog::read_log(argv[i], [](std::string_view text)
                                            { std::fwrite(text.data(), 1, text.size(), stdout); });
      if (result.truncated)
        std::fprintf(stderr, "tinylog-cat: %s: last frame incomplete (file still being written?)\n", argv[i]);
      if (result.corrupt)
      {
        std::fprintf(stderr, "tinylog-cat: %s: %s\n", argv[i], result.error.c_str());
        status = 1;
      }
    }
    catch (const std::exception &e)
    {
      std::fprintf(stderr, "tinylog-cat: %s\n", e.what());
      status = 1;
    }
  }
  return status;
}
