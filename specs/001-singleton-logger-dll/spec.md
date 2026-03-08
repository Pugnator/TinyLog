# Feature Specification: Singleton Logger DLL

**Feature Branch**: `001-singleton-logger-dll`  
**Created**: 2026-03-08  
**Status**: Draft  
**Input**: User description: "Singleton-based C++20 logging library built as a Windows DLL with configurable trace backends, severity filtering, and cross-platform support"

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Basic Message Logging (Priority: P1)

As a developer integrating the logger DLL into my application, I want to log informational messages with a single macro call so that I can trace application behavior without writing boilerplate code.

**Why this priority**: This is the core value proposition — zero-friction logging through convenience macros (`LOG`, `LOG_INFO`, `LOG_DEBUG`). Without this, the library has no purpose.

**Independent Test**: Can be fully tested by calling `LOG("Hello {}\n", "world")` from a host application and verifying the message appears on the console or in the log file with a correct timestamp prefix.

**Acceptance Scenarios**:

1. **Given** a host application linked to logger.dll, **When** the developer calls `LOG("Server started on port {}\n", port)`, **Then** a timestamped message `[YYYY-MM-DD HH:MM:SS] Server started on port 8080` appears on the active trace output.
2. **Given** a host application linked to logger.dll, **When** the developer calls `LOG_DEBUG("Cache miss for key {}\n", key)`, **Then** a timestamped message prefixed with `Debug:` appears on the active trace output, provided the debug severity level is enabled.
3. **Given** the logger is initialized with default settings, **When** the DLL is loaded by a host process, **Then** the logger auto-configures to console output with at least the info severity level enabled.

---

### User Story 2 - Severity-Based Filtering (Priority: P1)

As a developer, I want to control which severity levels are active at runtime so that I can reduce noise in production while retaining full verbosity during debugging.

**Why this priority**: Severity filtering is essential for any usable logger — without it, output is either too noisy or too silent. This is tightly coupled with the core logging functionality.

**Independent Test**: Can be tested by enabling only `info` level, logging messages at `info`, `debug`, and `warning`, and verifying only the `info` message appears.

**Acceptance Scenarios**:

1. **Given** only the `info` severity level is enabled, **When** the developer logs a `debug` message, **Then** the message is silently discarded and does not appear in output.
2. **Given** the developer calls `LOG_LEVEL(TraceSeverity::debug)` at runtime, **When** subsequent `debug` messages are logged, **Then** those messages appear in output.
3. **Given** multiple severity levels are enabled via bitmask (e.g., `info | error`), **When** messages are logged at `info`, `warning`, `error`, and `debug`, **Then** only `info` and `error` messages appear.
4. **Given** the developer calls `clear_level(TraceSeverity::debug)`, **When** a `debug` message is logged afterward, **Then** it is discarded.

---

### User Story 3 - Configurable Trace Backend (Priority: P2)

As a developer, I want to switch the logging backend between console, file, and devnull at runtime so that I can adapt the logger to different deployment environments without recompiling.

**Why this priority**: Backend flexibility is what makes this library reusable across console apps, services, and scenarios where logging should be disabled entirely.

**Independent Test**: Can be tested by configuring `TraceType::file`, logging a message, and verifying the message appears in `log.txt`; then switching to `TraceType::devnull` and verifying no further output is produced.

**Acceptance Scenarios**:

1. **Given** the logger is configured with `TraceType::console`, **When** a message is logged, **Then** the message appears on the standard console output with color-coded severity on Windows.
2. **Given** the logger is configured with `TraceType::file`, **When** a message is logged, **Then** the message is appended to a log file with a timestamp.
3. **Given** the logger is configured with `TraceType::file` and a custom filepath, **When** a message is logged, **Then** the message is appended to the specified file path rather than the default `log.txt`.
4. **Given** the logger is configured with `TraceType::devnull`, **When** a message is logged, **Then** no output is produced anywhere.
5. **Given** the logger is configured with `TraceType::file` and the file path is invalid, **When** the configuration is applied, **Then** an exception is thrown indicating the file could not be opened.

