# Threading Plan

This note records the current concurrency MVP and the next design steps.

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

These are enough to implement the internal mechanics of types such as:

- `Rc<T>`
- `Arc<T>`
- `Mutex<T>`
- thread handles and join handles
- low-level runtime shims for OS threads and synchronization objects

The current goal is to give Cyan enough built-in semantics to construct
concurrency libraries without pushing those semantics into C shims.

## What Is Explicitly Not Solved Yet

This MVP does not claim to solve the full safe threading model.

The missing pieces are:

- scoped threads
- safe thread-boundary checking for high-level `spawn`
- semantic thread-affinity types such as GL contexts or epoll handles
- a first-class distinction between "movable to another thread" and
  "shareable across threads"
- an optimizer-visible interior-mutability marker comparable in role to
  `UnsafeCell`

## Immediate Stdlib Direction

The expected next library work is:

- add thread runtime bindings for create, dispatch, join, yield, and sleep
- implement `Rc<T>` on raw `T*`
- implement `Arc<T>` on `shared T*` plus atomics
- implement `Mutex<T>` on `shared` runtime state plus owner-bound borrow casts
- keep OS interaction and allocation details in stdlib/runtime code, not in the
  language surface

The owner-bound borrow cast exists for exactly this style of code: internal
pointer-backed abstractions that want their returned borrows to remain tied to
an outer owner value for alias tracking.

## Next Language Step: Scoped Threads

Scoped threads remain the biggest missing capability.

Without them, parallel read-only work over stack-owned data has avoidable
costs:

- deep copies
- heap promotion into `Arc`
- extra reference-count traffic

Before a full lexical scope model, there is a much smaller blocking fork-join
step that covers the highest-value use case.

The narrow shape is a stdlib combinator such as:

- `thread.parallel_do(&task_a, &task_b)`

where each argument is an erased task borrow or a generic task value with a
known `run_task(...)` implementation. The library may create worker threads
internally, but it MUST join them before `parallel_do` returns.

This is much easier than a general scoped-thread model because the borrow
lifetime does not escape the call site:

- the caller lends borrows into one ordinary function call
- the function blocks until all worker threads have completed
- once the call returns, those borrows are over

This is enough to express zero-copy parallel reads over stack-owned data and
similar fork-join patterns over disjoint mutable partitions.

The performance profile depends heavily on the runtime strategy. A naive
implementation that creates fresh OS threads on every `parallel_do(...)` call
is only suitable for coarse top-level jobs. Recursive divide-and-conquer needs
a cheaper substrate, for example:

- a fixed worker pool
- a latch or small task-group object per blocking call
- a policy of enqueueing one task and running the other on the current thread

That keeps the surface blocking and lexical while avoiding thread explosion in
nested fork-join code.

However, it is intentionally narrower than full scoped threads:

- no handles that live past the immediate call
- no late join
- no parent work overlapping with child work after dispatch
- no dynamic thread sets built incrementally across a lexical region
- no general API for "spawn now, join later inside the same scope"

This blocking combinator also does not solve nominal thread-affinity concerns.
Without future tags such as `@local`, values that are structurally harmless but
semantically thread-bound cannot yet be rejected at the API boundary.

The preferred immediate step is therefore:

- add one or more blocking fork-join combinators in stdlib/runtime
- use them for stack-borrowing parallel read and partitioned-write workloads
- postpone a general scope object or scope construct until the narrower shape
  proves insufficient

If the blocking combinator becomes common, the likely ergonomic surface is a
small family rather than one universal primitive:

- `parallel_do(a, b)` for void tasks
- fixed-arity variants such as `parallel_do3(...)` if needed
- higher-level data-parallel helpers such as `parallel_for(...)` built on top

The generic or fixed-arity forms may also be a better fit than a homogeneous
task slice, because they avoid forcing the caller to erase every task into an
interface container up front.

The broader model still points toward a lexical scope design once that extra
expressive power is needed.

The shape to pursue is:

- a scope object or scope construct that defines the lifetime of a thread set
- spawned scoped threads whose handles cannot escape that scope
- a rule that the scope must complete only after all spawned threads have
  joined
- borrow-based argument passing into scoped threads, so stack-owned read-only
  and disjoint mutable partitions can be expressed without heap indirection

This fits Cyan better than importing Rust lifetimes directly, because it keeps
the control structure lexical and explicit.

## Next Language Step: Nominal Thread Properties

The current MVP intentionally avoids pretending that thread safety is a purely
structural property.

The next design should add nominal properties or tags so type authors can state
semantic threading constraints directly.

Examples of the intended direction:

- `@local` or an equivalent marker for thread-affine values
- explicit type-level declarations for "movable to another thread"
- explicit type-level declarations for "shareable across threads"
- conditional declarations for generic wrappers, for example a future `Arc<T>`
  saying its thread properties depend on `T`

Important constraint: this should not force every ordinary type to opt in by
default with Java-style boilerplate. The common case should stay lightweight,
even if automatic inference might cause unsafe code.

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
