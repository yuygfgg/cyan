# Cyan Tutorial

**Cyan is a compiled systems language designed for explicit data layouts, borrowed views, zero-copy parsers, and safe C-interoperability.**

This tutorial introduces the language through practical, step-by-step explanations of its core mechanics. It is designed to be read sequentially. The snippets discussed here live in `docs/snippets/`. You can verify any standalone snippet by running `cyan <file> --check`.

---

## 1. Introduction & Quick Start

Cyan requires functions to declare explicit parameter types and return types. A typical entry point `main` returns an integer exit code, where `0` indicates success and any non-zero value indicates an error.

```cyan
i64 add(i64 lhs, i64 rhs) {
    return lhs + rhs;
}

i64 main() {
    i64 value = add(20, 22);
    if (value == 42) {
        return 0;
    }
    return 1;
}
```

### Running the Code

You can interact with Cyan files using the CLI tool:
```sh
# Type-check the file without generating an executable
cyan docs/snippets/hello_cyan.cyan --check

# Compile to a native object file
cyan docs/snippets/hello_cyan.cyan -o /tmp/hello_cyan.o

# Emit LLVM IR for inspection
cyan docs/snippets/hello_cyan.cyan --emit-llvm -o /tmp/hello_cyan.ll
```

### Explicitness and Inference

In Cyan, variables are explicitly typed (`i64 value = ...`). While the compiler does support type inference, it requires sufficient context to deduce the type.

```cyan
T make_default<T>() {
    return 0;
}

i64 main() {
    return make_default();
}
```
If you compile this, the compiler will reject it with `could not infer type argument for 'T'`. Because `make_default()` takes no arguments, the compiler has no clues about what `T` should be. In these situations, you must provide the type explicitly.

---

## 2. Basic Types

Cyan provides a precise vocabulary for primitive types, ensuring you always know the exact memory footprint of your data.

| Type           | Meaning                                                                        |
| -------------- | ------------------------------------------------------------------------------ |
| `void`         | No value. Used for functions that return nothing or as the pointee in `void*`. |
| `bool`         | Boolean truth value, `true` or `false`.                                        |
| `char`         | One byte. Used for characters and raw byte-oriented text.                      |
| `i8` to `i128` | Signed integers of specific bit-widths.                                        |
| `u8` to `u128` | Unsigned integers of specific bit-widths.                                      |
| `f32`, `f64`   | Floating-point numbers.                                                        |

### Integer Literals and Context

When you write an integer literal like `1`, Cyan determines its type based on the surrounding code. 

```cyan
i128 big = 1;
u128 wide = 340282366920938463463374607431768211455 as u128;
i64 defaulted = 1 + 2;
```

If there is no explicit type context (like the `defaulted` variable assignment), unsuffixed integer literals fall back to `i64`. Negative integers are written with a unary `-`. Notice that the minimum value of an `i128` (`-2^127`) is one step larger in magnitude than its maximum positive value (`2^127 - 1`). If you need the positive value `2^127`, you must use `u128`.

---

## 3. Composite Types & Memory Views

One of the most important concepts in systems programming is understanding exactly *who* owns a piece of memory. Cyan enforces a strict distinction between **owned data** and **views** over that data.

### Arrays (Owned) vs Slices (Views)

An array (`T[N]`) is a contiguous block of memory owned by the variable holding it. A slice (`[]T`) is a "view" or a "window" into a contiguous block of memory owned by someone else. Slices consist of a pointer and a length, and creating a slice **never** copies the underlying data.

```cyan
struct Pair {
    i64 left;
    i64 right;
};

i64 sum([]i64 values) {
    i64 i = 0;
    i64 total = 0;
    while (i < len(values)) {
        total = total + values[i];
        i++;
    }
    return total;
}

i64 main() {
    Pair pair = {3, 4};
    
    // 'values' is an array. It owns these 4 integers on the stack.
    i64[4] values = [pair.left, pair.right, 5, 6];
    
    // 'tail' is a slice. It points to the last 3 elements of 'values'.
    []i64 tail = subslice(values, 1, len(values) - 1);
    
    // 'sum' accepts a slice, so it can operate on 'tail' without copying.
    i64 total = sum(tail);

    for (i64 i = 0; i < 2; i++) {
        total++;
    }

    return total - 17;
}
```

In this example:
- `Pair pair = {3, 4};` demonstrates positional struct initialization. The fields are assigned in the order they are declared.
- `subslice(values, 1, len(values) - 1)` creates a view starting at index `1`. 

