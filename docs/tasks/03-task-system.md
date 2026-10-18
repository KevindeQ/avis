# Task 3 — Task system

| | |
|---|---|
| Design | [15 — Task System](../design/15-task-system.md), decisions D-13 to D-15, A-31, A-33 to A-36 ([99](../design/99-decisions-and-open-questions.md)) |
| Namespace | `avis::threading` |
| New headers | `include/avis/threading/task.h`, `work_stealing_deque.h`, `task_inbox.h`, `task_system.h`, `task_scheduler.h` |
| New sources | `lib/avis/threading/task_system.cpp` |
| Tests | `tests/test_work_stealing_deque.cpp`, `tests/test_task_inbox.cpp`, `tests/test_task_system.cpp`, `tests/test_task_scheduler.cpp` |
| Prerequisites | [Task 1](01-foundations.md), [Task 2](02-thread-pool.md) (`thread_pool::start`, `thread_handle`) |
| Reference | `external/stdexec/include/exec/static_thread_pool.hpp`: same stdexec version, shows the scheduler/`bulk_chunked` customisation points this version expects |

The task system multiplexes many short tasks over a fixed set of workers that it starts on the thread pool. It depends on the pool; the pool never sees it ([15 §1](../design/15-task-system.md#1-requirements)).

Build it bottom-up: lock-free containers first (testable without threads from the pool), then the worker loop, then the stdexec layer.

## 3.1 Task node
- [ ] `task.h`: `struct task { task* next = nullptr; void (*execute)(task*) noexcept = nullptr; };` ([15 §3.4](../design/15-task-system.md#34-tasks)). Intrusive: owners embed it (operation states) or allocate it (`submit`).
- [ ] `next` is used by the inbox only; the deque stores `task*` values in its ring buffer.

## 3.2 Chase–Lev work-stealing deque
- [ ] `work_stealing_deque.h`: `class work_stealing_deque` with `push(task*)` and `pop()` (owner only, bottom, LIFO) and `steal()` (any thread, top, FIFO). `steal()` returns a tri-state (`task*`, `empty`, `lost_race`) so the caller can retry on a lost race instead of treating it as empty.
- [ ] Follow the C11 memory-order version from Lê, Pop, Cohen, Zappa Nardelli, *Correct and Efficient Work-Stealing for Weak Memory Models* (PPoPP 2013). Keep its `seq_cst` fences in `pop()` and `steal()`; they are what makes the last-element race correct on x64 too.
- [ ] Growable ring buffer: capacity a power of two (initial 256). On `push` into a full buffer, allocate one twice as large, copy the live range, publish it with a release store. **Don't free the old buffer** while thieves may still read it: keep retired buffers in a list owned by the deque and free them in its destructor (bounded: at most log₂ of the peak size).
- [ ] Pad `top` and `bottom` to separate cache lines (`alignas(std::hardware_destructive_interference_size)`, or 64 if MSVC warns).

**Tests:** single-threaded LIFO/FIFO order and growth; one owner pushing/popping 1M items while 3 thieves steal: every item is seen exactly once (count with an `std::atomic<std::uint8_t>` per item), no item lost.

## 3.3 Inbox (MPSC queue)
- [ ] `task_inbox.h`: intrusive Vyukov MPSC queue over `task::next`, with an embedded stub node. `push(task*)` is wait-free for any producer.
- [ ] `drain_into(work_stealing_deque&)`: owner moves everything currently reachable into its deque and returns one task to run. `try_pop()` takes one task, for thieves (§3.5).
- [ ] Handle Vyukov's transient "inconsistent" state (a producer exchanged `head` but hasn't linked `next` yet): `try_pop` returns "retry later", not "empty". The worker loop must not park on that result without re-checking (§3.6).
- [ ] **Multiple consumers:** the design lets thieves `try_pop` from a victim's inbox, but the Vyukov queue is single-consumer. Serialise the consumer side with a per-inbox `std::atomic_flag` *try*-lock: the owner and thieves take it with `test_and_set`; whoever fails skips that inbox for this pass. See Open points.

**Tests:** 4 producers × 250k items, one consumer, each item exactly once; same with the owner draining while 2 thieves `try_pop` under the try-lock.

## 3.4 Task system skeleton
- [ ] `task_system.h`: `task_system_configuration` and `task_system` as in [15 §4.1](../design/15-task-system.md#41-api). `workers = 0` resolves to `max(1, hardware_concurrency − 2)`.
- [ ] Per-worker state, cache-line aligned: deque, inbox, inbox try-lock, `next_victim` cursor, `std::atomic<std::uint32_t> epoch` for parking, debug statistics (`executed`, `stolen_from_deque`, `stolen_from_inbox`, `parks`).
- [ ] `thread_local worker* current_worker_` and `thread_local task_system* current_system_`, set at the start of each worker body. `task_system::current()` returns `current_system_`.
- [ ] Constructor ([15 §5](../design/15-task-system.md#5-lifecycle)): allocate the workers, then `pool.start("task-<n>", body, { .priority = worker_priority })` for each. If any `start` fails, stop the ones already started and throw `avis::runtime_error` with the pool's error (configuration error: the runtime reports it). Wait on a `std::latch` until every worker has registered its thread-locals.
- [ ] Keep a global `std::atomic<std::size_t> outstanding_` counter: incremented **before** a task is enqueued, decremented **after** it executed. Shutdown relies on it (§3.7).

## 3.5 Submission
Follow [15 §3.2](../design/15-task-system.md#32-submission):

- [ ] `enqueue(task*)` (internal): from a worker of *this* system, push onto its own deque, then wake one parked worker if `sleepers_ > 0`; from any other thread, push into the inbox of worker `round_robin_.fetch_add(1) % workers` and wake that worker.
- [ ] `enqueue_batch(std::span<task*>)` for `bulk`: same rule, but local pushes go in one go and external ones are spread round-robin; wake `min(count, sleepers_)` workers.
- [ ] `submit(F&&)`: wrap the callable in a heap node `submit_task<F> : task` whose `execute` runs `f()` inside `try/catch`, logs a thrown exception on the `core` logger as a programming error ([15 §3.4](../design/15-task-system.md#34-tasks)), then deletes the node. Start with `new`/`delete`; replace with per-worker free lists in §3.10 once benchmarks show it matters.
- [ ] After `request_stop()`, `submit` drops the callable and logs a warning.

## 3.6 Worker loop
Implement [15 §3.3](../design/15-task-system.md#33-worker-loop):

```text
loop:
    task = local.pop()
    if !task: task = inbox.drain_into(local)           # under the inbox try-lock
    if !task: task = steal_round_robin()
    if task: execute(task); outstanding_--; continue
    if stop_requested and outstanding_ == 0: return
    spin ~2 µs (_mm_pause), retrying the three sources
    park
```

- [ ] `steal_round_robin()`: visit at most `N − 1` victims starting at `next_victim` (skip self). For each: `deque.steal()` (retry on `lost_race`), then `inbox.try_pop()` under the victim's try-lock. On success set `next_victim` to the victim **after** the one robbed.
- [ ] Parking without lost wake-ups (Dekker pattern):
  1. `observed = epoch.load()`; `sleepers_.fetch_add(1)`;
  2. `std::atomic_thread_fence(seq_cst)`; re-check own deque, own inbox and `outstanding_`/stop once more; if anything is found, `sleepers_--` and go back to the loop;
  3. `epoch.wait(observed)`; `sleepers_--`.

  Wakers: push, `std::atomic_thread_fence(seq_cst)`, then `epoch.fetch_add(1)` + `epoch.notify_one()` on the target. For "wake any parked worker" keep a small lock-free stack or bitmask of parked worker indices, so the waker notifies one that is actually parked.
- [ ] The worker body registers a `std::stop_callback` on its pool stop token that bumps **every** epoch and notifies all, so parked workers notice shutdown at once ([15 §3.3](../design/15-task-system.md#33-worker-loop)).

**Tests** (`test_task_system.cpp`, mapping [15 §8](../design/15-task-system.md#8-testing)):
- [ ] 1M tiny `submit` tasks from 4 external producers plus tasks spawned from inside tasks: each runs exactly once (per-index counter), `outstanding_` returns to 0.
- [ ] Own-queue-first: a task on worker *w* spawns children; with the other workers kept busy, *w* runs them in LIFO order.
- [ ] Round-robin stealing: put 10k tasks into one worker's deque (spawn them from a task on that worker); every other worker's `stolen_from_deque` > 0, and the victim cursors advanced (debug statistics).
- [ ] Inbox stealing: block worker *w* inside a long task, submit externally until *w*'s inbox has items, check another worker's `stolen_from_inbox` > 0.
- [ ] Parked workers wake: idle the system for 100 ms (all parked), submit one task, it runs within a few ms.

## 3.7 Shutdown
Implement [15 §5](../design/15-task-system.md#5-lifecycle):

- [ ] `request_stop()`: set `stopping_` (seq_cst), wake every worker.
- [ ] Workers keep draining until `stopping_ && outstanding_ == 0`, then return. Because `outstanding_` is incremented before enqueue, a submit racing with `request_stop()` either sees `stopping_` and drops its task, or is counted and gets executed. No task is lost in a queue of a worker that already exited.
- [ ] `~task_system()`: `request_stop()`, then destroy the `thread_handle`s (joins the workers). Assert that `outstanding_ == 0` afterwards.

**Tests:** shutdown with 100k queued tasks: every one runs (or, for scheduler operations, completes with `set_stopped`); no worker thread remains (thread-count helper).

## 3.8 stdexec scheduler
- [ ] `task_scheduler.h`: `class task_system::scheduler` with `using scheduler_concept = stdexec::scheduler_tag;`, `schedule()`, `operator==` (same `task_system*`), and `query(stdexec::get_forward_progress_guarantee_t) → parallel` ([15 §4.2](../design/15-task-system.md#42-scheduler)).
- [ ] Schedule sender: `completion_signatures<set_value_t(), set_stopped_t()>`; `get_env` exposes `get_completion_scheduler<set_value_t>` → this scheduler. (No error channel: scheduling itself can't fail.)
- [ ] Operation state embeds a `task` node, so `schedule()` never allocates. `start()`: if the system is stopping, complete `set_stopped()` inline; otherwise `enqueue(&node)`. `execute`: if `get_stop_token(get_env(receiver)).stop_requested()` → `set_stopped()`, else `set_value()`.
- [ ] `get_scheduler()` on `task_system`.

**Tests** (`test_task_scheduler.cpp`): `sync_wait(schedule(s) | then(…))` runs on a worker (`task_system::current() == &system`); `continues_on(s)` from an `io::scheduler` sender (existing `asio_scheduler.h`); a pre-stopped `inplace_stop_source` gives `set_stopped`; `when_all` of 1000 scheduled senders completes; scheduler concept checks with `static_assert(stdexec::scheduler<…>)`.

## 3.9 `bulk`
Implement [15 §4.3](../design/15-task-system.md#43-bulk) as a domain customisation, following `exec::static_thread_pool`'s handling of `bulk_chunked` / `bulk_unchunked` in the vendored stdexec:

- [ ] Give the scheduler a `task_system_domain` (returned from `query(stdexec::get_domain_t)`), with `transform_sender` for `bulk_chunked_t` that replaces the default sequential bulk with a parallel one.
- [ ] Operation state: holds the predecessor's values, the function, `std::atomic<std::size_t> remaining`, `std::atomic<bool> failed`, a once-only error slot, and an array of `min(n, workers × bulk_chunks_per_worker)` chunk task nodes (one allocation per bulk operation).
- [ ] When the predecessor completes with values, compute contiguous chunk ranges and `enqueue_batch` the chunk nodes.
- [ ] Each chunk: if `failed` or the receiver's stop token is signalled, skip; otherwise run `f(begin, end, values…)` in `try/catch`; on exception, the first one stores `make_error(current_exception())` and sets `failed`.
- [ ] The chunk that brings `remaining` to 0 completes the receiver: `set_error(avis::error)` if failed, else `set_stopped()` if stopped, else `set_value(values…)`.
- [ ] `n == 0` completes with the values immediately.

**Tests:** coverage of `[0, n)` for `n ∈ {0, 1, 7, workers × 4, 1M}` (each index exactly once); first error wins and later chunks are skipped; stop requested mid-bulk gives `set_stopped`; bulk issued from inside a task lands on the local deque (debug statistics).

## 3.10 Error conversion, waiting rules, hardening
- [ ] Error type ([15 §4.4](../design/15-task-system.md#44-error-type)): provide `avis::threading::into_avis_error()` (a sender adaptor built on `stdexec::let_error`/`upon_error`) that maps `std::exception_ptr` to `avis::error` via `make_error`. See Open points about doing this automatically.
- [ ] `sync_wait` from a worker ([15 §4.5](../design/15-task-system.md#45-waiting)): add `avis::threading::sync_wait(sender)` that asserts `task_system::current() == nullptr` in debug builds, then forwards to `stdexec::sync_wait`. Use it in library code; see Open points.
- [ ] Per-worker node free lists for `submit` (a thread-local cache; nodes freed on another worker go to that worker's cache) and a global fallback for external submitters. Only if the 1M-task benchmark shows allocation is significant.
- [ ] Stress test: 30 s mixed workload (external submits, nested spawns, bulks, stops) under ASan and under Application Verifier (`appverif -enable Heaps Locks Handles -for avis_tests.exe`). Tag `[stress]` and keep it out of the default test run.
- [ ] Micro-benchmark (`[benchmark]` tag, Catch2 `BENCHMARK`): empty-task throughput from inside and outside, compared against `exec::static_thread_pool`.

## Acceptance criteria
- All tests in this document pass in Debug and Release; the stress test passes under ASan.
- The separation check from task 2.7 still passes (task-system headers may include `thread_pool.h`, not the reverse).
- The two examples in [15 §4.6](../design/15-task-system.md#46-examples) compile and run as tests (with `avis::io::read_file` replaced by an `io::scheduler` sender until the IO layer exists).

## Open points
- **Inbox stealing vs. a single-consumer queue.** Proposed resolution: a per-inbox try-lock on the consumer side (above). Alternative: drop inbox stealing and rely on the owner draining its inbox, at the cost of a stuck worker's inbox being unreachable until it returns.
- **Automatic exception → `avis::error` conversion.** stdexec's `then` reports exceptions as `set_error(std::exception_ptr)` and the scheduler never sees them. Options: (a) the explicit `into_avis_error()` adaptor; (b) also customise `then`/`let_value` in `task_system_domain`. Proposal: (a) now, (b) only if call sites get noisy. Update [15 §4.4](../design/15-task-system.md#44-error-type) accordingly.
- **Enforcing "no `sync_wait` on a worker".** `stdexec::sync_wait` can't be intercepted for arbitrary senders. Proposal: the `avis::threading::sync_wait` wrapper plus a clang-tidy/grep rule banning `stdexec::sync_wait` in `lib/` and `apps/`.
