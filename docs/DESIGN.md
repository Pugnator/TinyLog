# Design notes

This page covers how tinylog works, which logging best practices it follows, and what it deliberately does not do.

## Data path

```
TLOG_INFO(fmt, args...)
  │ should_log(level): one relaxed atomic load; arguments not evaluated when off
  ▼
detail::vlog(): std::vformat_to into a thread-local buffer (compiled once, in the library)
  ▼
Core::submit(): timestamp, OS thread id
  ├─ sync:  lock sinks → Sink::write() → flush by policy
  └─ async: lock queue → memcpy header+text into the active buffer → return
               worker: swap buffers, dispatch the whole batch, flush by policy
```

- **Async queue.** It is a pair of byte arenas: a record header followed by the message text. There is no allocation per message in steady state.
- **Producers rarely notify the worker.** The worker polls every 1 ms while records keep arriving, and blocks only after about 100 idle polls. Producers notify only a sleeping worker, or one whose buffer is half full.
  - The first version woke the worker for every message: 0.27 M msg/s at 800 ns p50.
  - Polling brought that to 1.6–2.1 M msg/s at 400 ns p50.
- **Bounded memory.** `queue_capacity` limits memory. When the queue is full, `block` waits and `drop` discards and counts the message in `stats().dropped`.
- **Sinks see one caller at a time.** The core serializes calls, so sinks need no locks of their own.
  - A sink that throws is contained and counted.
  - A sink that logs from inside `write()` is refused instead of deadlocking.

## Files

- **Rotation triggers.**
  - Size is exact for plain files. For compressed files it counts compressed bytes already written, so the file can overshoot by about one block.
  - The time interval is aligned to UTC.
  - `on_open` rotates a file left by the previous run.
- **Naming.** Rotated files get timestamped names (`app.20260923-101502[.N].log`), so no file is renamed twice.
  - Several rotations in one second get increasing sequence numbers. A number freed by retention is never reused; a reused number would make the newest file look oldest.
- **Background work.** Compression and retention run on one background thread per file sink, in order. Retention therefore never deletes a file that is being compressed.
  - Compression writes `*.zst.tmp`, renames it into place, and only then deletes the source.
  - An interrupted run leaves either a `.tmp`, which is deleted, or an uncompressed rotated file, which is compressed at the next start.
- **Windows renames** are retried for about 200 ms, because antivirus scanners and search indexers briefly lock new files.
  - Log files are opened with `FILE_SHARE_DELETE`, so other tools can rotate or remove them.
  - If a rotation still fails, logging continues in the current file and rotation is retried 30 s later.
- **Live compression** uses independent zstd frames of `frame_size` input bytes each. Concatenated frames are a valid zstd stream (zstd format spec §2), so appending after a restart needs no special reader.
  - A flush ends a *block*, not the frame, so everything flushed can be decoded.
  - Tiny blocks compress badly: one block per record came out larger than the plain text. Flush requests are therefore coalesced to at most one per `compressed_flush_interval` (default 1 s) or 64 KiB.
  - Explicit `flush()`, `shutdown()`, re-`init()` and rotation always write everything.
- **Crash recovery.** On open, frame and block headers are walked; nothing is decompressed.
  - An incomplete last frame is decoded as far as possible, cut off, and its text compressed again into a new frame.
  - Data that is not zstd at all is moved aside (`*.corrupt`) and never deleted.
  - After a write error such as a full disk, the file is cut back to the start of the unfinished frame, so it stays decodable.

## Best practices applied

| Practice | How |
|---|---|
| Level check before evaluating arguments | Macros test `should_log()` first; `TINYLOG_ACTIVE_LEVEL` removes levels at compile time. |
| Compile-time format checking | `std::format_string<Args...>`. Run-time format strings (`log_runtime`, the compat API) report errors in the log instead of throwing. |
| Logging never throws or crashes the caller | Format errors, sink exceptions, I/O errors and re-entrancy are caught, counted in `stats().errors`, and reported to stderr with a rate limit. |
| Bounded async queue with an explicit overflow policy | `queue_capacity`, `block` or `drop`, drop counter. |
| Flush by severity and by time; durability explicit | `flush_level`, `flush_interval`. A flush hands data to the OS (it survives a process crash), not `fsync`. |
| Explicit shutdown | `shutdown()` / `Guard`. Logging after shutdown is synchronous, so late messages from static destructors are not lost. |
| Windows loader lock | `DllMain` only records that the process is exiting. Static finalization never joins threads on Windows; it waits for the worker to finish, then detaches. |
| One logger across EXE and DLLs | The state lives in the library; header code only formats. The DLL exports the level mask, so the hot path does not call into the DLL. |
| No `<windows.h>` in public headers | All OS code is in `src/platform.cc`. |
| UTF-8 everywhere | Consoles get `WriteConsoleW` without changing the process code page; paths are `std::filesystem::path`; JSON escapes control characters only. |
| Redirected output | Consoles are detected (`GetConsoleMode` / `isatty`); files and pipes get plain bytes and no colour. `NO_COLOR` is honoured. |
| Structured output | JSON Lines, with UTC ISO-8601 timestamps. |
| Configuration without recompiling | Settings string, `TINYLOG_CONFIG`, `TINYLOG_LEVEL`. |
| Stable ABI option | C API. The C++ API needs the same compiler and standard library as the DLL. |
| Symbol hygiene | zstd is compiled in with hidden visibility and namespaced xxhash; the shared build uses `-fvisibility=hidden`. |
| Versioning | Semantic versions, a generated header, the SONAME, the DLL version resource, the CMake package version, and a run-time version check in `init()`. |

## Limitations and possible next steps

- **Caller-side cost.** Formatting happens on the calling thread, about 0.4 µs per message. Deferred formatting (Quill-style: copy the arguments, format on the worker) would reduce this to tens of ns, but needs type-erased argument capture with lifetime rules for pointers and views. Worth it only if logging on latency-critical threads matters.
- **No crash-signal handler.** A crash loses what is still queued in async mode. Use sync mode, or a low `flush_level`, where that matters. A best-effort handler (POSIX signals plus a Windows vectored exception handler) could drain the queue.
- **One global logger.** There are no named loggers or per-module levels. Per-module levels could be added by giving `SourceLocation` a category.
- **Retention by count only.** There is no maximum total size, age limit or free-space check.
- **Time rotation aligns to UTC boundaries.** Daily rotation at local midnight would need time-zone offsets.
- **`fork()` without `exec()`** is not handled: the child has no async worker. Call `init()` again in the child.