Because a slice is merely a view, it **must not** outlive the data it points to. If a function attempts to return a slice pointing to a local array that is about to be destroyed, the compiler will catch the error and reject the code.

### String Literals and Slices

In Cyan, string literals are stored in static memory and naturally end with a null terminator (`\0`). When you assign a string literal to a slice, the slice covers the **entire** literal storage, including that null byte.

```cyan
import /std.slice as slice;

i64 main() {
    // 'text' has a length of 6, not 5! It includes the '\0'.
    []const char text = "hello";
    
    // Create a sub-slice spanning the characters 'e', 'l', 'l'.
    []const char part = slice.span_slice(text, 1, 3);
    
    if (slice.span_len(text) == 6 && slice.span_len(part) == 3 &&
        part[0] == 'e' && part[2] == 'l') {
        return 0;
    }
    return 1;
}
```

If you need a slice that exactly covers the visible text without the terminator, you must explicitly subslice it: `[]const char name = subslice("cyan", 0, 4);`.

---

## 4. Modules & Builtins

Cyan uses explicit import paths to bring code from other files or the standard library into scope. 

- **Local imports** use relative dotted paths based on the project's directory structure.
- **Builtin imports** are prefixed with a `/` and provide access to the standard library.

`docs/snippets/modules_demo.cyan`:
```cyan
import tutorial_support.stats as stats;

i64 main() {
    return stats.meaning() - 42;
}
```

`docs/snippets/tutorial_support/stats.cyan`:
```cyan
// The 'export' keyword makes this function visible to importers.
export i64 meaning() {
    return 42;
}
```

---

## 5. Ownership & Borrowing

When passing data to functions or sharing it across scopes, copying entire structures can be expensive. Cyan uses **borrowing** to allow temporary access to data without transferring ownership. 

Cyan enforces strict aliasing rules at compile time to prevent data races and logical inconsistencies:
- **`&T` (Shared Borrow)**: You can have multiple read-only borrows of a value at the same time. You cannot modify the data through a shared borrow.
- **`&mut T` (Mutable Borrow)**: You can have exactly **one** active mutable borrow of a value at a time. While it exists, no other borrows (shared or mutable) are allowed.

```cyan
i64 main() {
    i64 value = 0;
    {
        // We create an exclusive mutable borrow of 'value'.
        &mut i64 first = &value;
        {
            // We temporarily reborrow 'first'. 
            // 'first' cannot be used while 'second' is active.
            &mut i64 second = first;
            *second = 1;
        }
        // 'second' is out of scope. 'first' is active again.
        *first = 2;
    }
    // 'first' is out of scope. The original 'value' owner can be read again.
    return value;
}
```

### Compiler Protections

If you attempt to write to a shared borrow:
```cyan
i64 main() {
    i64 value = 0;
    &i64 alias = &value;
    *alias = 3; // ERROR: cannot write to a borrowed place
    return value;
}
```
The compiler blocks this because `&i64` represents a strictly read-only contract.

---

## 6. Enums & Pattern Matching

Enums in Cyan are "tagged unions" that can carry payloads of different types. You inspect the contents of an enum using a `switch` statement.

Because payloads reside inside the enum, Cyan requires you to specify your ownership intent when matching. You can either borrow the enum (to peek at the payload) or move the enum (to consume it and take ownership of the payload).

```cyan
enum Option<T> {
    None,
    Some(T),
};

i64 read_by_borrow(Option<i64> value) {
    // By matching on '&value', the 'payload' is bound as a borrow (&i64).
    switch (&value) {
        case None:
            return 0;
        case Some(payload):
            return *payload; // Dereference the borrow to get the value
    }
}

i64 read_by_move(Option<i64> value) {
    // By matching on 'move value', the enum is consumed. 
    // 'payload' is bound as an owned value (i64).
    switch (move value) {
        case None:
            return 0;
        case Some(payload):
            return payload; // Return the owned value directly
    }
}
```

If you need to modify the payload in place, you can match on a mutable borrow (`switch (&mut value)`). Note that `switch` statements in Cyan must be exhaustive; if you handle every declared variant, adding a `default:` case will trigger a `default case is unreachable` error.

---

## 7. Zero-Copy Provenance: `depends`

This is Cyan's most distinctive feature. In C, zero-copy parsers often rely on raw pointers and programmer discipline to ensure that a parsed view (like a substring) doesn't outlive the original buffer. 

