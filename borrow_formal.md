# Borrow Checker Model

## 1. Static Domains

Let:

- `kappa ::= shr | mut`
- `rho ::= local(id) | external(id)`
- `pi ::= epsilon | f :: pi | [*] :: pi | payload(v) :: pi`
- `p ::= (rho, pi)`

`p` is a resolved place. `f` is a struct-field index, `[*]` is the whole-index
sentinel used for array/slice element overlap, and `payload(v)` denotes the
payload path for enum variant `v`.

Types are partitioned into:

- owned types `O`
- borrow-like view types `Vb ::= &T | &mut T | &Interface`
- slice view types `Vs ::= []T`

The checker also tracks aggregates that contain view leaves.

## 2. Local State

Each local is a record:

`L = (type, status, scope_depth, in_scope, origin?, element_origins, parent?)`

where:

- `status ::= Uninitialized | Live | Moved`
- `origin?` is the tracked source place for a top-level borrow/slice leaf
- `element_origins` is the tracked source set for direct shared-view slices
- `parent?` is the suspended mutable-borrow parent during mutable reborrow

The function-state is:

`Sigma = (Locals, TempLoans, Scopes, Loops, Reachable, UncheckedDepth)`

## 3. Overlap

Define `overlap(p1, p2)` iff:

1. `p1` and `p2` have the same root kind and root id, and
2. one field path is a prefix of the other.

Because the checker uses a single `[ * ]` sentinel for indexing, any two
indexed subplaces of the same base overlap.

## 4. Active Loans

The active named-loan set `Named(Sigma)` contains one loan for each in-scope
live borrow-like or slice local:

- for ordinary borrows/interfaces: one loan on `origin`
- for direct shared-view slices: one shared loan for each `element_origins`
- for ordinary slices: one shared loan on `origin`

Temporary loans `Temp(Sigma)` are statement-scoped loans created by explicit
borrows, borrow arguments, borrow-based `switch` scrutinees, and temporary
borrow-returning call results.

Mutable reborrow additionally suspends the parent mutable borrow local until
statement cleanup or child-scope exit.

## 5. Core Judgements

We write:

- `Sigma |-read p`
- `Sigma |-write p ignore i`
- `Sigma |-borrow(kappa, p) ignore i`
- `Sigma |-outlives src > dst`

### Read

`Sigma |-read p` requires the root local of `p` to be `Live` whenever `p` is
local.

### Write

`Sigma |-write p ignore i` holds iff `p` overlaps no loan in
`Named(Sigma) union Temp(Sigma)`, except loans owned by local `i`.

### Borrow

`Sigma |-borrow(kappa, p) ignore i` holds iff for every overlapping loan `l`
in `Named(Sigma) union Temp(Sigma)` that is not owned by `i`:

- `kappa = shr` and `l` is shared, or
- the borrow is rejected.

Equivalently, any overlap involving a mutable side is forbidden.

### Outlives

`Sigma |-outlives src > dst` holds when either `src` is external, or:

- `src` is `Live`
- `src.scope_depth < dst.scope_depth`, or
- `src.scope_depth = dst.scope_depth` and `src` is declared early enough to be
  dropped after `dst`

For aggregate view slots, `dst` is interpreted as the owning visible local.

## 6. Derived Rules

### Named Borrow Creation

Creating or reassigning a named borrow local requires:

1. the source expression resolves to a borrow-compatible source place
2. source-type compatibility (`&T` may reborrow from `&mut T`, `&mut T` may
   only come from a mutable source)
3. `Sigma |-borrow(kappa, source)`
4. `Sigma |-outlives source > target_local`

If the new borrow is a mutable reborrow from a mutable borrow local, the child
records that parent and the parent becomes `Moved`.

### Move

Moving a value:

1. is trivial for copy types
2. requires a whole local place for move types
3. marks the root local `Moved`

Partial moves are rejected.

### Return Dependencies

For each returned borrow/slice leaf, a `depends(return_path on param_path)`
entry specifies the parameter-owned source leaf that the returned leaf must be
equal to or a subplace of.

This is checked at:

- function definition time, for completeness and shape agreement
- each `return`, by comparing actual leaf sources to declared ones
- each call, by propagating source bindings from arguments to result leaves

## 7. Safety Invariants

For the safe fragment (`UncheckedDepth = 0`), the checker maintains:

1. If a local view leaf is `Live`, every tracked source place for that leaf is
   also live.
2. No two simultaneously active overlapping loans are mutable on either side.
3. A write is only accepted when it overlaps no active loan except the owner
   loan through which the write occurs.
4. A moved local is never read until a whole-local write restores it to `Live`.
5. A named view local never outlives any of its tracked source places.

## 8. Soundness Statement

For the safe fragment, let `ok(stmt, Sigma)` mean semantic analysis accepts
`stmt` from state `Sigma`, producing `Sigma'`.

Then:

`ok(stmt, Sigma) => Invariants(Sigma')`

and every accepted read/write/borrow step preserves the no-conflicting-loans
property above.

This is proved by induction over the statement/expression forms:

- borrow creation uses `ensureCanBorrow(...)` and `ensureViewSourceOutlivesLocal(...)`
- writes use `ensureCanWrite(...)`
- moves use `consumeValue(...)`
- merges require identical move/borrow metadata across incoming states
- loop back-edges and `continue` paths are validated against the loop-entry
  state

The proof obligation that mattered in practice is that control-transfer checks
must run on the post-cleanup state, not the raw in-body state.

