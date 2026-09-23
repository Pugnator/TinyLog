# tinylog

A small, fast C++20 logger for Windows, Linux and macOS, built on `std::format`, with no dependencies and zstd compression built in.

- **Compile-time checked formatting**: `TLOG_INFO("port {}", port)`. Arguments are not evaluated when the level is off.
- **Sync or async**: async mode uses a bounded queue with a `block` or `drop` policy, and dropped messages are counted.
- **Sinks**: console (colour, UTF-8 on Windows consoles, safe when redirected), files, and your own via `Sink` or `CallbackSink`.
- **Rotation**: by size, by time interval, or at start-up, with retention by file count. Rotated files are compressed with zstd **on a background thread**.
- **Compressed live log**: the active file itself can be a zstd stream of appended frames.
  - A restarted process keeps appending to it.
  - `zstd -d` reads the whole file.
  - A frame cut short by a crash is repaired on the next start.
- **Text or JSON Lines** output, with optional thread id and source location.
- **Configuration**: from code (`tinylog::Config`), from a string (`"level=debug;file=app.log;rotate.size=10M"`), or from the environment (`TINYLOG_LEVEL`, `TINYLOG_CONFIG`).
- **Linking**: static library, shared library/DLL (one logger shared by the EXE and every DLL), or a C API with a stable ABI.
- **Compatibility**: code written for the original `Log::get()` / `LOG_*` API still compiles.

## Quick start

```cpp
#include <tinylog/tinylog.hpp>

int main()
{
  tinylog::Config config;
  config.level = tinylog::Level::debug;
  config.mode = tinylog::Mode::async;

  auto &file = config.files.emplace_back();
  file.path = "logs/app.log";
  file.rotation.max_size = 10 << 20;                     // rotate at 10 MiB
  file.rotation.max_files = 7;                           // keep 7 rotated files
  file.rotation.compress = tinylog::Compression::zstd;   // compress them in the background

  tinylog::Guard guard(config);                          // init() now, shutdown() at scope exit

  TLOG_INFO("listening on {}:{}", "0.0.0.0", 8080);
  TLOG_WARN("disk {:.1f}% full", 91.5);
}
```

Without `init()`, messages at `info` and above go to stdout.

Macros: `TLOG_TRACE`, `TLOG_DEBUG`, `TLOG_INFO`, `TLOG_WARN`, `TLOG_ERROR`, `TLOG_CRITICAL`, `TLOG_FATAL`.

Compile levels out entirely with `-DTINYLOG_ACTIVE_LEVEL=2`, where `2` means `info` and above remain.

**Call `tinylog::shutdown()` (or keep a `Guard`) before `main()` returns and before unloading a DLL that logs.** Worker threads cannot be joined safely from static destructors or `DllMain`.

## Configuration reference

| `Config` field | Default | Meaning |
|---|---|---|
| `level` | `info` | Threshold. `set_level()` changes it at run time; `enable()` and `disable()` toggle single levels. |
| `mode` | `sync` | `async`: callers format and enqueue, a worker thread writes in batches. |
| `queue_capacity` / `overflow` | 4 MiB / `block` | Async queue size in bytes, and what happens when it is full (`block` or `drop`). |
| `flush_level` / `flush_interval` | `trace` / 1 s | Flush after records at this level or above, and at least this often. |
| `console` | stdout | `ConsoleSinkConfig`: stream, `color` (`automatic` honours `NO_COLOR`), `min_level`, layout. `std::nullopt` disables it. |
| `files` | none | `FileSinkConfig` list. See the next table. |
| `sinks` | none | Your own `std::shared_ptr<Sink>`s. |
| `use_env` | `true` | Apply `TINYLOG_CONFIG` and `TINYLOG_LEVEL` on top. |

| `FileSinkConfig` field | Default | Meaning |
|---|---|---|
| `path` | `log.txt` | Active file. Missing directories are created. |
| `compression` / `compression_level` | `none` / 3 | Compress the **active** file as appendable zstd frames. `.zst` is appended to the name. |
| `frame_size` | 4 MiB | Uncompressed bytes per frame. |
| `compressed_flush_interval` | 1 s | Shortest time between compressed block flushes. Tiny blocks compress badly. 0 = flush on every flush request. |
| `rotation.max_size` | 0 (off) | Rotate before the file exceeds this many bytes. |
| `rotation.interval` | 0 (off) | Rotate on UTC-aligned boundaries, e.g. `std::chrono::hours(24)`. |
| `rotation.on_open` | `false` | Rotate a non-empty file left by the previous run. |
| `rotation.max_files` | 5 | Rotated files to keep. 0 = all. |
| `rotation.compress` / `compress_level` | `none` / 19 | zstd for rotated files, on a background thread. |
| `layout` | text, ms | `format` (`text` or `json`), `timestamp` precision, `utc`, `show_level`, `show_thread`, `show_source`. |
| `buffer_size`, `truncate`, `min_level` | 64 KiB, `false`, `trace` | |

Rotated files are named `app.20260923-101502.log[.zst]`, with `.1`, `.2`, … added for several rotations in one second.

### Settings string and environment

The same options as a string. This is what `TINYLOG_CONFIG` and the C API's `tinylog_init()` accept:

```
level=debug; mode=async; flush=warning; console=stderr; timestamp=us
file=logs/app.log; rotate.size=10M; rotate.keep=7; rotate.compress=zstd
file=logs/errors.jsonl; file.level=error; file.format=json
```

`file=` starts a new file sink. The `file.*` and `rotate.*` keys after it apply to that sink.

Unknown keys and bad values throw `std::invalid_argument` naming the key.

`TINYLOG_LEVEL=debug` also takes effect before `init()` is called.

