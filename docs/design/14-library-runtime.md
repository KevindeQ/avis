# 14 — Library Runtime

| | |
|---|---|
| Namespace | `avis` |
| Header | `include/avis/runtime.h` |
| CMake target | `AVIS_LIB` |

## 1. Purpose
`avis::runtime` is the **library-level RAII object** that initialises and shuts down the Avis library's process-wide services. It's a separate concept from the services themselves: the runtime *creates, configures, owns and exposes* them, but doesn't define how any of them work.

The runtime owns these services, so it is also their **static access point**:

| Service | Type | Static access | Document |
|---|---|---|---|
| Logging | spdlog loggers | `avis::log::get("<logger>")` | [13 §7](13-build-and-quality.md#7-logging) |
| Default thread pool | `avis::threading::thread_pool` | `avis::runtime::thread_pool()` | [06](06-thread-pool.md) |
| Default IO context | `avis::io::io_context` | `avis::runtime::io_context()` | [07](07-io-layer.md) |
| Default parallel scheduler | `exec::static_thread_pool`, behind `stdexec::parallel_scheduler` | `stdexec::get_parallel_scheduler()` | [15](15-parallel-scheduler.md) |

Everything else (RHI devices, scenes, Zenoh sessions, render graphs) is owned by the application. They aren't global.

The services themselves have no static or global state. `thread_pool`, `io_context` and `exec::static_thread_pool` are ordinary classes. The *default* instances exist only because the runtime creates them. This keeps each component testable in isolation, and the dependencies stay clear: `thread_pool` ← `io_context` ← `runtime`, and `exec::static_thread_pool` ← `runtime`.

The parallel scheduler is the one service whose static access point isn't on the runtime: code uses the standard `stdexec::get_parallel_scheduler()`, and Avis's link-time definition of stdexec's backend query returns the backend the runtime publishes ([15 §3.1](15-parallel-scheduler.md#31-replacing-the-backend-at-link-time)). The runtime still owns the pool and decides its size and lifetime.

## 2. API

```cpp
namespace avis
{
    class runtime
    {
    public:
        explicit runtime(const runtime_configuration& configuration);   // throws avis::runtime_error on failure (unrecoverable, 01 §6)
        ~runtime();                                        // ordered shutdown (§4)
        runtime(const runtime&) = delete;
        runtime& operator=(const runtime&) = delete;

        // static access to the default services (§5)
        static threading::thread_pool& thread_pool();
        static io::io_context&        io_context();
        // the parallel scheduler is reached through stdexec::get_parallel_scheduler() (15 §3.1)

        static bool alive() noexcept;
        static const runtime_configuration& configuration();
    };
}
```

- **Exactly one** `runtime` per process at a time. A second concurrent construction throws. Sequential lifetimes are allowed (construct, destroy, construct), which unit tests rely on.
- The static accessors read an atomic pointer to the live runtime instance. It's set at the end of construction and cleared at the start of destruction. This is the same binding pattern as the virtual clock's `static_state_policy`.
- Applications create the runtime at the top of `main` and keep it alive until `main` returns:

```cpp
int main(int argc, char** argv)
{
    auto configuration = load_application_configuration(argc, argv);                 // visualizer.yaml / driver.yaml
    avis::runtime runtime{ make_runtime_configuration(configuration) };      // logging, thread pool, IO, parallel pool up
    return run_application(configuration);                            // app-owned threads stopped before returning
}
```

- Tests use a Catch2 fixture that constructs a `runtime` with a small configuration per test case. Component tests that don't need the runtime construct a `thread_pool` or `io_context` directly. Tests of code that uses `stdexec::get_parallel_scheduler()` need the runtime (or the fixture), because the backend is installed by it.

## 3. Configuration

```cpp
namespace avis
{
    struct logging_configuration
    {
        log_level default_level = log_level::info;
        std::map<std::string, log_level> levels;          // per logger, e.g. {"rhi", debug}
        std::optional<std::filesystem::path> file;        // rotating file sink
        bool console = false;
        bool debugger_output = true;                      // OutputDebugString sink
    };

    struct runtime_configuration
    {
        logging_configuration                logging;
        threading::thread_pool_configuration thread_pool;        // capacity, join warning (06 §3)
        io::io_configuration                 io;                 // threads of the default IO context (07 §3)
        threading::parallel_configuration    parallel;           // workers of the parallel pool (15 §7)
        std::size_t application_threads = 0;              // long-running threads the application will start itself

        static result<runtime_configuration> parse(std::string_view yaml_text);           // the YAML sections of §3.2
        static result<runtime_configuration> resolve(runtime_configuration configuration);  // computed values of §3.1
    };
}
```

Each service's configuration is defined next to the service, so `runtime_configuration` only gathers them: `logging_configuration` in `avis/log.h`, `threading::thread_pool_configuration` in `avis/threading/thread_pool.h`, `io::io_configuration` in `avis/io/io_configuration.h` and `threading::parallel_configuration` in `avis/threading/parallel_configuration.h`. The last two are free of asio and stdexec, so including `runtime_configuration.h` doesn't pull them in.

### 3.1 Sizing the default pool
If `thread_pool.capacity` is 0, the runtime computes it as

```text
capacity = io.threads + application_threads + 2 (headroom)
```

| Application | `application_threads` | Defaults |
|---|---|---|
| Visualizer | 1 (simulation; the renderer has no thread of its own, it's a sender chain) | io 1, parallel workers `max(1, hardware_concurrency − 2)` |
| Driver | 2 (service, step executor) | io 1, parallel workers `max(1, hardware_concurrency − 2)` |
| Tests | per fixture | small explicit values |

The parallel workers are created by `exec::static_thread_pool`, not by the thread pool, so they don't count towards its capacity ([15 §1](15-parallel-scheduler.md#1-requirements)).

An explicitly configured capacity that is smaller than `io.threads + application_threads` is rejected with a configuration error at construction.

### 3.2 Mapping from application configuration
Application YAML ([01 §7](01-system-architecture.md#7-configuration)) feeds `runtime_configuration`:

```yaml
runtime:
  io_threads: 1
  parallel_workers: 0        # 0 = max(1, hardware_concurrency - 2)
  thread_pool_capacity: 0    # 0 = computed (§3.1)
logging:
  level: info
  file: "./logs/driver.log"
```

## 4. Initialisation and shutdown order

### 4.1 Construction
1. **Logging**: first, so every later step can report errors.
2. **Default thread pool**: `threading::thread_pool(configuration.thread_pool)` with the computed capacity.
3. **Default IO context**: `io::io_context`, which starts `io.threads` long-running bodies on the pool, each running `io_context::run()` under a work guard ([07 §3](07-io-layer.md#3-execution-model)).
4. **Parallel pool**: `exec::static_thread_pool` with `parallel.workers` threads, a `threading::parallel_backend` for it, published for Avis's definition of stdexec's backend query, which lives in `runtime.cpp` ([15 §3.1](15-parallel-scheduler.md#31-replacing-the-backend-at-link-time), [15 §5](15-parallel-scheduler.md#5-lifecycle)). `stdexec::get_parallel_scheduler()` works from here on.
5. Publish the instance pointer: the static accessors work from here on.

If a step fails, the steps already completed are rolled back in reverse order, and the constructor throws.

### 4.2 Destruction
Applications are expected to have stopped **their own** long-running threads (by destroying their `thread_handle`s) and to have finished or cancelled their parallel work before the runtime is destroyed. Work still queued on the parallel pool at that point may never run, and its operations may never complete (D-63). The runtime then:

1. **Parallel pool**: unpublish the backend (`get_parallel_scheduler()` throws from here on), detach it (earlier scheduler copies complete with `set_stopped()`), then destroy `exec::static_thread_pool`, which stops and joins its workers ([15 §5](15-parallel-scheduler.md#5-lifecycle)). This goes first, because parallel work may still issue IO.
2. **IO**: stop accepting new operations, cancel outstanding ones (they complete with `set_stopped`), release the work guard, `io_context::stop()`, and join the IO bodies.
3. **Unpublish** the instance pointer (the static accessors assert from here on). This happens only after the parallel pool and IO have stopped, so the last parallel work and IO completions can still use the accessors.
4. **Thread pool**: destroying it runs the signal-all-then-join-all backstop for anything still running (an application thread that wasn't stopped), with the `join_warning` diagnostic ([06 §4.2](06-thread-pool.md#42-shutdown-as-specified)).
5. **Logging**: flush sinks, then shut down.

The destructor never throws. Failures during shutdown are logged.

## 5. Static access rules
| Function | No runtime alive | Runtime alive |
|---|---|---|
| `runtime::thread_pool()` | assert (debug) / `std::logic_error` (release) | the default thread pool |
| `runtime::io_context()` | assert / `std::logic_error` | the default IO context |
| `stdexec::get_parallel_scheduler()` | `std::runtime_error` (thrown by stdexec: no backend published) | the parallel scheduler on the runtime's pool |
| `runtime::configuration()` | assert / `std::logic_error` | the active configuration |
| `runtime::alive()` | `false` | `true` |
| `avis::log::get(name)` | null logger (messages dropped) | the named logger |

Logging stays usable (as a no-op) outside the runtime, so static destructors and early startup code can log safely. Optional code checks `runtime::alive()` before using the other accessors.

## 6. Testing
- Construct, destroy, construct again in one process. Concurrent second construction throws.
- A construction failure at each step rolls back cleanly (fault injection through configuration: too-small capacity, an invalid log path).
- Static accessors behave as in §5 before, during and after the runtime's lifetime.
- `stdexec::get_parallel_scheduler()` runs work on the runtime's pool while the runtime is alive, and throws before and after.
- Shutdown order: parallel work that issues IO and finishes before the runtime is destroyed completes (value or stopped) before the IO context stops.
- No threads remain after destruction, including when the application "forgot" to stop a thread (the pool backstop joins it).
