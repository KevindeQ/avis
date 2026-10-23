# Task 2 — Thread pool

| | |
|---|---|
| Design | [06 — Thread Pool](../design/06-thread-pool.md), decisions D-10, D-11, D-14, D-15, A-32 ([99](../design/99-decisions-and-open-questions.md)) |
| Namespace | `avis::threading` |
| New headers | `include/avis/threading/thread_pool.h`, `include/avis/threading/thread_handle.h`, `include/avis/threading/thread_priority.h`, `include/avis/threading/thread_options.h` |
| New sources | `lib/avis/threading/thread_pool.cpp`, `lib/avis/threading/thread_handle.cpp`, `lib/avis/threading/thread_priority.cpp`, `lib/avis/threading/pool_thread.h/.cpp`, `lib/avis/threading/thread_state.h/.cpp` (private) |
| Tests | `tests/test_thread_pool.cpp` |
| Prerequisites | [Task 1](01-foundations.md) (`avis::error`, `make_error`, `avis::log`) |

> **Status (2026-10-18): done.** All 12 thread pool tests pass in Debug, Release and an AddressSanitizer build, also in random order over repeated runs (40 in Debug, 40 in Release). The separation check was removed later (2026-10-23). Decisions made while implementing are under *As implemented*.

The pool is a bounded set of named, long-lived OS threads, one *body* each. It has **no queues, no scheduler and no stdexec**. Its headers must never include task-system or stdexec headers ([06 §5](../design/06-thread-pool.md#5-relation-to-the-parallel-scheduler)).

## 2.1 Public types and headers
- [x] `thread_pool.h`: `thread_pool_configuration`, `thread_pool`; `thread_priority.h`: `thread_priority`; `thread_options.h`: `thread_options`, `thread_body`; as in [06 §3](../design/06-thread-pool.md#3-api).
- ~~`thread_information` and `thread_pool::snapshot()`~~: removed after review (2026-10-19) as unneeded for now. `thread_state` (`starting`, `running`, `stop_requested`, `exited`) stays, private to the library.
- [x] `thread_handle.h`: the move-only `thread_handle` ([06 §3](../design/06-thread-pool.md#3-api)). Also add a default constructor (empty handle) and move assignment, so owners can store handles as members.
- [x] Use `std::move_only_function` (C++23, available in MSVC with `/std:c++latest`) for `thread_body` and `on_exit`.

## 2.2 Shared thread record
Both the pool (for the backstop shutdown) and the `thread_handle` (for RAII ownership) need to reach one running thread. Put the state in a reference-counted record:

```cpp
// lib/avis/threading/thread_record.h (private)
struct thread_record
{
    std::string name;
    std::uint32_t index;                 // dense pool index (§2.4)
    std::stop_source stop_source;
    std::thread thread;                  // joined exactly once (see below)
    std::atomic<thread_state> state;
    std::optional<error> exit_error;     // written by the thread before `state = exited`
    std::mutex join_mutex;
    std::condition_variable exited_condition;
    ...
};
```

- [x] Use `std::thread` + an explicit `std::stop_source`, not `std::jthread`, so a stop can be requested and the join performed by two different owners without double-joining.
- [x] `join` is idempotent and safe from both owners: under `join_mutex`, if `thread.joinable()` then `thread.join()`.
- [x] Guard against **self-join**: if `join()` is called on the record's own thread (for example a handle destroyed inside `on_exit` or inside its own body), skip the join (it would deadlock) and leave it to the other owner, which joins later. Never detach. Log a warning in that case, because it usually means an ownership mistake.
- [x] The pool keeps `std::vector<std::shared_ptr<pool_thread>>` under a mutex (implemented as the class `detail::pool_thread`, see *As implemented*). Exited records are pruned lazily in `start()` and `join()`.
- [x] A `thread_handle` that outlives its pool stays valid: its `join()` returns at once because the pool's destructor already joined the thread.

## 2.3 Thread wrapper
The function actually run by `std::thread`:

1. Set the thread-local current index (§2.4), and call `avis::log::set_thread_name("avis-<name>")` so the log pattern's `[t:…]` field shows the thread's role (task 1.2).
2. `SetThreadDescription(GetCurrentThread(), L"avis-<name>")` (convert with `MultiByteToWideChar`).
3. Apply `priority` (`SetThreadPriority`, map `thread_priority` → `THREAD_PRIORITY_*`) and `affinity_mask` (`SetThreadAffinityMask`). A failure here is logged as a warning, not fatal.
4. `state = running`, then call `body(stop_source.get_token())` inside `try { … } catch (...) { exit_error = make_error(std::current_exception()); log error with the thread name; }`.
5. Free the slot and the index (§2.4) **before** `on_exit`, so a restart triggered from `on_exit` can find a free slot.
6. Call `on_exit(exit_error)`, also inside `try/catch` (log and swallow).
7. `state = exited`, notify `exited_condition`.

- [x] Implement the wrapper and the Win32 helpers in an anonymous namespace in `thread_pool.cpp`. `<windows.h>` stays out of the public headers (define `WIN32_LEAN_AND_MEAN` and `NOMINMAX`).

## 2.4 Capacity, slots and dense indices
- [x] `start()` takes the pool mutex, rejects with `pool_stopping` if `request_stop()` was called, rejects with `pool_exhausted` if `running == capacity` ([06 §3.2](../design/06-thread-pool.md#32-capacity)), otherwise reserves a slot and the lowest free index, then creates the thread. Slot reservation and the check happen under the same lock, so concurrent `start()` calls can't oversubscribe.
- [x] If `std::thread` construction throws (`std::system_error`), release the slot and return the error.
- [x] Indices come from a free-index set of size `capacity` (a `std::vector<bool>` or a small bitset). `current_index()` reads a `thread_local std::optional<std::uint32_t>`; it is `nullopt` on threads not created by any pool.
- [x] `running()` reads an atomic counter.

## 2.5 Stop and shutdown
- [x] `thread_handle::request_stop()` / `join()` / `running()` / `exit_error()` / `~thread_handle()` (`request_stop()` + `join()`).
- [x] `thread_pool::request_stop()`: set the stopping flag, then call `stop_source.request_stop()` on **every** record before anything waits (signal-all-then-join-all, [06 §4.2](../design/06-thread-pool.md#42-shutdown-as-specified)).
- [x] `thread_pool::join()`: wait for every record. Use `exited_condition.wait_until(deadline)` with `deadline = now + join_warning`; when it expires, log every thread still running (name, OS id, state) once on the `core` logger, then keep waiting without a timeout. Never detach.
- [x] `~thread_pool()`: `request_stop()` then `join()`. Must not throw.

## 2.6 Tests (`tests/test_thread_pool.cpp`)
Map one-to-one to [06 §6](../design/06-thread-pool.md#6-testing):

- [x] Start a body that waits on its token (`std::condition_variable_any::wait(lock, token, predicate)`); `request_stop()` + `join()` returns; `running()` drops back to 0.
- [x] Capacity: with `capacity = 2`, the third `start()` returns `pool_exhausted`; after one body returns, a new `start()` succeeds (slot reuse).
- [x] `start()` after `request_stop()` returns `pool_stopping`.
- [x] A throwing body: `exit_error()` is set, `on_exit` receives the error, a second body keeps running.
- [x] Signal-all-then-join-all: N bodies each record the time they observed stop, and sleep a little after that; `~thread_pool` returns, and every "observed stop" time is earlier than the first body's exit.
- [x] `join_warning`: with `join_warning = 50 ms` and a body that ignores the token for 200 ms, the captured `core` log contains the warning with the body's name.
- [x] `current_index()`: dense (`0 … n-1` for n live threads), unique, reused after exit, `nullopt` on the test's main thread.
- [x] Thread naming: `GetThreadDescription` on the body's thread returns `avis-<name>`.
- [x] Handle outliving the pool: destroy the pool first, then the handle; no hang, no crash.
- [x] No leaked threads: the process thread count after `~thread_pool` equals the count before construction (helper from task 1.3).

## 2.7 Separation check
- ~~Add `tools/checks/check-threading-includes.ps1` that fails if `include/avis/threading/thread_pool.h`, `thread_handle.h` or `lib/avis/threading/thread_pool.cpp`, `thread_handle.cpp` include `task_system.h`, `task_scheduler.h`, `work_stealing_deque.h` or any `<stdexec/…>` / `<exec/…>` header ([13 §9](../design/13-build-and-quality.md#9-coding-standards)). Register it as a CTest test so it runs without CI.~~ Removed (2026-10-23): the script and its CTest test are gone.

## As implemented
- **Structure (refactored after review, 2026-10-20):** everything about one thread lives in the private class `detail::pool_thread` (`lib/avis/threading/pool_thread.h/.cpp`): the `std::thread`, name, index, stop source, `thread_state`, OS id and exit error, plus the thread wrapper (`run()`), the private `configure_os_thread()` that sets the OS thread's description, priority and affinity, and the two `static thread_local` members behind `current()` and `current_index()`. `thread_priority` and `to_win32_priority()` live in their own files, the public `include/avis/threading/thread_priority.h` (which needs no `<windows.h>`: the function returns the `THREAD_PRIORITY_*` value as `int`) and `lib/avis/threading/thread_priority.cpp`. It is shared by the pool and the body's `thread_handle`, so a handle may outlive its pool. `pool_thread::start()` creates it behind a `shared_ptr` and launches the OS thread, which holds its own reference while it runs. `thread_pool` keeps only the pool-wide state as direct members (configuration, mutex, thread list, used indices, running counter, stopping flag) with no pimpl. When its body returns, before `on_exit`, the thread calls a `release_index` callback that `thread_pool::start()` passes to `pool_thread::start()` (a lambda calling the private `thread_pool::release_index()`). So `pool_thread` holds no pointer to the pool and needs no `friend` access (changed after review, 2026-10-20). `thread_pool` is explicitly non-movable, because the callback captures `this`. `thread_options` and `thread_body` live in the public `include/avis/threading/thread_options.h`, included by both `thread_pool.h` and `pool_thread.h`, so `pool_thread` doesn't depend on `thread_pool` at all (2026-10-20).
- **Why `std::thread`, not `std::jthread`:** `request_stop()` from the pool or the handle can run concurrently with `join()` on another thread, and calling a `jthread`'s non-const members concurrently is a data race; its self-joining destructor would also be a hazard when the last reference is dropped on the thread itself. A separate `std::stop_source` avoids both.
- **`thread_state`** and its `to_string()` (in the private `lib/avis/threading/thread_state.h/.cpp`) is `starting`, `running`, `stop_requested`, `exited`. `request_stop` moves `starting`/`running` to `stop_requested`, which the `join_warning` message reports.
- **Joining:** `pool_thread::join()` first waits for the body and `on_exit` to finish, then joins the OS thread under its join mutex. A join from the thread itself is skipped with a warning, never detached; `pool_thread::current()` (thread-local) identifies the calling thread. `pool_thread::current_index()` backs `thread_pool::current_index()` and is reset before `on_exit`, because the index may already be reused.
- **`exit_error()` returns a copy** (changed after review, 2026-10-20), on both `thread_handle` and `pool_thread`. It is read under the exit mutex and is `nullopt` until the thread has exited, so it is safe to call at any time and can't dangle once the handle releases the thread. A reference was valid only while the same handle still held the thread, and reading it while the body ran was a data race. The copy costs one string allocation, and only when the body failed. `on_exit` still receives `const std::optional<error>&`, because it runs on the exiting thread while the object is alive.
- **Pruning:** a record is dropped from the pool only once it has exited **and** its OS thread has been joined (by its handle). The pool's own `join()` joins every record, so `~thread_pool` leaves no OS thread behind, even if a handle is still alive.
- **`join_warning`** logs once on `core`, for example `thread pool: threads still running 50 ms after the stop request: avis-stubborn (os id 1234, stop_requested)`, then waits without a timeout.
- **`request_stop()`** signals the records outside the pool mutex, because stop callbacks run inline and may call back into the pool.
- **Extra tests** beyond §2.6: destroying one handle stops only its body; the slot is already free when `on_exit` runs (a restart from `on_exit` succeeds).
- **ASan:** there is no `AVIS_ENABLE_ASAN` option yet. The ASan run used a separate build folder configured with `-fsanitize=address` (and without `/RTC1`) for the C and C++ flags.

## Acceptance criteria
- [x] All tests in 2.6 pass in Debug and Release, and under AddressSanitizer (a manually configured build, since `AVIS_ENABLE_ASAN` doesn't exist yet).
- ~~The separation check passes, and fails on a forbidden include (checked with a scratch copy).~~ Removed with the check (2026-10-23).
- [x] `thread_pool` has no static or global state other than the thread-local index and record pointer ([06 §3.1](../design/06-thread-pool.md#31-static-semantics-through-the-runtime)).
