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
| Default task system | `avis::threading::task_system` | `avis::runtime::task_system()` | [15](15-task-system.md) |

Everything else (RHI devices, scenes, Zenoh sessions, render graphs) is owned by the application. They aren't global.

The services themselves have no static or global state. `thread_pool`, `io_context` and `task_system` are ordinary classes. The *default* instances exist only because the runtime creates them. This keeps each component testable in isolation, and the dependencies stay clear: `thread_pool` ← `io_context`, `task_system` ← `runtime`.

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
        static threading::task_system& task_system();

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
    avis::runtime runtime{ make_runtime_configuration(configuration) };      // logging, thread pool, IO, tasks up
    return run_application(configuration);                            // app-owned threads stopped before returning
}
```

- Tests use a Catch2 fixture that constructs a `runtime` with a small configuration per test case. Component tests that don't need the runtime construct a `thread_pool`, `io_context` or `task_system` directly.

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

    struct io_configuration
    {
        std::size_t threads = 1;                          // long-running io_context bodies on the default pool
    };

    struct runtime_configuration
    {
        logging_configuration                logging;
        threading::thread_pool_configuration thread_pool;        // capacity, join warning (06 §3)
        io_configuration                     io;
        threading::task_system_configuration tasks;              // workers, priority, bulk chunking (15 §7)
        std::size_t application_threads = 0;              // long-running threads the application will start itself
    };
}
```

### 3.1 Sizing the default pool
If `thread_pool.capacity` is 0, the runtime computes it as

```text
capacity = io.threads + tasks.workers + application_threads + 2 (headroom)
```

| Application | `application_threads` | Defaults |
|---|---|---|
| Visualizer | 1 (simulation; the renderer has no thread of its own, it's a sender chain) | io 1, task workers `max(1, hardware_concurrency − 2)` |
| Driver | 2 (service, step executor) | io 1, task workers `max(1, hardware_concurrency − 2)` |
| Tests | per fixture | small explicit values |

An explicitly configured capacity that is smaller than `io.threads + tasks.workers + application_threads` is rejected with a configuration error at construction.

### 3.2 Mapping from application configuration
Application YAML ([01 §7](01-system-architecture.md#7-configuration)) feeds `runtime_configuration`:

```yaml
runtime:
  io_threads: 1
  task_workers: 0            # 0 = max(1, hardware_concurrency - 2)
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
4. **Default task system**: `threading::task_system(pool, configuration.tasks)`, which starts its worker bodies on the pool ([15 §5](15-task-system.md#5-lifecycle)).
5. Publish the instance pointer: the static accessors work from here on.

If a step fails, the steps already completed are rolled back in reverse order, and the constructor throws.

### 4.2 Destruction
Applications are expected to have stopped **their own** long-running threads (by destroying their `thread_handle`s) and to have finished using the task system before the runtime is destroyed. The runtime then:

1. **Task system**: `request_stop()`, drain queued tasks, join its workers ([15 §5](15-task-system.md#5-lifecycle)). This goes first, because tasks may still issue IO.
2. **IO**: stop accepting new operations, cancel outstanding ones (they complete with `set_stopped`), release the work guard, `io_context::stop()`, and join the IO bodies.
3. **Unpublish** the instance pointer (the static accessors assert from here on). This happens only after the task system and IO have stopped, so draining tasks and IO completions can still use the accessors.
4. **Thread pool**: destroying it runs the signal-all-then-join-all backstop for anything still running (an application thread that wasn't stopped), with the `join_warning` diagnostic ([06 §4.2](06-thread-pool.md#42-shutdown-as-specified)).
5. **Logging**: flush sinks, then shut down.

The destructor never throws. Failures during shutdown are logged.

## 5. Static access rules
| Function | No runtime alive | Runtime alive |
|---|---|---|
| `runtime::thread_pool()` | assert (debug) / `std::logic_error` (release) | the default thread pool |
| `runtime::io_context()` | assert / `std::logic_error` | the default IO context |
| `runtime::task_system()` | assert / `std::logic_error` | the default task system |
| `runtime::configuration()` | assert / `std::logic_error` | the active configuration |
| `runtime::alive()` | `false` | `true` |
| `avis::log::get(name)` | null logger (messages dropped) | the named logger |

Logging stays usable (as a no-op) outside the runtime, so static destructors and early startup code can log safely. Optional code checks `runtime::alive()` before using the other accessors.

## 6. Testing
- Construct, destroy, construct again in one process. Concurrent second construction throws.
- A construction failure at each step rolls back cleanly (fault injection through configuration: too-small capacity, an invalid log path, zero task workers).
- Static accessors behave as in §5 before, during and after the runtime's lifetime.
- Shutdown order: a task that issues IO during shutdown completes (value or stopped) before the IO context stops.
- No threads remain after destruction, including when the application "forgot" to stop a thread (the pool backstop joins it).
