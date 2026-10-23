# Task 3 — Parallel scheduler

| | |
|---|---|
| Design | [15 — Parallel Scheduler](../design/15-parallel-scheduler.md), decisions D-60 to D-63, A-31, A-35, A-36 ([99](../design/99-decisions-and-open-questions.md)) |
| Namespace | `avis::threading` |
| New headers | `include/avis/threading/parallel_configuration.h`, `include/avis/threading/parallel_backend.h`, `include/avis/threading/parallel_helpers.h` |
| New sources | `lib/avis/threading/parallel_backend.cpp` |
| Removed | `include/avis/threading/task.h`, `task_system.h`, `task_scheduler.h`, `work_stealing_deque.h`, `task_inbox.h`, `lib/avis/threading/task_system.cpp`, and their tests `tests/test_task_inbox.cpp`, `test_task_scheduler.cpp`, `test_task_system.cpp`, `test_task_system_stress.cpp`, `test_work_stealing_deque.cpp` |
| Tests | `tests/test_parallel_backend.cpp` (with the runtime fixture of task 4) |
| Prerequisites | [Task 1](01-foundations.md) (`avis::error`, `make_error`, `avis::log`). Independent of [Task 2](02-thread-pool.md): the parallel workers aren't pool threads. |
| Reference | `external/stdexec/include/stdexec/__detail/__parallel_scheduler_backend.hpp` (the interface), `__parallel_scheduler_default_impl.hpp` (stdexec's own backend over `exec::static_thread_pool`, the model for §3.2), `__parallel_scheduler_replacement_api.hpp` |

> **Status (2026-10-21): done in Debug and ASan**, except the Release run and the benchmark numbers. All 54 test cases pass in Debug and in the AddressSanitizer build, the `[stress]` test passes in both, and 20 random-order Debug runs of the `[parallel]` tests found no failure. Windows Application Control blocked `out/build/windows-release/bin/avis_tests.exe` from starting, so the Release run (and with it meaningful benchmark numbers) is left to you; the benchmark itself runs to completion in Debug.

> **Amended (2026-10-22):** the backend query and the published backend moved into `lib/avis/runtime.cpp`, and `install_parallel_backend` and `tests/helpers/parallel_backend_scope.h` were removed, so only the runtime can provide the parallel scheduler's backend. The tests here use the runtime fixture instead (task 4). §3.1's `#error` guard moved with the query. Steps below are updated to match.

Parallel work goes through the standard `stdexec::get_parallel_scheduler()`. Avis supplies its backend: a thin adapter over an `exec::static_thread_pool` it doesn't own. The runtime (task 4) owns the pool and publishes the adapter.

The earlier custom task system (work-stealing deque, inboxes, worker loop, scheduler, `bulk`) is replaced and deleted at the end (§3.6). Its git history keeps the implementation and its benchmarks.

## 3.1 Build: Avis is the only backend definition
stdexec declares `stdexec::parallel_scheduler_replacement::query_parallel_scheduler_backend()` and calls it from every `get_parallel_scheduler()`. Its default definition is compiled only in two opt-in cases, and Avis must use neither ([15 §3.1](../design/15-parallel-scheduler.md#31-replacing-the-backend-at-link-time)):

- [x] In `external/CMakeLists.txt`, before `add_subdirectory(stdexec …)`, set `STDEXEC_BUILD_PARALLEL_SCHEDULER` to `OFF` explicitly (it already defaults to `OFF`; the explicit setting documents why), next to the other per-library option blocks.
- [x] In the file that defines the backend query (now `runtime.cpp`), fail the build if stdexec's header-only default is switched on:
  ```cpp
  #if defined(STDEXEC_PARALLEL_SCHEDULER_HEADER_ONLY) || defined(STDEXEC_SYSTEM_CONTEXT_HEADER_ONLY)
  #    error "AVIS_LIB defines the parallel scheduler backend (docs/design/15-parallel-scheduler.md §3.1)"
  #endif
  ```
- [x] Don't call or define `set_parallel_scheduler_backend` (deprecated run-time replacement; with no default compiled it has no definition at all).

## 3.2 `parallel_backend`
`include/avis/threading/parallel_backend.h`, as in [15 §3.2](../design/15-parallel-scheduler.md#32-parallel_backend). Only this header, its source and the runtime include `exec/static_thread_pool.hpp`.

- [x] `parallel_configuration.h`: `struct parallel_configuration { std::size_t workers = 0; };` with no stdexec include, so `runtime_configuration.h` (task 4) can include it without pulling in stdexec. Add `std::uint32_t resolve_worker_count(std::size_t workers) noexcept` (`0` → `max(1, hardware_concurrency − 2)`), used by the runtime and the tests. *(Moved 2026-10-22 into `runtime_configuration.cpp` as a file-private helper of `runtime_configuration::resolve` (then `detail::resolve`), its only user.)*
- [x] `class parallel_backend final : public stdexec::parallel_scheduler_replacement::parallel_scheduler_backend`, constructed from `exec::static_thread_pool&`. Store the pool pointer, the pool's scheduler and `pool.available_parallelism()`.
- [x] **Own operation wrapper.** stdexec's default backend uses its internal `__detail::__proxy_receiver`; Avis writes its own instead, so no stdexec `__detail` name is used (the old `task_scheduler.h` relied on internals, which is what made it fragile):
  - a receiver that forwards `set_value`, `set_error(std::exception_ptr)` and `set_stopped` to the `receiver_proxy`, and whose environment answers `get_stop_token` with `proxy.try_query<stdexec::inplace_stop_token>(stdexec::get_stop_token)` (the never-stopping token when absent);
  - an operation that holds `stdexec::connect_result_t<Sender, receiver>`, is placement-constructed in the `scratch` span when it fits (after aligning it) and heap-allocated otherwise, and destroys itself when it completes (a flag tells whether to `delete` it).
- [x] `schedule(receiver_proxy&, scratch)`: connect `stdexec::schedule(pool_scheduler)` to the wrapper and start it. If building the operation throws (heap allocation), call `set_error(std::current_exception())`.
- [x] `schedule_bulk_chunked(count, proxy, scratch)`: same chunking as stdexec's default: `chunk = (parallelism > 0 && count > 3 × parallelism) ? count / parallelism / 2 : 1`, `chunks = ⌈count / chunk⌉`; run `stdexec::bulk(stdexec::schedule(pool_scheduler), stdexec::par, chunks, f)` where `f(index)` calls `proxy.execute(index × chunk, min((index + 1) × chunk, count))`. The pool's own `bulk` customisation spreads the chunks over the workers. Completion goes to `proxy` (set_value / set_error / set_stopped).
- [x] `schedule_bulk_unchunked(count, proxy, scratch)`: `bulk` over `count` with `f(index)` calling `proxy.execute(index, index + 1)`.
- [x] `count == 0`: complete `proxy.set_value()` without touching the pool.
- [x] **Parallel-work marker** ([15 §3.2](../design/15-parallel-scheduler.md#32-parallel_backend)): a `thread_local` depth counter (since 2026-10-22 in `parallel_helpers.cpp`, with the other definitions of `parallel_helpers.h`; the backend's `detail::parallel_work_scope` is declared in `parallel_helpers.h`), incremented by an RAII guard around every completion the wrapper delivers on a worker (`set_value`, `set_error`, `set_stopped`) and around every `proxy.execute` call. Expose it as `bool avis::threading::detail::in_parallel_work() noexcept`, declared in `parallel_helpers.h`.
- [x] **Detach** ([15 §5](../design/15-parallel-scheduler.md#5-lifecycle)): `std::atomic<bool> detached_`. `detach()` sets it (release). Every `schedule*` checks it first (acquire) and, when set, completes `proxy.set_stopped()` inline without touching the pool. A call racing with `detach()` may still reach the pool; that's the accepted "scheduling during runtime destruction is a programming error" case (D-63), so no further synchronisation.

## 3.3 The backend query (now in the runtime)
- [x] *(Replaced: the runtime publishes its backend in a file-level atomic in `runtime.cpp`; there's no install function.)* Originally `void install_parallel_backend(std::shared_ptr<parallel_backend> backend) noexcept` in `parallel_backend.h`. Store it in a file-static `std::atomic<std::shared_ptr<parallel_backend>>` (C++20; MSVC implements it with a lock, which is fine at the call rate of `get_parallel_scheduler()`). Installing a non-null backend while another one is installed is a programming error: `assert` in debug builds, and log an error and keep the existing one in release. `nullptr` uninstalls.
- [x] Define, in `runtime.cpp` (originally `parallel_backend.cpp`):
  ```cpp
  namespace stdexec::parallel_scheduler_replacement
  {
      auto query_parallel_scheduler_backend() -> std::shared_ptr<parallel_scheduler_backend>
      {
          return published_backend.load();   // the live runtime's backend, nullptr when none is alive
      }
  }
  ```
  The vendored declaration lives in `STDEXEC::parallel_scheduler_replacement`, where `STDEXEC` is `stdexec` unless `STDEXEC_NAMESPACE` is set. Write the definition with the same `STDEXEC::` spelling, so it keeps matching if that macro is ever set.
- [x] With nothing installed, `stdexec::get_parallel_scheduler()` throws `std::runtime_error` (stdexec's own check). Don't add another one.

## 3.4 `parallel_helpers.h`: error conversion and waiting
Move the two helpers out of the old `task_scheduler.h` into `include/avis/threading/parallel_helpers.h` ([15 §4.2–4.3](../design/15-parallel-scheduler.md#42-error-type)):

- [x] `avis::threading::into_internal_error()`: unchanged sender adaptor that maps `std::exception_ptr` (through `make_error`) and `std::error_code` to `avis::error`.
- [x] `avis::threading::sync_wait(sender)`: throws `std::logic_error` when `detail::in_parallel_work()` is true (in every build), otherwise forwards to `stdexec::sync_wait`. The message names the alternative (`let_value` / `when_all`).
- [x] `parallel_helpers.h` includes `stdexec/execution.hpp` but not `exec/static_thread_pool.hpp`.

## 3.5 Tests
*(Replaced by the runtime fixture: each test constructs an `avis::runtime` with `tests::small_runtime_configuration(workers)`.)* Originally `tests/helpers/parallel_backend_scope.h`, an RAII helper that created an `exec::static_thread_pool` and a `parallel_backend` for it and installed it. Tests run one at a time, so only one scope (or, after task 4, one runtime) is alive at once.

`tests/test_parallel_backend.cpp`, mapping [15 §8](../design/15-parallel-scheduler.md#8-testing):
- [x] Link-time replacement: inside a scope, `stdexec::sync_wait(stdexec::schedule(stdexec::get_parallel_scheduler()) | then(…))` runs on a thread that isn't the test's thread, and the set of thread ids seen over many runs has at most `workers` entries. Outside any scope, `get_parallel_scheduler()` throws `std::runtime_error`.
- [x] `bulk` coverage: `bulk(par, n, f)` on the parallel scheduler calls `f` exactly once per index (per-index atomic counter) for `n ∈ {0, 1, 7, 3 × workers, 3 × workers + 1, 1M}`, which crosses the chunking threshold; `bulk_chunked` ranges are contiguous and cover `[0, n)` without overlap.
- [x] `bulk` errors: an exception thrown for one index arrives as a single `set_error(std::exception_ptr)`; with `into_internal_error()` it's an `avis::error` carrying the exception's message.
- [x] Stop: a receiver whose `inplace_stop_source` is already stopped completes with `set_stopped()` (stdexec's front end handles it; the test pins the behaviour).
- [x] Detach: keep a `parallel_scheduler` copy, `detach()` the backend, then `sync_wait` on `schedule(copy)` returns the stopped result (an empty `optional`), and the pool isn't touched (the pool can already be destroyed in the test).
- [x] `avis::threading::sync_wait` inside `then` on the parallel scheduler throws `std::logic_error` (the outer `sync_wait` sees it as an error); on the test thread it works.
- [x] `continues_on(stdexec::get_parallel_scheduler())` from an `io::scheduler` sender on a test `asio::io_context` (existing `asio_scheduler.h`): the continuation runs on a worker. This is the IO → decode example of [15 §4.5](../design/15-parallel-scheduler.md#45-examples).
- [x] Shutdown: creating and destroying a scope with no outstanding work leaves the process thread count unchanged (thread-count helper).
- [x] `static_assert(stdexec::scheduler<stdexec::parallel_scheduler>)` and that the parallel scheduler reports `forward_progress_guarantee::parallel`.
- [x] Stress (`[stress]`, out of the default run): 10 s of mixed `schedule`, `bulk` and nested `continues_on` from 4 external threads, under ASan. *(Amended 2026-10-22: the stress test no longer detaches mid-run, because detach is now internal to runtime shutdown, and shutting down with work queued could hang the test (D-63). The runtime outlives every submission, and every operation must complete with a value.)*
- [ ] Benchmark (`[benchmark]`): `spawn(schedule() | then)` and `bulk` over 1M indices through `get_parallel_scheduler()` against the same work on `pool.get_scheduler()` directly, to measure the type-erasure cost. Record the numbers under *As implemented*.

## 3.6 Remove the task system
- [x] Delete the files listed under *Removed* and their entries in `lib/avis/CMakeLists.txt` and `tests/CMakeLists.txt`.
- [x] `grep -rn "task_system\|task_scheduler\|work_stealing_deque\|task_inbox\|threading/task.h" lib include apps tests tools` finds nothing.
- ~~`tools/checks/check-threading-includes.ps1` ([13 §9](../design/13-build-and-quality.md#9-coding-standards), [06 §5](../design/06-thread-pool.md#5-relation-to-the-parallel-scheduler)): the forbidden includes become `parallel_backend.h`, `parallel_helpers.h`, `parallel_configuration.h` and any `stdexec/` or `exec/` header; update the synopsis, the failure message and the comment above the CTest registration in `tests/CMakeLists.txt` ("must not depend on the parallel scheduler").~~ Removed (2026-10-23): the include check no longer exists.

## As implemented
- **Headers:** `parallel_configuration.h` (no stdexec), `parallel_backend.h` (includes `exec/static_thread_pool.hpp`), `parallel_helpers.h` (`into_internal_error`, `sync_wait`, and `detail::in_parallel_work()`, which the tests also use). `resolve_worker_count` was defined in `parallel_backend.cpp`; it now lives in `runtime_configuration.cpp`.
- **Operation wrapper:** `proxy_receiver` copies the proxy reference, destroys the operation, then completes the proxy inside a parallel-work scope, the same order as stdexec's own backend (completing the proxy may end the scratch storage). Its environment forwards the front end's `inplace_stop_token`. No stdexec `__detail` name is used.
- **Marker:** a `thread_local` depth counter, raised around every completion delivered on a worker and around every `execute` call. Code that continues inline from a completion (`then` after `continues_on`) therefore sees it too.
- **Publication:** `std::atomic<std::shared_ptr<parallel_backend>>` in `runtime.cpp` (originally an install function with an assert against a second install, removed 2026-10-22). Only one runtime can be alive, so a second backend can't be published.
- **Build guard:** `STDEXEC_BUILD_PARALLEL_SCHEDULER` is forced `OFF` in `external/CMakeLists.txt`, and `runtime.cpp` (originally `parallel_backend.cpp`) has the `#error` for the header-only macros. `STDEXEC_ATTRIBUTE(weak)` expands to nothing on MSVC, so the vendored declaration is a plain one and Avis's definition is the only one.
- **Fire-and-forget in tests:** `stdexec::spawn` rejects senders that can complete with an error, and `schedule(parallel_scheduler)` declares `set_error(std::exception_ptr)`, so spawned chains end with `upon_error` (as design 15 §4.4 asks of fire-and-forget work anyway).
- **Stress test:** mixed `schedule`, `bulk` and nested `continues_on` from 4 threads until the deadline, then the producers stop and the spawn scope joins before the runtime is destroyed. The original version raced a detach with live submissions; that needed direct access to the backend, which the runtime no longer exposes.
- **Removed:** the five task-system headers, `task_system.cpp` and the five task-system test files.

## Acceptance criteria
- All tests in this document pass in Debug and Release; the stress test passes under ASan.
- No source in `lib/`, `include/`, `apps/` or `tests/` names the old task system, and only `parallel_backend.*` (and, after task 4, `runtime.cpp`) include `exec/static_thread_pool.hpp`.
- The two examples in [15 §4.5](../design/15-parallel-scheduler.md#45-examples) compile and run as tests (with an `io::scheduler` sender in place of `avis::io::read_file`).

## Open points
- **Banning `stdexec::sync_wait`** in `lib/` and `apps/` (a grep or clang-tidy rule) is still not added; it carries over from the old task 3.
- **`AVIS_ENABLE_ASAN`** ([13 §2.2](../design/13-build-and-quality.md#22-options)) would replace the manually configured ASan build folder.
- **Benchmark outcome.** If the type-erased path turns out much slower than the direct pool scheduler for the frame chain's recording `bulk`, the design would need a way to reach the pool scheduler directly for that one use. Decide only if the numbers call for it.