---

### User Story 4 - Thread-Safe Logging (Priority: P2)

As a developer working on a multithreaded application, I want the logger to be safe to call from multiple threads simultaneously so that I do not need external synchronization.

**Why this priority**: Thread safety is a fundamental requirement for any shared library used in real-world applications where concurrent access is the norm.

**Independent Test**: Can be tested by spawning multiple threads that each log 1000 messages concurrently, then verifying all messages appear in output without corruption or interleaving within a single log line.

**Acceptance Scenarios**:

1. **Given** multiple threads calling `LOG_INFO(...)` concurrently, **When** all threads complete, **Then** every message is present in the output and no message is corrupted or partially written.
2. **Given** multiple threads logging to a file backend simultaneously, **When** the file is inspected, **Then** each line is a complete, well-formed log entry with a timestamp.

---

### User Story 5 - Color-Coded Console Output on Windows (Priority: P3)

As a developer running a Windows console application, I want log messages to be color-coded by severity so that I can visually distinguish errors from informational messages at a glance.

**Why this priority**: Color coding is a usability enhancement that improves developer experience but is not strictly required for the logging functionality.

**Independent Test**: Can be tested by logging messages at each severity level to the console and visually verifying distinct colors for info (cyan), debug (green), warning (yellow), error (red), critical (magenta), and fatal (red background).

**Acceptance Scenarios**:

1. **Given** the console tracer is active on Windows, **When** an `info` message is logged, **Then** the text appears in cyan (green+blue intensity).
2. **Given** the console tracer is active on Windows, **When** an `error` message is logged, **Then** the text appears in bright red.
3. **Given** the console tracer is active on Windows, **When** a `fatal` message is logged, **Then** the text appears with a red background, and console colors are reset afterward.

---

### User Story 6 - DLL Lifecycle Integration (Priority: P3)

As a developer loading the logger as a DLL, I want the logger to automatically initialize on DLL attach and be ready to use without explicit setup so that integration is seamless.

**Why this priority**: Automatic initialization reduces integration friction, but developers can also call `configure()` explicitly if needed.

**Independent Test**: Can be tested by loading `logger.dll` via `LoadLibrary`, and immediately calling a log macro to verify the logger is operational without prior setup.

**Acceptance Scenarios**:

1. **Given** a host application loads logger.dll, **When** `DLL_PROCESS_ATTACH` fires, **Then** the logger singleton is initialized with a default severity level (debug in DEBUG builds, info in release).
2. **Given** the DLL is loaded and the logger is initialized, **When** the developer calls `LOG("test\n")` without any prior setup, **Then** the message is logged successfully.

---

### Edge Cases