In Cyan, this relationship is tracked formally by the compiler using the **`depends`** clause. When a function returns a view (a borrow or a slice), the signature must explicitly declare which input parameter owns the memory that the returned view points to. This allows the compiler to enforce lifetimes across function boundaries.

### Single Return Views

```cyan
struct Split {
    []const char head;
    []const char tail;
};

// The compiler knows that every slice inside the returned 'Split' struct 
// points to the memory owned by the 'text' parameter.
Split split_at([]const char text, i64 mid)
    depends(return on text) {
    
    if (mid <= 0) {
        return {subslice(text, 0, 0), text};
    }
    if (mid >= len(text)) {
        return {text, subslice(text, 0, 0)};
    }
    return {subslice(text, 0, mid), subslice(text, mid, len(text) - mid)};
}
```

When you call `split_at(buffer, 5)`, the compiler now understands that the returned `Split` struct cannot be used after `buffer` goes out of scope or is modified.

### Mutable Output Parameters

The same mechanism applies when a function writes a view into an output parameter.

```cyan
struct View {
    []const char data;
};

// This signature guarantees that the view written into 'out.data' 
// originates from the 'text' parameter.
void take_tail(&mut View out, []const char text, i64 mid)
    depends(out on text) {
    
    Split parts = split_at(text, mid);
    out.data = parts.tail;
}
```

### Precise Dependency Paths

When a function takes multiple inputs and returns an aggregate structure containing multiple views, a blanket `depends(return on text)` might not be expressive enough. You must map each leaf view in the returned struct to its specific source.

```cyan
struct PairRefs {
    &i64 left;
    &i64 right;
};

// We declare precisely which field derives from which parameter.
PairRefs pair_refs(&i64 x, &i64 y)
    depends(return.left on x, return.right on y) {
    return {x, y};
}
```
If you altered the body of this function to `return {y, x};`, the compiler would immediately reject it with `returned borrow does not match depends`, because the implementation violated the structural promise made in the signature.

You can also use a baseline with overrides for complex structures:
```cyan
Bundle build([]const char text, []const char special)
    depends(return on text, return.pair.right on special) {
    // Body logic here...
}
```
This tells the compiler: "Assume all views in the return value come from `text`, except for the specific sub-field `pair.right`, which comes from `special`."

---

## 8. Interfaces & Polymorphism

Instead of class-based inheritance, Cyan uses interfaces to define capabilities. If a type implements the methods required by an interface, it can be used wherever that interface is expected.

```cyan
// We define an interface capability called 'measure'.
interface<T> i64 measure(&T value);

struct Box {
    i64 value;
};

// 'Box' implements 'measure'.
impl measure(&Box box) {
    return box.value;
}

// 'call_dyn' accepts an "erased borrow". It doesn't know the exact type,
// only that the type implements the 'measure' capability.
i64 call_dyn(&measure value) {
    return measure(value);
}

i64 main() {
    Box right = {8};
    return call_dyn(&right) - 8;
}
```

### Interface Composition

Capabilities can be bundled or excluded using `+` and `-`.

```cyan
interface<T> i64 read(&T value);
interface<T> i64 seek(&T value);

// 'streaming' represents types that can be read, but CANNOT be seeked.
interface streaming = read - seek;

// 'readable_seekable' represents types that can be both read and seeked.
interface readable_seekable = read + seek;
```

### Custom Formatting

You can make your custom structs printable by implementing the standard `fmt` interface, which integrates directly with `/std.println`.

```cyan
import /std.println;

struct Point {
    i64 x;
    i64 y;
};

impl fmt(&Point value, &mut StringBuilder out) {
    fmt_write_cstr(out, "Point(");
    fmt_write_i64(out, value.x);
    fmt_write_cstr(out, ", ");
    fmt_write_i64(out, value.y);
    fmt_write_char(out, ')');
}

i64 main() {
    Point point = {3, 4};
    println("point={}", [point]); // Outputs: point=Point(3, 4)
    return 0;
}
```

---

## 9. Unchecked Operations & Raw Memory

Systems programming sometimes requires stepping outside the compiler's strict safety guarantees—whether to interface with C libraries (`extern`), perform raw pointer arithmetic, or execute manual memory allocations. 

In Cyan, all such operations must be wrapped in an `unchecked` block. This acts as an explicit boundary between safe, compiler-verified code and raw memory manipulation.

