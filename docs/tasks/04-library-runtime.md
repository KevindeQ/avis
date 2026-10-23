# Task 4 — Library runtime (and the pool-hosted IO context)

| | |
|---|---|
| Design | [14 — Library Runtime](../design/14-library-runtime.md), [07 §2–3](../design/07-io-layer.md#2-components), decisions D-11, D-12, D-60, D-63, A-22, A-36 ([99](../design/99-decisions-and-open-questions.md)) |
| Namespace | `avis` (runtime), `avis::io` (IO context) |
| New headers | `include/avis/runtime.h`, `include/avis/runtime_configuration.h`, `include/avis/io/io_context.h`, `include/avis/io/io_configuration.h` |
| New sources | `lib/avis/runtime.cpp`, `lib/avis/io/io_context.cpp` |
| Removed | `include/avis/io/io_executor.h`, `lib/avis/io/io_executor.cpp` (replaced by `io_context`) |
| Tests | `tests/test_io_context.cpp`, `tests/test_runtime.cpp`, `tests/helpers/runtime_fixture.h`; extend `tests/test_asio_senders_receivers.cpp` |
| Prerequisites | [Task 0](00-external-dependencies.md) (yaml-cpp), [Task 1](01-foundations.md), [Task 2](02-thread-pool.md), [Task 3](03-parallel-scheduler.md) (`parallel_backend`) |

> **Status (2026-10-21): done in Debug and ASan**, except the Release run. All 79 test cases pass in Debug and in the AddressSanitizer build (26 of them new or extended here), and 20 random-order Debug runs of the threading, IO and runtime tests found no failure. The Visualizer starts with the runtime and, after a normal window close, exits with code 0 in under 100 ms, so no `join_warning` wait happened. Windows Application Control blocks the Release test binary on this machine, so the Release run is left to you. The Driver has no build target yet, so its one-line port (below) isn't compiled.

The runtime creates, configures, owns and exposes the process-wide services: logging, the default thread pool, the default IO context and the parallel pool (an `exec::static_thread_pool`) behind `stdexec::get_parallel_scheduler()`. It defines none of them ([14 §1](../design/14-library-runtime.md#1-purpose)).

## 4.1 Pool-hosted `io_context`
The runtime's construction step 3 needs an IO context that runs on the pool. Today `avis::io::io_executor` spawns its own `std::jthread`s and its destructor doesn't join them ([99](../design/99-decisions-and-open-questions.md), "existing code diverges"). Replace it; the rest of the IO layer (doc 07: `file`, `read_file`, `mapped_file`, `io/error.h`) is out of scope for this task.

- [x] `include/avis/io/io_context.h`: `class io_context` with
  - `io_context(threading::thread_pool& pool, std::size_t threads)`: creates the `asio::io_context` with a concurrency hint of `threads`, a work guard, and starts `threads` bodies `pool.start("io-<n>", …)`. Returns errors from `start` by throwing `avis::runtime_error` (startup misconfiguration);
  - each body: register `std::stop_callback{ token, [&]{ context.stop(); } }`, then `context.run()`;
  - `scheduler get_scheduler()` (the existing `avis::io::scheduler` from `asio_scheduler.h`), `asio::io_context& native()`;
  - `void request_stop() noexcept`: release the work guard, `context.stop()`;
  - `~io_context()`: `request_stop()`, then destroy the `thread_handle`s (join).
- [x] Cancelling outstanding operations on shutdown ([14 §4.2](../design/14-library-runtime.md#42-destruction) step 2): keep an intrusive list of live IO operation states registered by `use_sender` operations, or rely on `asio::io_context` destruction to abandon handlers. Proposal for now: release the guard, let in-flight operations complete, then `stop()`; full cancellation lands with `file::close()` in the IO layer task.
- [x] Port every user of `io_executor` (`grep -rn io_executor`; today `apps/driver/sensor_injector.h` and a reference in `include/avis/io/asio_scheduler.h`). Remove `io_executor.*` and update `lib/avis/CMakeLists.txt`.
- [x] Debug assert in a `sync_wait_result` helper (or the `avis::threading::sync_wait` wrapper from task 3.4) that it isn't called on an IO thread: keep a `thread_local const io_context* current_io_context_` set inside the bodies ([07 §7](../design/07-io-layer.md#7-testing)).

**Tests** (`tests/test_io_context.cpp`, plus the extension asked for in [07 §7](../design/07-io-layer.md#7-testing)):
- [x] `schedule(io.get_scheduler())` completes on a pool thread named `avis-io-0` (`thread_pool::current_index()` has a value).
- [x] A timer (`asio::steady_timer` + `use_sender`) completes through the pool-hosted context.
- [x] Destroying the `io_context` stops the bodies through their stop token; `pool.running()` returns to 0.
- [x] `pool.request_stop()` alone (backstop path) also stops the IO bodies.

## 4.2 Configuration types
- [x] `include/avis/runtime_configuration.h`: `runtime_configuration` as in [14 §3](../design/14-library-runtime.md#3-configuration). *(Amended 2026-10-22: `io::io_configuration` moved to its own `include/avis/io/io_configuration.h`, next to the IO context, like the other services' configurations.)* `logging_configuration` already exists in `avis/log.h` (task 1), so include that. It includes `thread_pool.h` and `parallel_configuration.h` for their configuration structs; neither includes stdexec (task 3.2), so `runtime.h` stays free of it too.
- [x] `resolve(runtime_configuration) → result<runtime_configuration>` (internal): replaces `parallel.workers = 0` by `max(1, hardware_concurrency − 2)` (a file-private `resolve_worker_count` in `runtime_configuration.cpp`; originally in task 3.2's `parallel_configuration.h`), computes `thread_pool.capacity = io.threads + application_threads + 2` when it is 0, and rejects an explicit capacity below `io.threads + application_threads` with `invalid_configuration`. Parallel workers aren't pool threads and don't count towards the capacity ([14 §3.1](../design/14-library-runtime.md#31-sizing-the-default-pool)). Also reject `io.threads == 0`.
- [x] YAML mapping ([14 §3.2](../design/14-library-runtime.md#32-mapping-from-application-configuration)): yaml-cpp 0.9.0 is in the build after [Task 0](00-external-dependencies.md). Both applications read the same `runtime:` and `logging:` sections, so add one library function `result<runtime_configuration> parse_runtime_configuration(std::string_view yaml_text)` (since 2026-10-22 the static member `runtime_configuration::parse`) in `lib/avis/runtime_configuration.cpp`. It reads only those two sections, ignores the rest of the file, and reports errors as `error_domain::parse` with `line:column` and the key path (`runtime.parallel_workers`), like scenario parsing ([04](../design/04-scenario-and-dataset.md)). The signature takes text, not a `YAML::Node`, so yaml-cpp stays a `PRIVATE` dependency of `AVIS_LIB` ([13 §2.1](../design/13-build-and-quality.md#21-library-targets)). The applications' `make_runtime_configuration` then reads the file and calls it.
- [x] Tests: the example from [14 §3.2](../design/14-library-runtime.md#32-mapping-from-application-configuration) and the `driver.yaml` example from [01 §7](../design/01-system-architecture.md#7-configuration) parse into the expected values; missing sections give the defaults; a wrong type (`io_threads: many`) and an unknown log level give `parse` errors with the key path.

## 4.3 `avis::runtime`
- [x] `include/avis/runtime.h`: the class from [14 §2](../design/14-library-runtime.md#2-api). Keep the services as private members of a pimpl (`std::unique_ptr<state>`) so the header includes only forward declarations plus `runtime_configuration.h`. In particular `exec::static_thread_pool` and `parallel_backend` live in the pimpl, so only `runtime.cpp` includes `exec/static_thread_pool.hpp`. There's no static accessor for the parallel pool: code uses `stdexec::get_parallel_scheduler()` ([14 §1](../design/14-library-runtime.md#1-purpose)).
- [x] Single instance: a file-static `std::atomic<runtime*> instance_` and a separate `std::atomic<bool> constructing_` flag. The constructor claims `constructing_` with `exchange(true)` first and throws `avis::runtime_error` if it was already set (concurrent second construction). The destructor clears it last, so construct → destroy → construct works ([14 §2](../design/14-library-runtime.md#2-api)).
- [x] Construction, in this order ([14 §4.1](../design/14-library-runtime.md#41-construction)):
  1. resolve the configuration (§4.2), throw on error;
  2. logging (`avis::log::detail::initialise`);
  3. `threading::thread_pool`;
  4. `io::io_context(pool, io.threads)`;
  5. parallel pool: `exec::static_thread_pool(workers)`, a `std::shared_ptr<threading::parallel_backend>` for it, then publish it for the backend query, which `runtime.cpp` defines (amended 2026-10-22; originally `threading::install_parallel_backend(backend)`) ([14 §4.1](../design/14-library-runtime.md#41-construction) step 4);
  6. `instance_.store(this, release)`.

  Hold each service in a `std::optional`/`unique_ptr` and build them inside one `try`: on failure, reset the ones already built **in reverse order** (for the parallel pool: unpublish, detach, destroy, as in destruction step 1), shut logging down, clear `constructing_`, and rethrow as `avis::runtime_error` ([14 §4.1](../design/14-library-runtime.md#41-construction)).
- [x] Destruction, in this order ([14 §4.2](../design/14-library-runtime.md#42-destruction)), `noexcept`, every step wrapped in `try/catch` that logs:
  1. parallel pool ([15 §5](../design/15-parallel-scheduler.md#5-lifecycle)): unpublish the backend, `backend->detach()`, release the runtime's `shared_ptr`, destroy the `exec::static_thread_pool` (stops and joins its workers). Work still queued may never run (D-63);
  2. IO context: stop and destroy;
  3. `instance_.store(nullptr)` (only now, so the last parallel work and IO completions can still use the accessors);
  4. thread pool: destroy (backstop with `join_warning` for application threads that weren't stopped);
  5. logging: flush and shut down;
  6. clear `constructing_`.
- [x] Static accessors ([14 §5](../design/14-library-runtime.md#5-static-access-rules)): load `instance_` (acquire); if null, `assert` in debug builds and throw `std::logic_error` in release; otherwise return the service. `alive()` returns `instance_ != nullptr`. `configuration()` returns the **resolved** configuration.

## 4.4 Test fixture and tests
- [x] `tests/helpers/runtime_fixture.h`: a Catch2 fixture that builds a `runtime` with a small configuration (`io.threads = 1`, `parallel.workers = 2`, `application_threads = 1`, console and file logging off). Since 2026-10-22 it's the only way tests get a parallel scheduler: `tests::small_runtime_configuration(workers)` sets the worker count, and task 3's `parallel_backend_scope` is gone. ([14 §2](../design/14-library-runtime.md#2-api)).
- [x] Because the debug-build accessors `assert`, the "no runtime alive" cases need the release behaviour: build the accessor check as `AVIS_ASSERT_OR_THROW` with a test hook (or a `#define` in the test target) that makes it throw in tests, so the cases in [14 §5](../design/14-library-runtime.md#5-static-access-rules) are testable in Debug.

`tests/test_runtime.cpp`, mapping [14 §6](../design/14-library-runtime.md#6-testing):
- [x] Construct, destroy, construct again in one process.
- [x] A second construction while one is alive throws; a construction on another thread racing with the first one: exactly one succeeds.
- [x] Rollback on failure at each step, by fault injection through configuration: too-small `thread_pool.capacity`, a log `file` path in a directory that can't be created, `io.threads = 0`. After each failure `alive()` is false, no threads remain (thread-count helper) and a new construction succeeds.
- [x] Static accessors before, during and after the lifetime; `avis::log::get("core")` before and after is a working null logger.
- [x] `stdexec::get_parallel_scheduler()` throws before construction, runs work on one of the runtime's `parallel.workers` threads during the lifetime, and throws again after destruction ([14 §6](../design/14-library-runtime.md#6-testing)).
- [x] A `parallel_scheduler` copied during the lifetime and kept past destruction completes `schedule()` with `set_stopped()` (the backend was detached) and doesn't crash under ASan.
- [x] Capacity computed as `io + application_threads + 2` when left at 0, independent of `parallel.workers`; `parallel.workers = 0` resolves to `max(1, hardware_concurrency − 2)`.
- [x] Shutdown order: parallel work that issues an IO operation (`schedule(io) | then(…)`) and has finished before the runtime is destroyed completes with a value or `set_stopped`, and the destructor returns (no hang, no use-after-free under ASan). Work still queued at destruction isn't required to complete (D-63), so the test waits for its operation before destroying the runtime.
- [x] "Forgotten" application thread: start a body on `runtime::thread_pool()` and leak its handle into a scope that ends after the runtime; the runtime's destructor still returns, the `join_warning` doesn't fire for a body that honours its token, and no threads remain.
- [x] The example in [14 §2](../design/14-library-runtime.md#2-api) (runtime at the top of `main`) as a smoke test.

## 4.5 Adopt in the applications
- [x] `apps/visualizer/main.cpp` and the Driver's entry point (`apps/driver/` has no `main.cpp` yet; add the runtime when it gets one): create `avis::runtime` at the top of `main` with hard-coded defaults (`application_threads` 1 for the Visualizer, 2 for the Driver, [14 §3.1](../design/14-library-runtime.md#31-sizing-the-default-pool); parallel workers at the default, [15 §7](../design/15-parallel-scheduler.md#7-configuration)). There's no worker priority to set any more (D-62). Then, if the application already loads its YAML file, feed it through `runtime_configuration::parse`; otherwise that step comes with the configuration-loading task.
- [x] Replace any remaining direct thread creation in the apps with `runtime::thread_pool().start(…)` where it is a long-running body, keeping the `thread_handle` in the owning object.

## As implemented
- **`io_context`** (`include/avis/io/io_context.h`): as listed, plus `io_context::current()` (the context whose body runs on the calling thread). `request_stop()` releases the guard and calls `stop()` straight away, so operations still in flight are abandoned rather than allowed to finish; waiting for them could hang shutdown on an operation that never completes. Full cancellation stays with the IO layer task.
- **`sync_wait` on IO threads:** each IO body holds a `threading::detail::forbid_blocking_waits` scope (a small generic marker added to `parallel_helpers.h`, so the threading headers stay free of IO), and `avis::threading::sync_wait` throws `std::logic_error` there, in every build, like it does for parallel work (design 15 §4.3), instead of a debug-only assert. There's no `sync_wait_result` yet; it comes with the IO layer.
- **`io_executor` users:** only `apps/driver/sensor_injector.h` used it, through an unused member, which is gone. `asio_scheduler.h` only names asio's own `any_io_executor`.
- **Configuration:** `runtime_configuration.h` declares the parser and the resolver, since 2026-10-22 as static members `runtime_configuration::parse` and `runtime_configuration::resolve` (originally the free functions `parse_runtime_configuration` and `detail::resolve`). `runtime_configuration::thread_pool` defaults to capacity 0 (computed), unlike a standalone `thread_pool_configuration`. Besides the keys of 14 §3.2 (`io_threads`, `parallel_workers`, `thread_pool_capacity`, `level`, `file`), the parser reads `logging.console`, `logging.debugger_output` and `logging.levels` (logger → level), so every `logging_configuration` field can be set. Unknown keys are ignored. Parse errors use code 0, since `error_domain::parse` has no codes yet.
- **Runtime:** it defines stdexec's backend query in `runtime.cpp` and publishes its backend there (since 2026-10-22; before, it called `install_parallel_backend` after checking that no other backend was installed). The static accessors' debug assert can be turned into a throw with `avis::detail::make_runtime_accessors_throw` (wrapped by `tests::throwing_runtime_accessors`). `runtime::io_context()` also throws during the short shutdown window after the IO context is gone but before the runtime is unpublished.
- **Rollback tests:** too-small capacity and `io.threads = 0` both fail in `resolve`, and a log file under a regular file fails at logging. A failure after the thread pool, IO and parallel pool have started is no longer tested: it used to be produced by constructing the runtime while a `parallel_backend_scope` was alive, and with that helper gone, no configuration reaches that path.
- **Tests:** `tests/test_io_context.cpp`, `tests/test_runtime.cpp` and, for §4.2, a separate `tests/test_runtime_configuration.cpp`; one test added to `tests/test_asio_senders_receivers.cpp`. The YAML smoke test in `test_external_dependencies.cpp` now uses the `parallel_workers` key.
- **Visualizer:** `main` creates the runtime first, inside an `std::optional`, and returns 1 with the error on stderr if it can't start. The configuration is hard-coded (`application_threads = 1`) until the Visualizer loads `visualizer.yaml`.

## Acceptance criteria
- `io_executor` is gone; every IO user runs on the pool-hosted `io_context`.
- `stdexec::get_parallel_scheduler()` works exactly while a runtime is alive, and only `runtime.cpp` and `parallel_backend.*` include `exec/static_thread_pool.hpp`.
- All tests above pass in Debug and Release, and under ASan.
- Both applications start and shut down cleanly with the runtime: no `join_warning` in the log on a normal exit.

## Open points
- **IO scope.** This task only provides `io_context` and its scheduler. `file`, `read_file`/`write_file`, `mapped_file`, `io/error.h` and full cancellation on shutdown (doc 07) need their own task document.
