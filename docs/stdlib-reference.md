# Cyan Stdlib Reference

This document lists the public modules shipped in Cyan's standard library and the exports you are expected to use directly. Internal helpers inside a module are omitted unless they are part of the public surface.

## Quick Map

| Module             | Use it for                                               |
| ------------------ | -------------------------------------------------------- |
| `/std.view`        | slice length, subslices, raw slice pointer access        |
| `/std.ptr`         | null pointers                                            |
| `/std.mem`         | `memcpy`, `memset`, `memcmp`, typed copy/fill            |
| `/std.heap`        | heap allocation and typed wrappers                       |
| `/std.cstr`        | C string length and compare                              |
| `/std.file`        | `FILE*`-style I/O                                        |
| `/std.strconv`     | text to number, number to text                           |
| `/std.ryu`         | low-level float formatting                               |
| `/std.println`     | formatted output and custom formatting                   |
| `/std.atomic`      | atomic loads, stores, RMW operations, fences             |
| `/std.sync`        | `Arc<T>`, `Mutex<T>`, `MutexGuard<T>`                    |
| `/std.thread`      | spawn threads, join, parallel work over interface values |
| `/std.abi`         | ABI helpers                                              |
| `/std.rc`          | single-thread reference counting                         |
| `/std.math`        | scalar math functions                                    |
| `/std.math.intrin` | raw LLVM math intrinsics                                 |

## `/std.view`

Purpose: work with slices after importing the module.

Public API:

- `len<T>([]T values) -> i64`
- `subslice<T>([]T values, i64 start, i64 count) -> []T`
- `raw_data<T>([]T values) -> const void*`

Typical use:

```cyan
import /std.view;

i64 main() {
    []const char text = "cyan";
    []const char name = subslice(text, 0, 4);
    return len(name) - 4;
}
```

Use `raw_data(...)` when you need to pass a slice buffer to an extern function.

## `/std.ptr`

Purpose: construct typed null pointers.

Public API:

- `null<T>() -> T*`

Typical use:

```cyan
import /std.ptr as ptr;

bool is_missing(i64* value) {
    return value == ptr.null<i64>();
}
```

## `/std.mem`

Purpose: raw memory moves and typed buffer helpers.

Public API:

- `memcpy(char* dst, const char* src, i64 size) -> char*`
- `memset(char* dst, u8 value, i64 size) -> char*`
- `memcmp(const char* lhs, const char* rhs, i64 size) -> i32`
- `copy<T>(T* dst, const T* src, i64 count) -> T*`
- `fill<T>(T* dst, u8 value, i64 count) -> T*`

Typical use:

```cyan
import /std.mem as mem;

void clone4(i64* dst, const i64* src) {
    mem.copy(dst, src, 4);
}
```

`copy(...)` and `fill(...)` are count-based typed wrappers. `memcpy(...)` and `memset(...)` are byte-based.

## `/std.heap`

Purpose: heap allocation.

Public API:

- `malloc(i64 size) -> void*`
- `realloc(void* ptr, i64 size) -> void*`
- `free(void* ptr)`
- `malloc_array<T>(i64 count) -> T*`
- `realloc_array<T>(T* ptr, i64 count) -> T*`
- `free_ptr<T>(T* ptr)`

Typical use:

```cyan
import /std.heap as heap;

i64 main() {
    i64* values = heap.malloc_array<i64>(8);
    heap.free_ptr(values);
    return 0;
}
```

## `/std.cstr`

Purpose: basic C string helpers for `const char*`.

Public API:

- `strlen(const char* text) -> i64`
- `strcmp(const char* lhs, const char* rhs) -> i32`

Typical use:

```cyan
import /std.cstr as cstr;

bool same(const char* lhs, const char* rhs) {
    return cstr.strcmp(lhs, rhs) == 0;
}
```

## `/std.file`

Purpose: `FILE*`-style buffered file I/O.

Public API:

- `fopen(const char* path, const char* mode) -> void*`
- `fdopen(i32 fd, const char* mode) -> void*`
- `fclose(void* file) -> i32`
- `fread(char* ptr, i64 size, i64 count, void* file) -> i64`
- `fwrite(const char* ptr, i64 size, i64 count, void* file) -> i64`
- `fseek(void* file, i64 offset, i32 whence) -> i32`
- `ftell(void* file) -> i64`