## Reading compressed logs

A compressed log is plain zstd. `zstd -dc app.log.zst` and `zstdcat` work on finished files.

A live file, or one cut short by a crash, ends inside a frame. `zstd` reports "premature end" for such a file. `tinylog-cat` prints everything that decodes:

```
tinylog-cat logs/app.log.zst logs/app.20260922-000000.log.zst | grep ERROR
```

From code: `tinylog::read_log(path)` or the streaming overload, in `<tinylog/reader.hpp>`.

## Building

```
git submodule update --init          # vendor/zstd
cmake --preset release && cmake --build --preset release && ctest --preset release
```

Presets: `debug`, `release`, `msvc`, `asan`, `tsan`, `bench`.

Requirements:
- A C++20 compiler with `<format>`: GCC 13+, Clang 17+, MSVC 19.30+ (VS 2022), or AppleClang 15+ with `CMAKE_OSX_DEPLOYMENT_TARGET` ≥ 13.3.
- CMake 3.21+.

| CMake option | Default | |
|---|---|---|
| `TINYLOG_BUILD_STATIC` | ON | Target `tinylog` (alias `tinylog::tinylog`, legacy `tinyLog`). |
| `TINYLOG_BUILD_SHARED` | ON when top level | Target `tinylog_shared` (`tinylog::shared`): `tinylog.dll` / `libtinylog.so`. |
| `TINYLOG_USE_SYSTEM_ZSTD` | OFF | Link the installed libzstd instead of compiling `vendor/zstd` into the library. |
| `TINYLOG_BUILD_TESTS` / `_BENCH` / `_TOOLS` | ON when top level | |
| `TINYLOG_SANITIZE` | empty | e.g. `address,undefined` or `thread`. |
| `TINYLOG_BENCH_COMPARE` | OFF | Also benchmark spdlog and Quill (downloaded). |

### Using it from another project

```cmake
add_subdirectory(third_party/tinylog)          # builds the static library only
target_link_libraries(app PRIVATE tinylog::tinylog)
# or, after `cmake --install`:
find_package(tinylog 0.2 REQUIRED)
target_link_libraries(app PRIVATE tinylog::tinylog)   # tinylog::shared for the DLL
```

zstd is compiled into tinylog with hidden symbols, so consumers never link it.

pkg-config users get `tinylog.pc`.

**Migrating from the pre-0.2 layout:** projects that compiled `log.cc` themselves and ran `add_subdirectory(vendor/zstd/build/cmake)` should replace both with the `add_subdirectory` line above. The `tinyLog` and `tinylog` target names and `#include "logger/log.hpp"` / `<log.hpp>` keep working.

## Compatibility API

```cpp
#include <log.hpp>   // or <tinylog/compat.hpp>
Log::get().configure(TraceType::file, "app.log", RotationConfig{.max_file_size = 5 << 20, .max_backup_count = 5, .compress = true});
Log::get().set_level(TraceSeverity::debug);
LOG_DEBUG("loaded {} items\n", n);
```

Changes from the original behaviour:
- The singleton now lives in the library, so an EXE and its DLLs share it.
- `warning`, `error`, `critical` and `fatal` are enabled by default.
- The trailing `\n` is optional.
- `LOG_WARNING`, `LOG_WARN`, `LOG_ERROR`, `LOG_CRITICAL` and `LOG_FATAL` are provided.
- `LOG_EXCEPTION` compiles on MSVC.
- Format errors are logged instead of thrown.

## C API

`<tinylog/tinylog.h>` provides `tinylog_init(settings)`, `tinylog_write(level, file, line, func, msg, len)`, `tinylog_set_level`, `tinylog_flush`, `tinylog_shutdown` and `tinylog_version`.

Use it from other compilers or languages, or through `LoadLibrary`/`dlopen`.

## Performance

Run `tinylog_bench` for latency percentiles and throughput. Build with `--preset bench` to compare against spdlog and Quill.

These numbers are from an i7-7700 under Linux (WSL2), 500k messages per thread, with a 4-argument message. The timer resolution is 100 ns.

| | tinylog | spdlog 1.17 | Quill 13 |
|---|---|---|---|
| disabled level | < 100 ns | < 100 ns | < 100 ns |
| async file, 1 thread: p50 / p99 / throughput | 400 ns / 0.9 µs / 2.1 M/s | 400 ns / 2.7 µs / 1.5 M/s | < 100 ns / 100 ns / 2.1 M/s |
| async file, 4 threads: p50 / p99 / throughput | 0.8 µs / 5.8 µs / 1.9 M/s | 59 µs / 165 µs / 0.07 M/s | < 100 ns / 100 ns / 1.9 M/s |
| sync buffered file, 4 threads: throughput | 2.0 M/s | 0.7 M/s | — |
| live zstd, 43 MiB of text | 0.9 MiB on disk, same throughput | — | — |

tinylog formats in the calling thread, like spdlog. Quill copies arguments and formats on its backend thread, so its caller-side cost is about ten times lower.

See [docs/DESIGN.md](docs/DESIGN.md).

## Versioning

- Semantic versioning. `project(VERSION)` in `CMakeLists.txt` is the single source.
- The version reaches `<tinylog/version.hpp>`, `tinylog::version()`, the DLL's version resource, the shared library's SONAME, and the CMake package version.
- Before 1.0, a minor release may break compatibility.
- `init()` refuses a header/library version mismatch, which catches a stale DLL.
- Changes are recorded in [CHANGELOG.md](CHANGELOG.md).
- Pushing a `vX.Y.Z` tag builds and publishes release packages.

## License

MIT, see [LICENSE](LICENSE). zstd, in `vendor/zstd`, is BSD-licensed.
