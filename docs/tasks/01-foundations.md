# Task 1 — Foundations: error type, logging, test helpers

| | |
|---|---|
| Design | [01 §6.2](../design/01-system-architecture.md#62-error-type), [13 §7](../design/13-build-and-quality.md#7-logging), [14 §5](../design/14-library-runtime.md#5-static-access-rules) |
| New headers | `include/avis/error.h`, `include/avis/log.h` |
| New sources | `lib/avis/error.cpp`, `lib/avis/log.cpp` |
| Dependency | spdlog 1.17.0 (`external/spdlog-1.17.0`, integrated by [Task 0](00-external-dependencies.md)) |

The thread pool, task system and runtime all report failures through `avis::error` and log through `avis::log`. Neither exists yet, so they come first.

## 1.1 Error type
- [ ] Create `include/avis/error.h` with `avis::error_domain`, `avis::error` and `avis::result<T> = std::expected<T, error>` exactly as in [01 §6.2](../design/01-system-architecture.md#62-error-type).
- [ ] Add a `std::formatter<avis::error>` that prints `"[<domain>:<code>] <message> (<file>:<line>)"`. Add a `to_string(error_domain)`.
- [ ] Add `avis::runtime_error : std::runtime_error` carrying an `avis::error`, for the unrecoverable paths (`runtime` construction failure, [14 §2](../design/14-library-runtime.md#2-api)).
- [ ] Add `avis::error make_error(std::exception_ptr, std::source_location = current())`: rethrows and catches `avis::runtime_error` (keeps its error), `std::system_error` (keeps the code), `std::exception` (`what()`), and `...` (`"unknown exception"`). The thread pool ([06 §3.3](../design/06-thread-pool.md#33-body-contract)) and the task system ([15 §4.4](../design/15-task-system.md#44-error-type)) both use it.
- [ ] Define the generic error codes the thread pool needs (see Open points): at least `pool_exhausted`, `pool_stopping`, `invalid_configuration`, `cancelled`.

**Tests** (`tests/test_error.cpp`): formatting, `source_location` captured at the call site, `make_error` for each exception category.

## 1.2 Logging
- [ ] Prerequisite: [Task 0](00-external-dependencies.md) has added `external/spdlog-1.17.0` to the build (compiled static library, `SPDLOG_USE_STD_FORMAT=ON`) and linked `spdlog::spdlog` `PUBLIC` from `AVIS_LIB`.
- [ ] Because spdlog uses `std::format`, log calls use `std::format` syntax and custom types need a `std::formatter` (like the one for `avis::error` in 1.1), not an fmt formatter.
- [ ] Create `include/avis/log.h` with:
  - `enum class avis::log_level { trace, debug, info, warn, error, critical, off }` in namespace `avis`, because `logging_configuration` uses it unqualified there ([14 §3](../design/14-library-runtime.md#3-configuration));
  - in namespace `avis::log`:
    - `std::shared_ptr<spdlog::logger> get(std::string_view name)`: returns the named logger, or a **null logger** (no sinks) when logging hasn't been initialised ([14 §5](../design/14-library-runtime.md#5-static-access-rules));
    - internal `initialise(const logging_configuration&)` / `shutdown()` used only by the runtime (declare them in a `detail` namespace or a private header under `lib/avis/`). `logging_configuration` itself is defined with the runtime (task 4.3); define it in `log.h` if that keeps the include graph simpler.
- [ ] Implement the sinks needed now: MSVC debug output (`msvc_sink_mt`), optional console (`stdout_color_sink_mt`), optional rotating file (5 × 10 MiB, [13 §7](../design/13-build-and-quality.md#7-logging)). The UI ring-buffer and Zenoh sinks come later with their applications.
- [ ] Pattern: `[%Y-%m-%d %H:%M:%S.%e] [%n] [%l] [t:%t] %v`. `%t` prints the numeric thread id; to show the pool thread name (`avis-io-0`), add a custom `spdlog::custom_flag_formatter` that reads a thread-local name set by the thread pool wrapper (task 2.3).
- [ ] Pre-register the component loggers from [13 §7](../design/13-build-and-quality.md#7-logging) (`core`, `io`, …) and apply `default_level` and the per-logger `levels` map.
- [ ] `get()` must be safe to call concurrently with `initialise()` / `shutdown()` (static destructors and early startup may log, [14 §5](../design/14-library-runtime.md#5-static-access-rules)). Guard the registry with a mutex or an atomic shared pointer.

**Tests** (`tests/test_log.cpp`): `get()` before initialisation returns a usable null logger; per-logger levels apply; a test sink (`spdlog::sinks::ringbuffer_sink_mt`) captures messages. Expose a test hook to attach such a sink, the thread pool's `join_warning` test needs it (task 2.6).

## 1.3 Test helpers
- [ ] `tests/helpers/thread_count.h`: `std::size_t current_process_thread_count()` using `CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)`. Used for the "no leaked threads" checks ([06 §6](../design/06-thread-pool.md#6-testing), [14 §6](../design/14-library-runtime.md#6-testing)).
- [ ] `tests/helpers/log_capture.h`: RAII object that attaches a ring-buffer sink to a named logger and exposes the captured lines.

## Acceptance criteria
- `AVIS_LIB` builds with spdlog linked; `avis_tests` passes.
- No other code changes behaviour yet.

## Open points
- **Generic error codes.** [06 §3.2](../design/06-thread-pool.md#32-capacity) writes `error{ domain = generic, code = pool_exhausted }` but no enum is specified. Proposal: `enum class generic_errc : std::int32_t { ok, invalid_configuration, pool_exhausted, pool_stopping, cancelled, already_exists, logic_error }` in `error.h`, plus `make_error(generic_errc, std::string message)`.