```cyan
extern {
    void* malloc(u64 n);
    void free(void* p);
}

struct IntBox {
    i64* ptr; // A raw pointer, not a borrow.
};

impl drop(&mut IntBox self) {
    // Casting and calling C functions must happen in unchecked blocks.
    unchecked {
        free(self.ptr as void*);
    }
}

IntBox new_box(i64 value) {
    unchecked {
        i64* ptr = malloc(sizeof(i64));
        *ptr = value;
        return {ptr};
    }
}
```

If you try to index into a raw pointer (`ptr[0]`) or cast away `const` outside of an `unchecked` block, the compiler will refuse to compile it. 

To acquire a null pointer, use the standard library helper `ptr.null<T>()`, making sure to explicitly provide the generic type since the compiler cannot infer it from an empty invocation.

---

## 10. Concurrency

Cyan provides robust abstractions for safe concurrent execution. It utilizes a system of thread boundary markers (`local`, `send`, `share`) to ensure that data races and invalid cross-thread accesses are caught at compile time.

### 10.1 Fork-Join Tasks (`parallel_do`)

`parallel_do` is used for scoped concurrency: it launches a batch of tasks and blocks the current thread until all tasks complete. Because the execution is fully scoped, it is mathematically guaranteed that the worker threads will finish before the caller's stack frame is destroyed. This means tasks can safely process **borrows** (`&T`) of local variables.

```cyan
import /std.sync;
import /std.thread;

struct AddTask {
    &Mutex<i64> total; // A borrow of a shared Mutex
    i64 value;
};

impl run_task(&AddTask task) {
    // Acquire the lock. While 'guard' is in scope, we have exclusive access.
    MutexGuard<i64> guard = lock(task.total);
    *guard.data = *guard.data + task.value;
    // The lock is automatically released when 'guard' goes out of scope.
}

i64 main() {
    Mutex<i64> total = mutex_new(0);
    AddTask left = {&total, 4};
    AddTask middle = {&total, 7};
    
    // Executes tasks concurrently. Blocks until both are done.
    parallel_do([left, middle]);

    return 0;
}
```

For `parallel_do` to accept a task, the task structure must satisfy the `thread_shared` marker, ensuring it does not contain thread-bound (`local`) resources.

### 10.2 Spawned Tasks (`spawn`)

If you want a background task that outlives the caller's scope, you use `spawn`. Because the caller might exit before the task finishes, you **cannot** pass borrows to a spawned task. Instead, the task must take ownership of its data.

For shared mutable state across spawned tasks, you use `Arc<T>` (Atomic Reference Counting) to share ownership of heap-allocated data.

```cyan
import /std.sync;
import /std.thread;

struct SharedState {
    Mutex<i64> total;
};

struct AddTask {
    Arc<SharedState> state; // Shared heap ownership, no borrows!
    i64 value;
};

// Spawned tasks implement 'run_once' (indicating consumption).
impl run_once(&mut AddTask task) {
    &SharedState state = arc_get(&task.state);
    MutexGuard<i64> guard = lock(&state.total);
    *guard.data = *guard.data + task.value;
}

i64 main() {
    Arc<SharedState> root = arc_new({mutex_new(0)});

    AddTask left = {arc_clone(&root), 4};
    
    // spawn returns a JoinHandle. Ownership of 'left' transfers to the runtime.
    JoinHandle a = spawn(left);
    
    // We wait for the task to complete.
    join(&mut a);

    return 0;
}
```

### 10.3 Thread Boundaries and Atomics

The compiler uses marker capabilities to govern what types can cross thread boundaries:
- **`local`**: Data tied to a specific thread (e.g., an OpenGL context or UI widget). It cannot be sent or shared across threads.
- **`send`**: Data that can safely be moved (transferred ownership) to another thread. Required by `spawn`.
- **`share`**: Data that can be safely accessed concurrently by multiple threads. Required by `parallel_do`.

If you try to pass an ordinary raw pointer (`i64*`) across a thread boundary, Cyan will block it because a raw pointer `does not satisfy marker 'share'`.

If you are implementing low-level lock-free coordination, you must explicitly annotate your pointers as `shared T*`. Atomic operations exclusively accept this `shared` qualifier:

```cyan
i64 counter = 0;
shared i64* total;
unchecked {
    total = (&mut counter) as shared i64*;
}
// This succeeds because 'total' is explicitly typed as a 'shared' pointer.
atomic_fetch_add(total, 1, atomic_seq_cst());
```