# Changelog

All notable changes to this project are documented here. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project uses [Semantic Versioning](https://semver.org/).

## [Unreleased]

## [0.2.0] - 2026-09-23

A rewrite. The pre-0.2 API still compiles through `<log.hpp>` / `<tinylog/compat.hpp>`.

### Added
- New API in `<tinylog/tinylog.hpp>`:
  - `TLOG_*` macros with compile-time checked format strings, which skip argument evaluation for disabled levels.
  - `TINYLOG_ACTIVE_LEVEL` to strip levels at compile time.
- Levels `trace` and `fatal`. Threshold semantics via `set_level()`, plus per-level `enable()` and `disable()`.
- Async mode: a bounded queue with a `block` or `drop` overflow policy, batched writes, and a dropped-message counter (`tinylog::stats()`).
- Flush policy by level and by interval. `flush()`, `wait_idle()`, `shutdown()` and the `tinylog::Guard` RAII helper.
- File rotation:
  - by size, by UTC-aligned time interval, or at open;
  - timestamped names;
  - retention by count;
  - zstd compression of rotated files on a background thread;
  - leftovers from interrupted runs are compressed at the next start.
- Live compression of the active log as appendable zstd frames:
  - an unfinished frame left by a crash is salvaged on the next open;
  - non-zstd data is moved aside rather than destroyed;
  - after a write error, the file is cut back to the last complete frame.
- `tinylog::read_log()` and the `tinylog-cat` tool, which read plain, compressed and truncated logs.
- JSON Lines output, and optional thread id and source location in text output.
- Configuration from a settings string (`parse_config`) and from the environment (`TINYLOG_CONFIG`, `TINYLOG_LEVEL`).
- Custom sinks (`Sink`, `CallbackSink`), `make_file_sink`, `make_console_sink`, and `add_sink()`.
- C API (`<tinylog/tinylog.h>`) with a stable ABI.
- Static and shared builds, CMake package config, pkg-config file, and CPack archives.
- Versioning:
  - `<tinylog/version.hpp>`, `tinylog::version()`, and a DLL version resource;
  - `init()` rejects a header/library version mismatch.
- GoogleTest suite, the `tinylog_bench` profiling harness (optionally against spdlog and Quill), and GitHub Actions CI:
  - Linux GCC/Clang, MSVC, MinGW and macOS;
  - ASan/UBSan and TSan;
  - consumer tests, system-zstd build, and benchmark summary;
  - a tag-driven release workflow.

### Changed
- The logger singleton lives in the library. An EXE and the DLLs that include the headers now share one logger; before, each module had its own.
- `warning`, `error`, `critical` and `fatal` are enabled by default. Before, only `info` was.
- One line per message: the trailing `\n` is optional.
- The Windows console gets UTF-16 output, so UTF-8 text renders without changing the process code page.
- Output redirected to a file or pipe is no longer lost.
- Colours use ANSI/VT sequences and honour `NO_COLOR`.
- Rotated file names changed from `log.1.txt` to `log.20260923-101502.txt`. A name no longer changes after rotation.
- zstd compression of rotated files defaults to level 19, not 22 (which needs hundreds of MiB of RAM), and runs off the logging path.
- Sources moved to `include/tinylog/` and `src/`.
- zstd is compiled into the library with hidden symbols.
- Format errors are logged instead of thrown. Exceptions from sinks are contained and counted.

### Fixed
- Compressing a rotated log blocked every logging thread, and read the whole file into memory.
- With `max_backup_count = 0` ("unlimited"), every rotation overwrote the previous backup.
- `LOG_EXCEPTION` did not compile with MSVC (`__PRETTY_FUNCTION__`).
- `<windows.h>` leaked from the public header (`min`/`max` macros).
- `DllMain` did work under the loader lock and ignored its own `DEBUG` setting.
- The version resource declared the DLL as an application.
- `-Ofast` was set in variables that were never used, and shared-only builds were forced.

## [0.1.0] - 2023-09-10

- Singleton logger with console, file and null tracers, a severity bitmask, and a Windows DLL build.
