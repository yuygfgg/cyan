# Threading Plan

This note records the threading design after choosing a non-hole `spawn`
implementation.

The important constraint is now explicit:

- `spawn(...)` must not be a compiler-only special form
- the compiler may provide small general-purpose primitives
- the stdlib remains responsible for task ownership and API shape
- the native runtime remains a separate library that the build links in

That gives Cyan two distinct concurrency layers:

- `parallel_do(...)`: borrowed, blocking, lexical fork-join
- `spawn(...)`: owned, non-lexical task submission with later `join`

## Status

Phase 1 is implemented:

- `/std.thread` now exposes owned-task spawn/join APIs
- the native runtime now has a small executor shared by `parallel_do(...)` and
  `spawn(...)`
- `/std.abi` now exposes builtin-lowered `fn_ptr(...)`, which `/std.thread`
  uses internally
- no `spawn`-specific compiler lowering was added

## Surface API

The current thread surface is:

```cyan
interface<T> void run_task(&T task);
interface<T> void run_once(&mut T task);

interface thread_shared = share - local;
interface thread_sendable = send - local;
interface threaded_runnable = run_task + thread_shared;
interface spawnable = run_once + thread_sendable;

struct JoinHandle {
    void* raw;
};

void parallel_do([]&threaded_runnable tasks);
JoinHandle spawn<T>(T task);
void join(&mut JoinHandle handle);
```

`JoinHandle` is completion-only in phase 1. Dropping an unjoined handle detaches
it.

## Why This Design

The key design choice is that `spawn(...)` is an ordinary generic stdlib
function, not a compiler hole.

That matters because the real ownership transition happens in library code:

- `spawn<T>(T task)` checks `spawnable`
- stdlib heap-allocates storage for `T`
- stdlib moves `task` into that heap payload
- stdlib passes the payload and two thunk addresses into the runtime

The runtime only sees erased pointers. It does not need to understand Cyan
types, generic instantiations, or interface dispatch layout for owned tasks.

## Stdlib Work

`/std.thread` now contains the owned-task layer:

- `run_once(&mut T)` for one-shot owned execution
- `spawnable = run_once + thread_sendable`
- `JoinHandle`
- `spawn<T>(T task)`
- `join(&mut JoinHandle handle)`
- `drop JoinHandle` as detach-on-drop

The implementation strategy is:

```cyan
void spawn_run_once<T>(T* task) { ... }
void spawn_drop<T>(T* task) { ... }

JoinHandle spawn<T>(T task) {
    require_spawnable(&mut task);
    T* payload = malloc(sizeof(T)) as T*;
    *payload = task;
    void* handle = cyan_runtime_spawn(payload as void*,
                                      fn_ptr<T>(spawn_run_once),
                                      fn_ptr<T>(spawn_drop));
    ...
}
```

Important details:

- `spawn<T>` stays fully generic in stdlib
- per-`T` run/drop thunks are also ordinary generic Cyan functions
- `spawn(...)` requires ownership transfer via `thread_sendable`, not
  `thread_shared`
- if the runtime cannot enqueue a task, stdlib falls back to inline
  run-and-drop so task semantics still complete

The stdlib split is now explicit:

- `/std.thread` owns task execution APIs
- `/std.abi` owns the raw function-address escape hatch `fn_ptr(...)`
- `/std.atomic` owns memory-order helpers and atomic operations
- `/std.sync` remains the safe `Arc<T>` and `Mutex<T>` layer

## Runtime Work

The native runtime is still required. The build must always link it.

The important change is that it is no longer just a narrow `parallel_do(...)`
helper. It is now a small executor substrate.

Current runtime exports:

```c
int cyan_runtime_parallel_do(const void *tasks, int64_t count);
void *cyan_runtime_spawn(void *payload, void *run_fn, void *drop_fn);
void cyan_runtime_join(void *handle);
void cyan_runtime_detach(void *handle);
```

Current runtime responsibilities:

- worker pool
- global task queue
- wait-group state for blocking `parallel_do(...)`
- join state for owned spawned tasks
- task records carrying:
  - payload pointer
  - run thunk pointer
  - drop thunk pointer
  - optional wait-group pointer
  - optional join-state pointer

Conceptually:

```text
Task {
    payload
    run
    drop
    group
    join_state
}
```

This keeps the runtime ABI simple:

- all owned-task execution is `void* payload + void* thunk pointers`
- no struct-passing ABI research is needed
- no typed function pointer ABI is needed
- `parallel_do(...)` and `spawn(...)` share one executor backend

Waiting threads help execute queued tasks while waiting on a wait-group or
join-state. That avoids dead time and makes nested parallel usage less fragile.

## Compiler Work

The compiler needed one minimal general-purpose feature exposed through
`/std.abi`:

```cyan
/std.abi.fn_ptr(...)
```

Rules:

- returns `void*`
- only allowed in `unchecked`
- argument must be a visible function name
- explicit type arguments are allowed when the target is a generic function

Examples:

```cyan
fn_ptr(plus_one)
fn_ptr<T>(spawn_run_once)
```

Internally the compiler reserves `__builtin_fn_ptr(...)`, while user-facing
code reaches the feature through `/std.abi.fn_ptr(...)`. Codegen lowers the
call to the function symbol address, bitcast to `void*`.

This is enough for runtime callback registration. It is intentionally smaller
than adding first-class typed function pointer support.

The compiler does not know anything special about `spawn(...)`. It only knows
how to instantiate normal generic functions and how to materialize raw function
addresses for `/std.abi.fn_ptr(...)`.

## Language Changes

No new language syntax was required for phase 1.

What already existed and was sufficient:

- callable interfaces with `&T` and `&mut T` receivers
- interface composition
- nominal `local/send/share` markers
- generic function instantiation

That means:

- `run_once(&mut T)` is just another callable interface
- `spawnable = run_once + thread_sendable` is just another interface alias
- `spawn<T>(T task)` is just another generic stdlib function

The only new compiler-exposed surface is builtin lowering for
`/std.abi.fn_ptr(...)`, which is an unchecked escape hatch, not a new
type-system feature.

## Safety Boundary

The ownership split remains:

- `parallel_do(...)` requires `thread_shared`
- `spawn(...)` requires `thread_sendable`

That is the right boundary:

- borrowed fork-join work may share access
- owned spawned work must be movable across the thread boundary

Important current behavior:

- borrows are not `send`
- slices are not `send`
- so ordinary borrowed data is rejected by `spawn(...)`

`shared T*` is still `send`. That is acceptable for phase 1 because producing a
`shared T*` already requires `unchecked`. The unsafety stays at pointer
creation, not at `spawn(...)` itself.

## What This Deliberately Avoids

This plan does not do any of the following:

- no `spawn`-specific compiler lowering
- no hidden compiler-generated owned-task record type
- no "one OS thread per spawn call" implementation
- no first-class typed function pointer feature in phase 1
- no typed result-bearing `JoinHandle<T>` yet

Those can all wait.

## Remaining Work

Phase 1 is enough to make owned background work possible, but it is not the end
of the threading story.

Next steps:

- result transport primitives such as channels or futures
- possibly a later typed function-pointer feature if user code needs it
- possibly typed result-bearing join handles once the callable/result model is
  clearer

## Summary

The chosen design is:

- keep the native runtime
- keep `parallel_do(...)` borrowed and blocking
- add owned `spawn(...)` in stdlib, not in the compiler
- pass owned-task thunks into the runtime through `/std.abi.fn_ptr(...)`
- keep the compiler change minimal and general-purpose

That is the smallest change set that gives Cyan a real owned-task model without
turning `spawn(...)` into a special language hole.