- What happens when the log file cannot be opened (e.g., read-only directory, disk full)? The `FileTracer` constructor throws `std::runtime_error`.
- What happens when `ConsoleTracer` cannot obtain a valid stdout handle? Messages are silently discarded (no crash).
- What happens when `log()` is called with a severity not handled by the switch statement (e.g., `critical`)? It falls through to the default case and is output as an info-level message.
- What happens when `Fatal` is called on `FileTracer`? Currently a no-op — no message is written.
- What happens when `reset_levels()` is called and then a message is logged? All severity levels are disabled; the message is silently discarded.
- What happens when `std::vformat` fails due to mismatched format args? An exception propagates to the caller.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: System MUST provide a singleton `Log` instance accessible via `Log::get()` that is thread-safe on initialization (Meyers' singleton via function-local static).
- **FR-002**: System MUST support logging messages using `std::format`-compatible format strings with variadic arguments.
- **FR-003**: System MUST support six severity levels: `info` (1), `warning` (2), `error` (4), `debug` (8), `verbose` (16), `critical` (32), represented as a bitmask.
- **FR-004**: System MUST allow enabling, disabling, and resetting severity levels at runtime via `set_level`, `clear_level`, and `reset_levels`.
- **FR-005**: System MUST silently discard log messages whose severity level is not currently enabled.
- **FR-006**: System MUST support three trace backends: console output (`ConsoleTracer`), file output (`FileTracer`), and null output (`VoidTracer`).
- **FR-007**: System MUST allow switching the trace backend at runtime via `configure(TraceType)`.
- **FR-008**: System MUST allow configuring the file tracer with a custom file path via `configure(TraceType::file, filepath)`.
- **FR-009**: System MUST prepend a timestamp in `[YYYY-MM-DD HH:MM:SS]` format to every log message.
- **FR-010**: System MUST prepend a severity-specific prefix (e.g., `Debug:`, `Warning:`, `ERROR:`, `CRITICAL:`, `*** FATAL ***:`) for non-info messages.
- **FR-011**: System MUST protect concurrent log writes with a mutex in both `FileTracer` and `ConsoleTracer`.
- **FR-012**: System MUST provide convenience macros (`LOG`, `LOG_INFO`, `LOG_DEBUG`, `LOG_CALL`, `LOG_LEVEL`, `LOG_EXCEPTION`) for ergonomic usage.
- **FR-013**: On Windows, the console tracer MUST color-code output by severity using Win32 `SetConsoleTextAttribute`.
- **FR-014**: On Windows, the console tracer MUST set the console output code page to UTF-8 (65001).
- **FR-015**: The `FileTracer` MUST flush after every write to ensure messages are persisted immediately.
- **FR-016**: The `FileTracer` MUST throw `std::runtime_error` if the log file cannot be opened.
- **FR-017**: System MUST be buildable as a shared library (DLL on Windows) via CMake with C++20 standard.
- **FR-018**: On Windows, the DLL entry point MUST initialize the logger singleton on `DLL_PROCESS_ATTACH`.
- **FR-019**: System MUST auto-detect execution context on Windows — use file tracer when no module handle is available, console tracer otherwise.
- **FR-020**: System MUST support cross-platform compilation for Windows (MSVC, MinGW) and Linux (GCC/Clang).

### Key Entities

- **Log**: The singleton facade that owns the active `Tracer` instance and manages severity-level bitmask. Entry point for all logging operations.
- **Tracer**: Abstract interface defining the contract for all trace backends (Info, Debug, Warning, Error, Critical, Fatal).
- **FileTracer**: Concrete tracer that writes timestamped log entries to a file, with mutex-guarded I/O and automatic flush.
- **ConsoleTracer**: Concrete tracer that writes to the system console, with color-coded severity on Windows and plain text on Linux.
- **VoidTracer**: Null-object tracer that discards all messages silently.
- **TraceSeverity**: Bitmask enum representing log severity levels (info, warning, error, debug, verbose, critical).
- **TraceType**: Enum representing available backend types (devnull, console, file; plus uart/swd/rtt on ARM).

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: A developer can integrate the logger into a new project by including a single header file and linking the DLL, with no additional configuration required for basic console logging.
- **SC-002**: All six severity levels can be independently toggled at runtime, and only messages matching enabled levels appear in output.
- **SC-003**: Switching between console, file, and devnull backends at runtime takes effect immediately for all subsequent log calls without restart.
- **SC-004**: Concurrent logging from 10+ threads produces zero corrupted or interleaved log lines over 10,000 total messages.
- **SC-005**: Every log entry includes a correctly formatted `[YYYY-MM-DD HH:MM:SS]` timestamp that reflects the actual time of the log call.
- **SC-006**: The library compiles cleanly as a C++20 shared library on both MSVC and MinGW toolchains via CMake.
- **SC-007**: File-based logging persists all messages to disk immediately (flush after each write), surviving abrupt process termination with at most one lost message.
- **SC-008**: The DLL initializes and is ready for logging upon `LoadLibrary` without any explicit setup calls from the host application.
