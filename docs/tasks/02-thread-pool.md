# Task 2 — Thread pool

| | |
|---|---|
| Design | [06 — Thread Pool](../design/06-thread-pool.md), decisions D-10, D-11, D-14, D-15, A-32 ([99](../design/99-decisions-and-open-questions.md)) |
| Namespace | `avis::threading` |
| New headers | `include/avis/threading/thread_pool.h`, `include/avis/threading/thread_handle.h` |
| New sources | `lib/avis/threading/thread_pool.cpp`, `lib/avis/threading/thread_handle.cpp` |
| Tests | `tests/test_thread_pool.cpp` |
| Prerequisites | [Task 1](01-foundations.md) (`avis::error`, `make_error`, `avis::log`) |

The pool is a bounded set of named, long-lived OS threads, one *body* each. It has **no queues, no scheduler and no stdexec**. Its headers must never include task-system or stdexec headers ([06 §5](../design/06-thread-pool.md#5-separation-from-the-task-system)).

## 2.1 Public types and headers
- [ ] `thread_pool.h`: `thread_pool_configuration`, `thread_priority`, `thread_options`, `thread_body`, `thread_information`, `thread_pool` as in [06 §3](../design/06-thread-pool.md#3-api).
- [ ] `thread_information` (not specified in detail): `std::string name`, `std::uint32_t os_id`, `thread_state state` (`starting`, `running`, `stop_requested`, `exited`), `std::chrono::system_clock::time_point start_time`, `std::optional<error> exit_error`.
- [ ] `thread_handle.h`: the move-only `thread_handle` ([06 §3](../design/06-thread-pool.md#3-api)). Also add a default constructor (empty handle) and move assignment, so owners can store handles as members.
- [ ] Use `std::move_only_function` (C++23, available in MSVC with `/std:c++latest`) for `thread_body` and `on_exit`.

## 2.2 Shared thread record
Both the pool (for the backstop shutdown and `snapshot()`) and the `thread_handle` (for RAII ownership) need to reach one running thread. Put the state in a reference-counted record:

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

- [ ] Use `std::thread` + an explicit `std::stop_source`, not `std::jthread`, so a stop can be requested and the join performed by two different owners without double-joining.
- [ ] `join` is idempotent and safe from both owners: under `join_mutex`, if `thread.joinable()` then `thread.join()`.
- [ ] Guard against **self-join**: if `join()` is called on the record's own thread (for example a handle destroyed inside `on_exit` or inside its own body), skip the join (it would deadlock) and leave it to the other owner, which joins later. Never detach. Log a warning in that case, because it usually means an ownership mistake.
- [ ] The pool keeps `std::vector<std::shared_ptr<thread_record>>` under a mutex. Exited records are pruned lazily in `start()` and `snapshot()`.
- [ ] A `thread_handle` that outlives its pool stays valid: its `join()` returns at once because the pool's destructor already joined the thread.

## 2.3 Thread wrapper
The function actually run by `std::thread`:

1. Set the thread-local current index (§2.4) and the thread-local thread name used by the log formatter (task 1.2).
2. `SetThreadDescription(GetCurrentThread(), L"avis-<name>")` (convert with `MultiByteToWideChar`).
3. Apply `priority` (`SetThreadPriority`, map `thread_priority` → `THREAD_PRIORITY_*`) and `affinity_mask` (`SetThreadAffinityMask`). A failure here is logged as a warning, not fatal.
4. `state = running`, then call `body(stop_source.get_token())` inside `try { … } catch (...) { exit_error = make_error(std::current_exception()); log error with the thread name; }`.
5. Free the slot and the index (§2.4) **before** `on_exit`, so a restart triggered from `on_exit` can find a free slot.
6. Call `on_exit(exit_error)`, also inside `try/catch` (log and swallow).
7. `state = exited`, notify `exited_condition`.

- [ ] Implement the wrapper and the Win32 helpers in an anonymous namespace in `thread_pool.cpp`. `<windows.h>` stays out of the public headers (define `WIN32_LEAN_AND_MEAN` and `NOMINMAX`).

## 2.4 Capacity, slots and dense indices
- [ ] `start()` takes the pool mutex, rejects with `pool_stopping` if `request_stop()` was called, rejects with `pool_exhausted` if `running == capacity` ([06 §3.2](../design/06-thread-pool.md#32-capacity)), otherwise reserves a slot and the lowest free index, then creates the thread. Slot reservation and the check happen under the same lock, so concurrent `start()` calls can't oversubscribe.
- [ ] If `std::thread` construction throws (`std::system_error`), release the slot and return the error.
- [ ] Indices come from a free-index set of size `capacity` (a `std::vector<bool>` or a small bitset). `current_index()` reads a `thread_local std::optional<std::uint32_t>`; it is `nullopt` on threads not created by any pool.
- [ ] `running()` reads an atomic counter.

## 2.5 Stop and shutdown
- [ ] `thread_handle::request_stop()` / `join()` / `running()` / `exit_error()` / `~thread_handle()` (`request_stop()` + `join()`).
- [ ] `thread_pool::request_stop()`: set the stopping flag, then call `stop_source.request_stop()` on **every** record before anything waits (signal-all-then-join-all, [06 §4.2](../design/06-thread-pool.md#42-shutdown-as-specified)).
- [ ] `thread_pool::join()`: wait for every record. Use `exited_condition.wait_until(deadline)` with `deadline = now + join_warning`; when it expires, log every thread still running (name, OS id, state) once on the `core` logger, then keep waiting without a timeout. Never detach.
- [ ] `~thread_pool()`: `request_stop()` then `join()`. Must not throw.

## 2.6 Tests (`tests/test_thread_pool.cpp`)
Map one-to-one to [06 §6](../design/06-thread-pool.md#6-testing):

- [ ] Start a body that waits on its token (`std::condition_variable_any::wait(lock, token, predicate)`); `request_stop()` + `join()` returns; `running()` drops back to 0.
- [ ] Capacity: with `capacity = 2`, the third `start()` returns `pool_exhausted`; after one body returns, a new `start()` succeeds (slot reuse).
- [ ] `start()` after `request_stop()` returns `pool_stopping`.
- [ ] A throwing body: `exit_error()` is set, `on_exit` receives the error, a second body keeps running.
- [ ] Signal-all-then-join-all: N bodies each record the time they observed stop, and sleep a little after that; `~thread_pool` returns, and every "observed stop" time is earlier than the first body's exit.
- [ ] `join_warning`: with `join_warning = 50 ms` and a body that ignores the token for 200 ms, the captured `core` log contains the warning with the body's name.
- [ ] `current_index()`: dense (`0 … n-1` for n live threads), unique, reused after exit, `nullopt` on the test's main thread.
- [ ] Thread naming: `GetThreadDescription` on the body's thread returns `avis-<name>`.
- [ ] Handle outliving the pool: destroy the pool first, then the handle; no hang, no crash.
- [ ] No leaked threads: the process thread count after `~thread_pool` equals the count before construction (helper from task 1.3).

## 2.7 Separation check
- [ ] Add `tools/checks/check-threading-includes.ps1` that fails if `include/avis/threading/thread_pool.h`, `thread_handle.h` or `lib/avis/threading/thread_pool.cpp`, `thread_handle.cpp` include `task_system.h`, `task_scheduler.h`, `work_stealing_deque.h` or any `<stdexec/…>` / `<exec/…>` header ([13 §9](../design/13-build-and-quality.md#9-coding-standards)). Register it as a CTest test so it runs without CI.

## Acceptance criteria
- All tests in 2.6 pass in Debug and Release, and under `AVIS_ENABLE_ASAN` when it exists.
- The separation check passes.
- `thread_pool` has no static or global state other than the thread-local index/name ([06 §3.1](../design/06-thread-pool.md#31-static-semantics-through-the-runtime)).
