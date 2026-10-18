# Task 4 — Library runtime (and the pool-hosted IO context)

| | |
|---|---|
| Design | [14 — Library Runtime](../design/14-library-runtime.md), [07 §2–3](../design/07-io-layer.md#2-components), decisions D-11, D-12, A-22, A-33, A-36 ([99](../design/99-decisions-and-open-questions.md)) |
| Namespace | `avis` (runtime), `avis::io` (IO context) |
| New headers | `include/avis/runtime.h`, `include/avis/runtime_configuration.h`, `include/avis/io/io_context.h` |
| New sources | `lib/avis/runtime.cpp`, `lib/avis/io/io_context.cpp` |
| Removed | `include/avis/io/io_executor.h`, `lib/avis/io/io_executor.cpp` (replaced by `io_context`) |
| Tests | `tests/test_io_context.cpp`, `tests/test_runtime.cpp`, `tests/helpers/runtime_fixture.h`; extend `tests/test_asio_senders_receivers.cpp` |
| Prerequisites | [Task 0](00-external-dependencies.md) (yaml-cpp), [Task 1](01-foundations.md), [Task 2](02-thread-pool.md), [Task 3](03-task-system.md) |

The runtime creates, configures, owns and exposes the process-wide services: logging, the default thread pool, the default IO context and the default task system. It defines none of them ([14 §1](../design/14-library-runtime.md#1-purpose)).

## 4.1 Pool-hosted `io_context`
The runtime's construction step 3 needs an IO context that runs on the pool. Today `avis::io::io_executor` spawns its own `std::jthread`s and its destructor doesn't join them ([99](../design/99-decisions-and-open-questions.md), "existing code diverges"). Replace it; the rest of the IO layer (doc 07: `file`, `read_file`, `mapped_file`, `io/error.h`) is out of scope for this task.

- [ ] `include/avis/io/io_context.h`: `class io_context` with
  - `io_context(threading::thread_pool& pool, std::size_t threads)`: creates the `asio::io_context` with a concurrency hint of `threads`, a work guard, and starts `threads` bodies `pool.start("io-<n>", …)`. Returns errors from `start` by throwing `avis::runtime_error` (startup misconfiguration);
  - each body: register `std::stop_callback{ token, [&]{ context.stop(); } }`, then `context.run()`;
  - `scheduler get_scheduler()` (the existing `avis::io::scheduler` from `asio_scheduler.h`), `asio::io_context& native()`;
  - `void request_stop() noexcept`: release the work guard, `context.stop()`;
  - `~io_context()`: `request_stop()`, then destroy the `thread_handle`s (join).
- [ ] Cancelling outstanding operations on shutdown ([14 §4.2](../design/14-library-runtime.md#42-destruction) step 2): keep an intrusive list of live IO operation states registered by `use_sender` operations, or rely on `asio::io_context` destruction to abandon handlers. Proposal for now: release the guard, let in-flight operations complete, then `stop()`; full cancellation lands with `file::close()` in the IO layer task.
- [ ] Port every user of `io_executor` (`grep -rn io_executor`; today `apps/driver/sensor_injector.h` and a reference in `include/avis/io/asio_scheduler.h`). Remove `io_executor.*` and update `lib/avis/CMakeLists.txt`.
- [ ] Debug assert in a `sync_wait_result` helper (or the `avis::threading::sync_wait` wrapper from task 3.10) that it isn't called on an IO thread: keep a `thread_local const io_context* current_io_context_` set inside the bodies ([07 §7](../design/07-io-layer.md#7-testing)).

**Tests** (`tests/test_io_context.cpp`, plus the extension asked for in [07 §7](../design/07-io-layer.md#7-testing)):
- [ ] `schedule(io.get_scheduler())` completes on a pool thread named `avis-io-0` (`thread_pool::current_index()` has a value).
- [ ] A timer (`asio::steady_timer` + `use_sender`) completes through the pool-hosted context.
- [ ] Destroying the `io_context` stops the bodies through their stop token; `pool.running()` returns to 0.
- [ ] `pool.request_stop()` alone (backstop path) also stops the IO bodies.

## 4.2 Configuration types
- [ ] `include/avis/runtime_configuration.h`: `io_configuration` and `runtime_configuration` as in [14 §3](../design/14-library-runtime.md#3-configuration). `logging_configuration` already exists in `avis/log.h` (task 1), so include that. It includes `thread_pool.h` and `task_system.h` for their configuration structs. (If including `task_system.h` pulls stdexec into too many translation units, move `task_system_configuration` to its own small header `task_system_configuration.h`.)
- [ ] `resolve(runtime_configuration) → result<runtime_configuration>` (internal): replaces `tasks.workers = 0` by `max(1, hardware_concurrency − 2)`, computes `thread_pool.capacity = io.threads + tasks.workers + application_threads + 2` when it is 0, and rejects an explicit capacity below `io.threads + tasks.workers + application_threads` with `invalid_configuration` ([14 §3.1](../design/14-library-runtime.md#31-sizing-the-default-pool)). Also reject `io.threads == 0`.
- [ ] YAML mapping ([14 §3.2](../design/14-library-runtime.md#32-mapping-from-application-configuration)): yaml-cpp 0.9.0 is in the build after [Task 0](00-external-dependencies.md). Both applications read the same `runtime:` and `logging:` sections, so add one library function `result<runtime_configuration> parse_runtime_configuration(std::string_view yaml_text)` in `lib/avis/runtime_configuration.cpp`. It reads only those two sections, ignores the rest of the file, and reports errors as `error_domain::parse` with `line:column` and the key path (`runtime.task_workers`), like scenario parsing ([04](../design/04-scenario-and-dataset.md)). The signature takes text, not a `YAML::Node`, so yaml-cpp stays a `PRIVATE` dependency of `AVIS_LIB` ([13 §2.1](../design/13-build-and-quality.md#21-library-targets)). The applications' `make_runtime_configuration` then reads the file and calls it.
- [ ] Tests: the example from [14 §3.2](../design/14-library-runtime.md#32-mapping-from-application-configuration) and the `driver.yaml` example from [01 §7](../design/01-system-architecture.md#7-configuration) parse into the expected values; missing sections give the defaults; a wrong type (`io_threads: many`) and an unknown log level give `parse` errors with the key path.

## 4.3 `avis::runtime`
- [ ] `include/avis/runtime.h`: the class from [14 §2](../design/14-library-runtime.md#2-api). Keep the services as private members of a pimpl (`std::unique_ptr<state>`) so the header includes only forward declarations plus `runtime_configuration.h`.
- [ ] Single instance: a file-static `std::atomic<runtime*> instance_` and a separate `std::atomic<bool> constructing_` flag. The constructor claims `constructing_` with `exchange(true)` first and throws `avis::runtime_error` if it was already set (concurrent second construction). The destructor clears it last, so construct → destroy → construct works ([14 §2](../design/14-library-runtime.md#2-api)).
- [ ] Construction, in this order ([14 §4.1](../design/14-library-runtime.md#41-construction)):
  1. resolve the configuration (§4.2), throw on error;
  2. logging (`avis::log::detail::initialise`);
  3. `threading::thread_pool`;
  4. `io::io_context(pool, io.threads)`;
  5. `threading::task_system(pool, tasks)`;
  6. `instance_.store(this, release)`.

  Hold each service in a `std::optional`/`unique_ptr` and build them inside one `try`: on failure, reset the ones already built **in reverse order**, shut logging down, clear `constructing_`, and rethrow as `avis::runtime_error` ([14 §4.1](../design/14-library-runtime.md#41-construction)).
- [ ] Destruction, in this order ([14 §4.2](../design/14-library-runtime.md#42-destruction)), `noexcept`, every step wrapped in `try/catch` that logs:
  1. task system: `request_stop()` and destroy (drains and joins);
  2. IO context: stop and destroy;
  3. `instance_.store(nullptr)` (only now, so draining tasks and IO completions can still use the accessors);
  4. thread pool: destroy (backstop with `join_warning` for application threads that weren't stopped);
  5. logging: flush and shut down;
  6. clear `constructing_`.
- [ ] Static accessors ([14 §5](../design/14-library-runtime.md#5-static-access-rules)): load `instance_` (acquire); if null, `assert` in debug builds and throw `std::logic_error` in release; otherwise return the service. `alive()` returns `instance_ != nullptr`. `configuration()` returns the **resolved** configuration.

## 4.4 Test fixture and tests
- [ ] `tests/helpers/runtime_fixture.h`: a Catch2 fixture that builds a `runtime` with a small configuration (`io.threads = 1`, `tasks.workers = 2`, `application_threads = 1`, console and file logging off) ([14 §2](../design/14-library-runtime.md#2-api)).
- [ ] Because the debug-build accessors `assert`, the "no runtime alive" cases need the release behaviour: build the accessor check as `AVIS_ASSERT_OR_THROW` with a test hook (or a `#define` in the test target) that makes it throw in tests, so the cases in [14 §5](../design/14-library-runtime.md#5-static-access-rules) are testable in Debug.

`tests/test_runtime.cpp`, mapping [14 §6](../design/14-library-runtime.md#6-testing):
- [ ] Construct, destroy, construct again in one process.
- [ ] A second construction while one is alive throws; a construction on another thread racing with the first one: exactly one succeeds.
- [ ] Rollback on failure at each step, by fault injection through configuration: too-small `thread_pool.capacity`, a log `file` path in a directory that can't be created, `io.threads = 0`. After each failure `alive()` is false, no threads remain (thread-count helper) and a new construction succeeds.
- [ ] Static accessors before, during and after the lifetime; `avis::log::get("core")` before and after is a working null logger.
- [ ] Capacity computed as `io + workers + application_threads + 2` when left at 0; `tasks.workers = 0` resolves to `max(1, hardware_concurrency − 2)`.
- [ ] Shutdown order: a task that issues an IO operation (`schedule(io) | then(…)`) while the runtime is being destroyed completes with a value or `set_stopped` before the IO context stops (no hang, no use-after-free under ASan).
- [ ] "Forgotten" application thread: start a body on `runtime::thread_pool()` and leak its handle into a scope that ends after the runtime; the runtime's destructor still returns, the `join_warning` doesn't fire for a body that honours its token, and no threads remain.
- [ ] The example in [14 §2](../design/14-library-runtime.md#2-api) (runtime at the top of `main`) as a smoke test.

## 4.5 Adopt in the applications
- [ ] `apps/visualizer/main.cpp` and the Driver's entry point (`apps/driver/` has no `main.cpp` yet; add the runtime when it gets one): create `avis::runtime` at the top of `main` with hard-coded defaults (`application_threads` 1 for the Visualizer, 2 for the Driver, [14 §3.1](../design/14-library-runtime.md#31-sizing-the-default-pool); Visualizer `worker_priority = above_normal`, [15 §7](../design/15-task-system.md#7-configuration)). Then, if the application already loads its YAML file, feed it through `parse_runtime_configuration`; otherwise that step comes with the configuration-loading task.
- [ ] Replace any remaining direct thread creation in the apps with `runtime::thread_pool().start(…)` where it is a long-running body, keeping the `thread_handle` in the owning object.

## Acceptance criteria
- `io_executor` is gone; every IO user runs on the pool-hosted `io_context`.
- All tests above pass in Debug and Release, and under ASan.
- Both applications start and shut down cleanly with the runtime: no `join_warning` in the log on a normal exit.

## Open points
- **IO scope.** This task only provides `io_context` and its scheduler. `file`, `read_file`/`write_file`, `mapped_file`, `io/error.h` and full cancellation on shutdown (doc 07) need their own task document.