Typical use:

```cyan
import /std.file as file;

i64 main() {
    void* handle = file.fopen("out.txt", "w+b");
    file.fclose(handle);
    return 0;
}
```

The `whence` argument to `fseek(...)` follows the C ABI values such as `0` for start, `1` for current, and `2` for end.

## `/std.strconv`

Purpose: number formatting and parsing.

Public API:

- `write_i64`, `write_u64`, `write_i128`, `write_u128`
- `write_f32`, `write_f64`
- `parse_i64`, `parse_u64`, `parse_i128`, `parse_u128`
- `parse_f32`, `parse_f64`

Typical use:

```cyan
import /std.strconv as strconv;

i64 main() {
    char[32] buf = ['\0'];
    strconv.write_i64(&mut buf[0], 42);
    return strconv.parse_i64(&buf[0]) - 42;
}
```

`write_*` writes a trailing `'\0'`. `write_f32` and `write_f64` take an explicit capacity. `parse_f32` delegates through `parse_f64` and then casts.

## `/std.ryu`

Purpose: low-level shortest-roundtrip float formatting.

Public API:

- `write_f32(char* out, i64 capacity, f32 value) -> i64`
- `write_f64(char* out, i64 capacity, f64 value) -> i64`

Typical use:

```cyan
import /std.ryu as ryu;

i64 main() {
    char[64] buf = ['\0'];
    return ryu.write_f64(&mut buf[0], 64, 0.5) < 0;
}
```

Most code should import `/std.strconv` instead. Reach for `/std.ryu` only if you want direct control over float formatting.

## `/std.println`

Purpose: formatted output and custom formatters.

Public API:

- `print`, `println`
- `fprint`, `fprintln`
- `print0`, `println0`
- `fprint0`, `fprintln0`
- `StringBuilder`
- `fmt<T>` interface
- `fmt_write_char`, `fmt_write_cstr`, `fmt_write_slice`
- `fmt_write_i64`, `fmt_write_u64`, `fmt_write_f64`, `fmt_write_bool`

Typical use:

```cyan
import /std.println;

i64 main() {
    println("answer = {}", [42]);
    println0("done");
    return 0;
}
```

Formatting rules:

- `{}` consumes the next argument from the `[]&fmt` slice.
- `{{` and `}}` escape literal braces.
- Missing arguments render as `<missing>`.
- Extra arguments append ` <extra args>`.

For custom types, implement `fmt(&T value, &mut StringBuilder out)`.

## `/std.atomic`

Purpose: atomics over `shared T*`.

Public API:

- memory orders: `atomic_relaxed`, `atomic_acquire`, `atomic_release`, `atomic_acq_rel`, `atomic_seq_cst`
- operations: `atomic_load`, `atomic_store`, `atomic_exchange`
- compare-and-exchange: `atomic_compare_exchange`
- read-modify-write: `atomic_fetch_add`, `atomic_fetch_sub`, `atomic_fetch_and`, `atomic_fetch_or`, `atomic_fetch_xor`
- fence: `atomic_fence`

Typical use:

```cyan
import /std.atomic;

void bump(shared i64* ptr) {
    atomic_fetch_add(ptr, 1, atomic_acq_rel());
}
```

The operations are generic over the pointee type, but the pointer itself must be `shared T*`.

## `/std.sync`

Purpose: shared ownership and mutual exclusion.

Public API:

- `Arc<T>`
- `arc_new<T>(T value) -> Arc<T>`
- `arc_clone<T>(&Arc<T> arc) -> Arc<T>`
- `arc_get<T>(&Arc<T> arc) -> &T`
- `Mutex<T>`
- `MutexGuard<T>`
- `mutex_new<T>(T value) -> Mutex<T>`
- `lock<T>(&Mutex<T> mutex) -> MutexGuard<T>`

Typical use:

```cyan
import /std.sync;

i64 main() {
    Mutex<i64> counter = mutex_new(0);
    {
        MutexGuard<i64> guard = lock(&counter);
        *guard.data = *guard.data + 1;
    }
    return 0;
}
```

