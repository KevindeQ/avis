# Task 1 — Foundations: error type, logging, test helpers

| | |
|---|---|
| Design | [01 §6.2](../design/01-system-architecture.md#62-error-type), [13 §7](../design/13-build-and-quality.md#7-logging), [14 §5](../design/14-library-runtime.md#5-static-access-rules) |
| New headers | `include/avis/error.h`, `include/avis/log.h` |
| New sources | `lib/avis/error.cpp`, `lib/avis/log.cpp` |
| Dependency | spdlog 1.17.0 (`external/spdlog-1.17.0`, integrated by [Task 0](00-external-dependencies.md)) |

The thread pool, task system and runtime all report failures through `avis::error` and log through `avis::log`. Neither exists yet, so they come first.

> **Status (2026-10-18): done.** Debug: all 26 test cases pass, also in random order over repeated runs. Release builds and passes too (run later, during Task 3). Decisions made while implementing are listed under *As implemented*.

## 1.1 Error type
- [x] Create `include/avis/error.h` with `avis::error_domain`, `avis::error` and `avis::result<T> = std::expected<T, error>` exactly as in [01 §6.2](../design/01-system-architecture.md#62-error-type).
- [x] Add a `std::formatter<avis::error>` that prints `"[<domain>:<code>] <message> (<file>:<line>)"`. Add a `to_string(error_domain)`.
- [x] Add `avis::runtime_error : std::runtime_error` carrying an `avis::error`, for the unrecoverable paths (`runtime` construction failure, [14 §2](../design/14-library-runtime.md#2-api)).
- [x] Add `avis::error make_error(std::exception_ptr, std::source_location = current())`: rethrows and catches `avis::runtime_error` (keeps its error), `std::system_error` (keeps the code), `std::exception` (`what()`), and `...` (`"unknown exception"`). The thread pool ([06 §3.3](../design/06-thread-pool.md#33-body-contract)) and the task system ([15 §4.4](../design/15-parallel-scheduler.md#42-error-type)) both use it.
- [x] Define the generic error codes the thread pool needs (see Open points): at least `pool_exhausted`, `pool_stopping`, `invalid_configuration`, `cancelled`.

**Tests** (`tests/test_error.cpp`): formatting, `source_location` captured at the call site, `make_error` for each exception category.

## 1.2 Logging
- [x] Prerequisite: [Task 0](00-external-dependencies.md) has added `external/spdlog-1.17.0` to the build (compiled static library, `SPDLOG_USE_STD_FORMAT=ON`) and linked `spdlog::spdlog` `PUBLIC` from `AVIS_LIB`.
- [x] Because spdlog uses `std::format`, log calls use `std::format` syntax and custom types need a `std::formatter` (like the one for `avis::error` in 1.1), not an fmt formatter.
- [x] Create `include/avis/log.h` with:
  - `enum class avis::log_level { trace, debug, info, warn, error, critical, off }` in namespace `avis`, because `logging_configuration` uses it unqualified there ([14 §3](../design/14-library-runtime.md#3-configuration));
  - in namespace `avis::log`:
    - `std::shared_ptr<spdlog::logger> get(std::string_view name)`: returns the named logger, or a **null logger** (no sinks) when logging hasn't been initialised ([14 §5](../design/14-library-runtime.md#5-static-access-rules));
    - internal `initialise(const logging_configuration&)` / `shutdown()` used only by the runtime (declare them in a `detail` namespace or a private header under `lib/avis/`). `logging_configuration` itself is defined with the runtime (task 4.3); define it in `log.h` if that keeps the include graph simpler.
- [x] Implement the sinks needed now: MSVC debug output (`msvc_sink_mt`), optional console (`stdout_color_sink_mt`), optional rotating file (5 × 10 MiB, [13 §7](../design/13-build-and-quality.md#7-logging)). The UI ring-buffer and Zenoh sinks come later with their applications.
- [x] Pattern: `[%Y-%m-%d %H:%M:%S.%e] [%n] [%l] [t:%t] %v`. `%t` prints the numeric thread id; to show the pool thread name (`avis-io-0`), add a custom `spdlog::custom_flag_formatter` that reads a thread-local name set by the thread pool wrapper (task 2.3).
- [x] Pre-register the component loggers from [13 §7](../design/13-build-and-quality.md#7-logging) (`core`, `io`, …) and apply `default_level` and the per-logger `levels` map.
- [x] `get()` must be safe to call concurrently with `initialise()` / `shutdown()` (static destructors and early startup may log, [14 §5](../design/14-library-runtime.md#5-static-access-rules)). Guard the registry with a mutex or an atomic shared pointer.

**Tests** (`tests/test_log.cpp`): `get()` before initialisation returns a usable null logger; per-logger levels apply; a test sink (`spdlog::sinks::ringbuffer_sink_mt`) captures messages. Expose a test hook to attach such a sink, the thread pool's `join_warning` test needs it (task 2.6).

## 1.3 Test helpers
- [x] `tests/helpers/thread_count.h`: `std::size_t current_process_thread_count()` using `CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)`. Used for the "no leaked threads" checks ([06 §6](../design/06-thread-pool.md#6-testing), [14 §6](../design/14-library-runtime.md#6-testing)).
- [x] `tests/helpers/log_capture.h`: RAII object that attaches a ring-buffer sink to a named logger and exposes the captured lines.

## As implemented
- **Error codes:** `avis::generic_error_kind { ok, invalid_configuration, pool_exhausted, pool_stopping, cancelled, already_exists, logic_error, unhandled_exception }` in `error.h`, with `make_error(generic_error_kind, std::string message)` and `to_string(generic_error_kind)`. `unhandled_exception` was added for exceptions converted by `make_error(std::exception_ptr)`; a null `exception_ptr` gives `logic_error`.
- **`logging_configuration`** is defined in `log.h`, in namespace `avis`; `runtime_configuration.h` (task 4.2) includes it. `levels` is a `std::map<std::string, log_level, std::less<>>`, so it can be looked up by `std::string_view`.
- **Null logger:** `get(name)` always returns a registered logger. Before `initialise` and after `shutdown` it has no output sinks and its level is `off`, so messages are dropped. Each logger writes to its own `dist_sink_mt`, which holds the shared output sinks. Loggers aren't registered with spdlog's global registry. The registry is intentionally never destroyed, so static destructors can still log.
- **`detail::initialise`** returns `result<void>`: `already_exists` when logging is already initialised, `error_domain::io` when a sink can't be created (for example an unusable log file path). Nothing changes on failure.
- **Thread name:** `avis::log::set_thread_name(name)` / `thread_name()` keep a thread-local name. The pattern is `[%Y-%m-%d %H:%M:%S.%e] [%n] [%l] [t:%N] %v`, where the custom flag `%N` prints that name, or the numeric thread id if none is set. The thread pool calls `set_thread_name("avis-<name>")` (task 2.3).
- **Test hooks:** `detail::attach_sink(logger, sink)` / `detail::detach_sink(logger, sink)`. While a sink is attached, the logger logs every level, whether or not logging is initialised. `tests/helpers/log_capture.h` wraps them and captures `"[<level>] <message>"` lines (spdlog prints `warn` as `warning`).
- **Extra test:** `tests/test_helpers.cpp` checks `current_process_thread_count()`. Note that `tlhelp32.h` must come after `windows.h`; clang-format is switched off around those two includes.

## Acceptance criteria
- [x] `AVIS_LIB` builds with spdlog linked; `avis_tests` passes (Debug).
- [x] No other code changes behaviour yet.

## Open points
- None. The generic error codes are decided (see *As implemented*).
