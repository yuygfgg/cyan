# Threading Plan

This note records the current concurrency MVP and the immediate library surface
to build on top of it.

## What Exists Now

The compiler now provides the low-level pieces needed to build threading and
concurrency primitives in Cyan code:

- `shared T*` as a contextual pointer qualifier
- owner-bound borrow casts:
  `ptr as &T on owner` and `ptr as &mut T on owner`
- atomic memory-order constants:
  `atomic_relaxed()`, `atomic_acquire()`, `atomic_release()`,
  `atomic_acq_rel()`, `atomic_seq_cst()`
- atomic operations:
  `atomic_load`, `atomic_store`, `atomic_exchange`,
  `atomic_compare_exchange`, `atomic_fetch_add`, `atomic_fetch_sub`,
  `atomic_fetch_and`, `atomic_fetch_or`, `atomic_fetch_xor`, `atomic_fence`
- nominal thread-affinity markers:
  `impl local(Type);`, `unchecked impl share(Type);`, and generic forms such as
  `impl local<T>(Rc<T>);`
- task-boundary checking for `/std.thread.run_task` that rejects task receiver
  types which recursively contain a `local` type or any field shape that is
  not safe to share across threads

These are enough to implement the internal mechanics of concurrency-aware
stdlib code such as:

- low-level runtime shims for OS threads and synchronization objects
- blocking fork-join helpers
- first-pass pointer-backed concurrency primitives such as `Rc<T>`, `Arc<T>`,
  and `Mutex<T>`

The current goal is to give Cyan enough built-in semantics to construct
concurrency libraries with only a thin runtime shim where the host ABI makes
that unavoidable.

## What Is Explicitly Not Solved Yet

This MVP does not claim to solve the full safe threading model.

The missing pieces are still:

- general scoped threads
- safe thread-boundary checking for high-level `spawn`
- semantic thread-affinity types such as GL contexts or epoll handles
- a first-class distinction between "movable to another thread" and
  "shareable across threads"
- an optimizer-visible interior-mutability marker comparable in role to
  `UnsafeCell`

## Immediate Stdlib Surface

The first stdlib thread API should stay narrow:

- `/std.thread` exports:
  - `interface<T> void run_task(&T task);`
  - `void parallel_do([]&run_task tasks);`
- `/std.rc` exports:
  - `Rc<T>`, `rc_new`, `rc_clone`, `rc_get`
- `/std.sync` exports:
  - `Arc<T>`, `arc_new`, `arc_clone`, `arc_get`
  - `Mutex<T>`, `MutexGuard<T>`, `mutex_new`, `lock`
- `parallel_do(...)` is blocking. It MUST NOT return until every submitted task
  has completed.
- The runtime may create or schedule worker threads internally, but that
  detail stays behind the library boundary.
- The primary use case is zero-copy fork-join over stack-borrowed read-only
  data and similar partitioned work.

This one API shape is intentionally the whole immediate design. There is no
separate `parallel_do2`, no scope object, and no general thread-handle API at
this stage.

The owner-bound borrow cast exists for the lower layers that follow from this
surface later, such as pointer-backed synchronization types that want returned
borrows to remain tied to an outer owner value for alias tracking.

## Blocking Fork-Join Instead Of Scoped Threads

This blocking fork-join combinator covers the highest-value scoped-thread use
case without needing a full lexical thread-scope model.

The key property is that all task borrows stay inside one ordinary call:

- the caller packages tasks into one `[]&run_task` slice
- the function blocks until all worker threads have completed
- once the call returns, those borrows are over

This is enough to express zero-copy parallel reads over stack-owned data.
It also gives a direct substrate for higher-level library helpers such as a
future `parallel_for(...)` built on top of task slicing.

The performance profile depends heavily on the runtime strategy. A naive
implementation that creates fresh OS threads on every `parallel_do(...)` call
is only suitable for coarse top-level jobs. Recursive divide-and-conquer needs
a cheaper substrate, for example:

- a fixed worker pool
- a latch or small task-group object per blocking call
- a policy of enqueueing one task and running the other on the current thread

That keeps the surface blocking and lexical while avoiding thread explosion in
nested fork-join code.

However, this remains intentionally narrower than full scoped threads:

- no handles that live past the immediate call
- no late join
- no parent work overlapping with child work after dispatch
- no dynamic thread sets built incrementally across a lexical region
- no general API for "spawn now, join later inside the same scope"

If that extra expressive power becomes necessary later, a lexical scope model
can still be added on top. This note does not commit to any scoped-thread API
beyond the blocking `parallel_do(...)` shape above.

## Current Thread Boundary Rule

The current MVP has a negative nominal marker, `local`, and a positive escape
 hatch, `share`.

Type authors can mark semantically thread-affine values directly:

```cyan
struct GLContext {
    i64 handle;
};

impl local(GLContext);
```

Generic library wrappers can also opt into the same rule:

```cyan
impl local<T>(Rc<T>);
```

When a value is coerced to `/std.thread.run_task`, the compiler now applies two
checks:

- it rejects any reachable nominal type marked `local`
- it requires the whole receiver type to be structurally share-safe

The structural rule is currently:

- scalars are share-safe
- borrows, slices, and arrays inherit the element result
- plain raw pointers are not share-safe
- `shared T*` is share-safe only if `T` is share-safe
- structs and enums are share-safe only if every reachable field or payload is
  share-safe
- `unchecked impl share(Type);` can opt a nominal type into the share-safe set directly

This still does not implement a separate `send` dimension for future
move-to-thread APIs such as `spawn`.

## Library Status

The first `Rc`, `Arc`, and `Mutex` wrappers now have real drop hooks.

The current library/runtime surface covers:

- construction and borrowing shape
- atomic operations and locking shape
- blocking `parallel_do(...)`
- task-boundary rejection for `local` or non-share-safe values
- final reclamation of `Rc`, `Arc`, and `Mutex` payload storage

## Why `shared` Stayed Narrow

`shared` is currently only a pointer qualifier.

It is not yet a general type property because that would immediately force the
language to answer harder questions:

- who can declare a nominal type thread-shareable
- how generic constraints are written
- how semantic non-thread-safe handles override structural shape
- how move-only and shareable dimensions interact

Restricting `shared` to `shared T*` keeps the MVP useful without prematurely
committing to the full nominal model.

## Optimizer Contract

The current LLVM lowering is still conservative: borrows and pointers lower to
plain `ptr`, and the backend is not yet attaching aggressive alias metadata
such as `readonly`, `noalias`, or invariant load assumptions.

However, once the compiler starts feeding stronger borrow facts into LLVM, Cyan
will need an explicit interior-mutability marker in the language or IR model.
That should be designed before any aggressive alias-based optimization pass is
added on top of shared borrows.
