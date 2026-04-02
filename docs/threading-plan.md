# Threading Plan

This note records the current threading surface and the next design target.
The current implementation is intentionally narrow. The real design work now is
preparing a safe and useful `spawn` model on top of the pieces that already
exist.

## What Exists Now

The current concurrency MVP is in place:

- `shared T*`, atomics, `Rc<T>`, `Arc<T>`, and `Mutex<T>` exist
- `/std.thread` exports:
  - `interface<T> void run_task(&T task);`
  - `interface thread_shared = share - local;`
  - `interface thread_sendable = send - local;`
  - `interface threaded_runnable = run_task + thread_shared;`
  - `void parallel_do([]&threaded_runnable tasks);`
- interface composition is now part of the language, so the thread boundary is
  expressed in stdlib rather than through compiler special-casing
- nominal thread properties use:
  - `impl local(Type);`
  - `unchecked impl send(Type);`
  - `unchecked impl share(Type);`

This gives Cyan one good concurrency primitive today: blocking fork-join over
borrowed tasks.

## Current Limits

The current model still stops at blocking borrowed work:

- `parallel_do(...)` is immediate fork-join only
- there is no handle that can outlive the call
- there is no late `join`
- there is no dynamic set of spawned tasks
- there is no general owned-task `spawn`
- the runtime strategy is still narrow and not yet designed as a general task
  executor

That is acceptable for the MVP. It is not enough for a real thread API.

## Design Direction

The next step should not be another ad hoc helper beside `parallel_do(...)`.
It should be a second, clearly separate layer:

- `parallel_do(...)` stays the borrowed, blocking, lexical primitive
- `spawn(...)` becomes the owned, non-lexical primitive

That split matters because the two models have different safety boundaries.

`parallel_do(...)` works because the caller keeps ownership of every task value
and the call does not return until all work is done. The runtime may borrow the
task slice temporarily because the slice dies exactly when the call ends.

`spawn(...)` is different. Once the call returns, the runtime may still be
executing the task. That means the runtime must own the task object, its
dispatch metadata, and its completion state. Borrowed task values are not
enough.

## Safety Model For `spawn`

The current interface/property system is already close to what `spawn(...)`
needs.

The important distinction is now:

- `thread_shared = share - local`
- `thread_sendable = send - local`

This should stay the core rule:

- `parallel_do(...)` requires `thread_shared`
- future `spawn(...)` requires `thread_sendable`

That gives the language two separate thread boundaries:

- shared-access boundary
- ownership-transfer boundary

This separation is the main preparation already completed.

## Proposed `spawn` Contract

The first `spawn(...)` should use owned tasks and completion-only handles.

Recommended surface:

```cyan
interface<T> void run_once(&mut T task);
interface spawnable = run_once + thread_sendable;

struct JoinHandle {
    // opaque
};

JoinHandle spawn<T>(T task);
void join(&mut JoinHandle handle);
```

### Why A New `run_once`

`run_task(&T task)` is a good fit for borrowed fork-join work. It is less good
as the basis of owned spawned work.

For spawned tasks, `run_once(&mut T task)` is a better semantic contract:

- the runtime owns the task object
- the task runs exactly once
- mutation of owned task state during execution is natural
- the task object is dropped after completion

This keeps `parallel_do(...)` and `spawn(...)` from pretending to be the same
thing when they are not.

### Why `JoinHandle` Should Be Completion-Only First

A first `spawn(...)` does not need typed task results.

Today, Cyan callable interfaces carry one receiver type parameter and a fixed
return type. That is enough for `run_task` and `run_once`. It is not yet a good
basis for a generic "`spawn` returns `R`" protocol.

So the first phase should keep the model simple:

- the task owns whatever state it needs
- shared output can go through `Arc<T>`, `Mutex<T>`, atomics, or future
  channels/promises
- `JoinHandle` only tracks completion

Typed result-bearing spawn can come later once there is a clearer design for
generic task result protocols.

## Runtime Work Needed For `spawn`

This is the part that matters most. The current `parallel_do(...)` runtime ABI
is borrowed and blocking. It cannot be stretched into `spawn(...)` safely.

Today the runtime effectively receives a slice of interface values:

- data pointer
- dispatch pointer

That is enough for immediate execution. It is not enough for a detached owned
task.

`spawn(...)` needs a heap-owned task record with at least:

- pointer to the heap-allocated task payload
- dispatch entry for `run_once`
- destructor for the payload
- completion flag or latch
- storage for join state

Conceptually:

```text
TaskRecord {
    payload_ptr
    run_once_fn
    drop_fn
    state
}
```

The current interface fat pointer should remain the calling convention for
erased borrows. `spawn(...)` should use a different internal runtime record for
owned tasks.

## Executor Strategy

Preparing for `spawn(...)` is the right time to stop thinking in terms of
"special runtime for `parallel_do(...)`" and start thinking in terms of one
executor used by both layers.

The executor should eventually provide:

- a worker pool
- a queue for owned tasks
- a task-group or latch for blocking fork-join calls
- join state for `JoinHandle`

That lets both APIs share one substrate:

- `parallel_do(...)` submits borrowed work to a temporary task-group and waits
- `spawn(...)` submits owned work and returns a handle

This also fixes the current long-term runtime problem: repeatedly creating OS
threads for nested parallel work does not scale.

## Relationship Between `parallel_do` And `spawn`

These APIs should stay distinct in semantics even if they share a runtime.

`parallel_do(...)` should continue to mean:

- task values may borrow from the caller
- the caller blocks until all tasks complete
- no handle escapes the call

`spawn(...)` should mean:

- the task value is moved into runtime ownership
- execution may outlive the call site
- the caller gets an explicit completion handle

Trying to collapse these into one polymorphic API would blur the boundary that
actually keeps the model understandable.

## What Not To Do Next

The next step should not be:

- a second `parallel_do2(...)`
- a fake `spawn(...)` implemented as "create one OS thread per call"
- a borrowed `spawn(...)` without real runtime ownership
- a handle API that secretly still depends on caller stack storage

Those would spend complexity without establishing the right model.

## Recommended Stages

### Stage 1: Keep Current Borrowed Surface Stable

Do not grow `parallel_do(...)` beyond its current role.

### Stage 2: Introduce An Executor

Refactor the runtime around:

- worker pool
- queue
- task-group/latch
- owned task record

`parallel_do(...)` may continue to expose the same API while switching to the
new backend.

### Stage 3: Add Owned Spawn

Add:

- `run_once`
- `spawnable`
- `JoinHandle`
- `spawn`
- `join`

Keep it completion-only.

### Stage 4: Add Result-Carrying Concurrency Primitives

Once owned spawn exists, build higher-level result transport on top:

- channels
- promises/futures
- typed result handles if the language grows the right abstraction for them

### Stage 5: Revisit Scoped Threads Only If Needed

After owned `spawn(...)` exists, reconsider whether Cyan still needs a more
expressive scoped-thread API. It may turn out that:

- `parallel_do(...)` handles borrowed fork-join well enough
- owned `spawn(...)` handles long-lived background work well enough

If so, there may be no need to add a third model immediately.

## Summary

The important part that is already done is small:

- thread boundaries are now expressed in stdlib with interface composition
- `local/send/share` exist as nominal thread properties
- `parallel_do(...)` is a working borrowed fork-join primitive

The important part that should come next is larger:

- treat `spawn(...)` as an owned-task model, not an extension of borrowed
  `parallel_do(...)`
- introduce `run_once + thread_sendable`
- add an opaque `JoinHandle`
- build one executor substrate for both blocking fork-join and owned spawned
  work