`Arc<T>` is atomically reference-counted. `MutexGuard<T>` releases the lock in its `drop` impl.

## `/std.thread`

Purpose: thread spawning and parallel task execution.

Public API:

- task interfaces: `run_task<T>`, `run_once<T>`
- thread marker interfaces: `thread_shared`, `thread_sendable`
- composed interfaces: `threaded_runnable`, `spawnable`
- `JoinHandle`
- `parallel_do([]&threaded_runnable tasks)`
- `spawn<T>(T task) -> JoinHandle`
- `join(&mut JoinHandle handle)`

Typical use:

```cyan
import /std.thread;

struct Job {
    i64 value;
}

impl run_once(&mut Job self) {
    self.value = self.value + 1;
}

i64 main() {
    Job job = {41};
    JoinHandle handle = spawn(job);
    join(&mut handle);
    return 0;
}
```

`parallel_do(...)` runs a slice of interface values implementing `run_task + thread_shared`. `spawn(...)` requires `run_once + thread_sendable`.

## `/std.abi`

Purpose: ABI-level helpers that do not belong to a normal language prelude.

Public API:

- `fn_ptr(void* target) -> void*`

Typical use:

```cyan
import /std.abi;

T id<T>(T value) {
    return value;
}

i64 main() {
    void* raw = fn_ptr<i64>(id);
    return 0;
}
```

This is mainly for passing Cyan functions to extern code.

## `/std.rc`

Purpose: single-thread reference counting.

Public API:

- `Rc<T>`
- `rc_new<T>(T value) -> Rc<T>`
- `rc_clone<T>(&Rc<T> rc) -> Rc<T>`
- `rc_get<T>(&Rc<T> rc) -> &T`

Typical use:

```cyan
import /std.rc;

i64 main() {
    Rc<i64> value = rc_new(42);
    Rc<i64> copy = rc_clone(&value);
    return *rc_get(&copy) - 42;
}
```

`Rc<T>` is marked `local`. It is not the thread-safe counterpart of `Arc<T>`.

## `/std.math`

Purpose: public scalar math over `f32` and `f64`.

Public API:

- elementary functions: `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `atan2`
- exponentials and logs: `exp`, `exp2`, `exp10`, `expm1`, `log`, `log2`, `log10`, `log1p`
- powers and roots: `pow`, `powi`, `sqrt`, `cbrt`
- rounding: `floor`, `ceil`, `trunc`, `rint`, `nearbyint`, `round`, `roundeven`
- decomposition and scaling: `ldexp`, `scalbn`, `scalbln`, `frexp`, `modf`, `remquo`
- comparisons and distance: `fabs`, `copysign`, `fmin`, `fmax`, `fdim`, `hypot`, `nextafter`, `remainder`, `fmod`
- special functions: `erf`, `erfc`, `tgamma`, `lgamma`, `logb`, `ilogb`
- integer rounding results: `lround`, `llround`, `lrint`, `llrint`
- fused multiply-add: `fma`

Typical use:

```cyan
import /std.math as math;

f64 hypot2(f64 x, f64 y) {
    return math.sqrt(math.fma(x, x, y * y));
}
```

These functions are generic, but intended for `f32` and `f64`. `ilogb(...)` returns `i32`. The rounding-to-integer helpers return `i64`.

## `/std.math.intrin`

Purpose: raw LLVM math intrinsics.

Public API:

- direct scalar intrinsics such as `sin`, `cos`, `pow`, `sqrt`, `fma`, `roundeven`
- expert-only intrinsics such as `canonicalize`, `arithmetic_fence`
- NaN-sensitive min/max families: `minnum`, `maxnum`, `minimum`, `maximum`, `minimumnum`, `maximumnum`
- explicit integer-width rounders: `lround_i32`, `lround_i64`, `llround_i32`, `llround_i64`, `lrint_i32`, `lrint_i64`, `llrint_i32`, `llrint_i64`

Typical use:

```cyan
import /std.math.intrin as intrin;

f64 stable_round(f64 x) {
    return intrin.roundeven(x);
}
```

Prefer `/std.math` unless you explicitly want LLVM-level behavior or names that do not make sense as part of a normal math API.
