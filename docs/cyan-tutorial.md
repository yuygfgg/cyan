# Cyan Tutorial

**Cyan is a compiled systems language for writing explicit data layouts, borrowed views, zero-copy parsers, and C-facing code without giving up on structure.**

This tutorial is written against the repository as it exists today. The main teaching snippets live in `docs/snippets/`. Longer excerpts come from repository examples and stdlib code. Nothing here invents syntax that the current compiler does not implement.

The easiest way to use this document is to keep a terminal open beside it. When a section points at a standalone snippet such as `docs/snippets/hello_cyan.cyan`, run `cyan docs/snippets/hello_cyan.cyan --check` and read the code while the compiler stays honest.

The current builtin modules are `/std.slice`, `/std.heap`, `/std.mem`, `/std.cstr`, `/std.file`, `/std.println`, `/std.ptr`, `/std.strconv`, `/std.rc`, `/std.sync`, and `/std.thread`. The standard library is still early in its development.

## Built-In Types

Before getting into borrows and zero-copy parsing, it helps to know the full primitive type vocabulary.

| Type   | Meaning                                                                                                                      |
| ------ | ---------------------------------------------------------------------------------------------------------------------------- |
| `void` | No value. Used for functions that return nothing and as the pointee in `void*`.                                              |
| `bool` | Boolean truth value, `true` or `false`.                                                                                      |
| `char` | One byte. Cyan uses it for characters and raw byte-oriented text.                                                            |
| `i8`   | 8-bit signed integer.                                                                                                        |
| `i16`  | 16-bit signed integer.                                                                                                       |
| `i32`  | 32-bit signed integer.                                                                                                       |
| `i64`  | 64-bit signed integer. This is the fallback type of unsuffixed integer literals when no stronger integer context is present. |
| `i128` | 128-bit signed integer.                                                                                                      |
| `u8`   | 8-bit unsigned integer.                                                                                                      |
| `u16`  | 16-bit unsigned integer.                                                                                                     |
| `u32`  | 32-bit unsigned integer.                                                                                                     |
| `u64`  | 64-bit unsigned integer.                                                                                                     |
| `u128` | 128-bit unsigned integer.                                                                                                    |
| `f32`  | 32-bit floating-point number.                                                                                                |
| `f64`  | 64-bit floating-point number. This is the default type of unsuffixed floating-point literals.                                |

Most real Cyan programs quickly build richer types from those primitives. The common type forms are `T[N]` for fixed arrays, `[]T` for slices, `&T` and `&mut T` for borrows, and `T*` for raw pointers. A large part of learning Cyan is learning when to move from one form to another without losing track of who owns the storage.

Integer ranges follow the usual binary rule. An unsigned `uN` stores `0` through `2^N - 1`. A signed `iN` stores `-2^(N-1)` through `2^(N-1) - 1`. The negative side is one step larger. That asymmetry matters for `i128`.

`i128` ranges from `-170141183460469231731687303715884105728` to `170141183460469231731687303715884105727`.

`u128` ranges from `0` to `340282366920938463463374607431768211455`.

Current integer literal behavior is worth stating plainly. Cyan does not treat every bare integer literal as `i128`. It first looks for an integer type from the surrounding code. If you write `i128 big = 1;`, the `1` is checked as `i128`. If you write `u128 mask = 1 as u128;`, the cast forces `u128`. If there is no stronger integer context, an unsuffixed integer literal falls back to `i64`.

```cyan
i128 big = 1;
u128 wide = 340282366920938463463374607431768211455 as u128;
i64 defaulted = 1 + 2;
```

Negative integers are still written with unary `-`, not as a separate literal token. The current compiler has a special case so signed minimum values such as `-170141183460469231731687303715884105728` can still fit when the surrounding type is `i128`.

That special case exists because `-170141183460469231731687303715884105728` is a valid `i128`, but the positive magnitude `170141183460469231731687303715884105728` is not. In other words, `-2^127` fits in `i128`, while `2^127` does not. If you need the positive value `2^127`, use `u128`.

## 1. Hello Cyan

### Why Start This Small

People coming from Python or Java usually learn a language by writing a tiny program first and trusting the runtime to sort out the details later. That works in dynamic or heavily managed languages because the system is doing a large amount of silent work for you. Cyan takes the opposite approach. It wants the shape of data, the return type of each function, and the control flow to be visible in the source. Starting with the smallest possible program shows that style before the harder topics arrive.

### Mental Model

Read a Cyan function signature as a label glued to a machine. The label tells you what the machine accepts and what it gives back. When you see `i64 add(i64 lhs, i64 rhs)`, you are seeing the entire contract: two 64-bit integers go in, one 64-bit integer comes out. The body then has one job, which is to keep that promise.

### The Code

This file is `docs/snippets/hello_cyan.cyan`.

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

If you want the minimum tool loop, use:

```sh
cyan docs/snippets/hello_cyan.cyan --check
cyan docs/snippets/hello_cyan.cyan -o /tmp/hello_cyan.o
cyan docs/snippets/hello_cyan.cyan --emit-llvm -o /tmp/hello_cyan.ll
```

### Walkthrough

Start with `i64 add(i64 lhs, i64 rhs)`. Cyan puts the return type first. That looks unusual if you came from Java, but it becomes natural once you notice how often systems code starts by asking, “what exact thing comes back from this function?” Here the answer is concrete before you even read the body.

Inside `add`, the line `return lhs + rhs;` is intentionally boring. That is useful. You can already see that Cyan arithmetic on ordinary integers looks familiar, and a function body does not need ceremony when the contract is simple.

Now move to `i64 main()`. In this repository, runnable programs almost always return an integer exit code. `0` means success. A non-zero value means failure. That is why the file tests the result and returns either `0` or `1`.

The declaration `i64 value = add(20, 22);` is your first local variable. Cyan does not hide the type. That is a recurring theme: when a value matters, the source usually says what it is. Later, when you are passing slices, borrows, or raw pointers around, that explicitness becomes much more valuable than it looks here.

The `if (value == 42) { ... }` block is conventional on purpose. Cyan spends its complexity budget on memory views and provenance tracking.

### Under The Hood And First Pitfall

Cyan does some inference, but only when the source gives the compiler something real to infer from. A short example makes that limit visible:

```cyan
T make_default<T>() {
    return 0;
}

i64 main() {
    return make_default();
}
```

This fails with `could not infer type argument for 'T'`. `make_default()` has no argument that reveals what `T` should be. This same rule will matter later when you call `ptr.null<T>()`: if no value reveals the type parameter, you must write it.

## 2. Data Shapes: Structs, Arrays, Loops, Slices, And Strings

### Why Cyan Separates These Ideas

Many beginner-friendly languages blur together “a sequence of things,” “a dynamic container,” and “a view into part of a container.” C famously goes the other way and makes you work with raw arrays that decay into pointers in ways that surprise even experienced programmers. Cyan draws sharper lines. A fixed array owns storage. A slice is a view over storage. A struct groups fields in a fixed layout. Once you accept those distinctions, the rest of the language gets easier to reason about.

### Mental Model

Imagine a row of apartments. The **array** is the actual building: it owns the rooms. A **slice** is a visitor pass that says, “you may look at apartments 3 through 5.” The pass points at part of a building that already exists. The important extra detail is that the pass is more general than the building. You can make a slice from an array, from a string literal’s backing storage, or from another slice. Slices are reusable windows into any contiguous region the compiler knows how to describe.

### The Code

This file is `docs/snippets/core_syntax.cyan`.

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
    i64[4] values = [pair.left, pair.right, 5, 6];
    []i64 tail = subslice(values, 1, len(values) - 1);
    i64 total = sum(tail);

    for (i64 i = 0; i < 2; i++) {
        total++;
    }

    return total - 17;
}
```

### Walkthrough

`struct Pair { i64 left; i64 right; };` introduces two basic Cyan habits at once. First, field types are explicit. Second, the layout is simple and visible. There is no hidden property system, no constructor magic, and no separate schema language. You read the struct and you know its shape.

`i64 sum([]i64 values)` is the first place where Cyan’s sequence story becomes interesting. The function takes `[]i64`, a slice. That means `sum` only cares that there is a contiguous run of `i64` values it can read. This is why slices are more flexible than arrays. An array is one concrete owner. A slice is an abstract view that many kinds of storage can satisfy.

Inside `sum`, the declarations `i64 i = 0;` and `i64 total = 0;` make the loop state obvious. Then `while (i < len(values))` does what you would expect: `len(...)` asks the slice how many elements it exposes, and `values[i]` reads one element at a time. There is no iterator protocol here. It is plain index-based code, which is often exactly what you want in systems work.

Move down to `Pair pair = {3, 4};`. Cyan uses positional struct initialization. The source order of values matches the field order in the struct. That is a small rule, but it keeps the language tight and predictable.

The declaration `i64[4] values = [pair.left, pair.right, 5, 6];` creates a fixed array. This is owned storage. Those four integers live in one concrete object. The type says so up front: four `i64` values in a fixed layout.

Fixed arrays are not limited to plain scalars. They can also store copyable
view values such as shared borrows, slices, and structs or enums that carry
those view values. The checker treats `array[index]` conservatively, so
borrowing through an indexed view array may keep more source places read-only
than a fully per-index analysis would.

`[]i64 tail = subslice(values, 1, len(values) - 1);` is where the owner/view split becomes real. `values` owns the storage. `tail` is a slice that starts at index `1` and spans the rest of the array. No copy happens here. Cyan is carving out a view.

Then `i64 total = sum(tail);` shows why that distinction matters. `sum` works directly with the slice and stays independent from the original owner.

The `for (i64 i = 0; i < 2; i++) { total++; }` loop exists for two reasons. It shows that Cyan also has a C-style `for`, and it quietly shows scope. The loop variable `i` lives inside the `for` loop, not after it.

Finally, `return total - 17;` turns the snippet into a runnable check. The file succeeds only if all the earlier pieces behaved the way we think they did.

### Under The Hood: Loop Scope

This tiny failing example teaches a rule that trips people early:

```cyan
i64 bad() {
    for (i64 i = 0; i < 1; i++) {}
    return i;
}
```

The compiler reports `unknown identifier 'i'`. That is not Cyan being fussy about syntax. It is telling you that the `for` initializer introduced a name whose lifetime ended with the loop. If you need the variable later, declare it outside the loop and update it inside.

## 3. Slices And String Literals

### Why This Deserves Its Own Chapter

If you only ever work in high-level languages, a string feels like one obvious thing. In low-level code, it usually is not. C uses a pointer to bytes that ends when a `'\0'` byte appears. Other languages store a length separately. Parsers often need a middle ground: “here is a view over these bytes, and I know exactly how long it is.” Cyan slices are that middle ground.

### Mental Model

Think of a slice as a transparent ruler laid over bytes. The ruler has a start and a length. It does not care whether the bytes came from an array on the stack, from a string literal in static storage, or from another slice that was already looking at the same memory. That is why slices are broader than arrays. Arrays are one source of bytes. Slices are the common language for *many* sources of bytes.

This is the easiest diagram to keep in your head:

```text
string literal storage:  [ h ][ e ][ l ][ l ][ o ][ \0 ]
whole slice "text":      [------------- six bytes -------------]
subslice "part":              [---- three bytes ----]
```

### The Code

This example is small enough to read in one pass:

```cyan
import /std.slice as slice;

i64 main() {
    []const char text = "hello";
    []const char part = slice.span_slice(text, 1, 3);
    if (slice.span_len(text) == 6 && slice.span_len(part) == 3 &&
        part[0] == 'e' && part[2] == 'l') {
        return 0;
    }
    return 1;
}
```

### Walkthrough

`import /std.slice as slice;` introduces two ideas at once. The leading `/` selects a builtin module. The `as slice` part gives it a local namespace.

Now read `[]const char text = "hello";` slowly. This line surprises many readers the first time. Cyan is happy to treat the string literal as a slice, but the slice covers the **entire literal storage**, including the trailing `'\0'`. That is why the program later checks that `slice.span_len(text) == 6`, not `5`.

The next line, `[]const char part = slice.span_slice(text, 1, 3);`, builds a smaller view on top of the first view. The operation allocates no new string and copies no bytes. It simply describes a shorter window into bytes that already existed.

The `if` condition checks all of this at once. `slice.span_len(text) == 6` confirms the whole literal includes the terminator. `slice.span_len(part) == 3` confirms the smaller view has exactly three visible bytes. `part[0] == 'e'` and `part[2] == 'l'` confirm that the view starts one byte into the original literal.

String and character literals use byte-oriented escape sequences for common
control characters. Today that includes `\n`, `\r`, `\t`, `\v`, `\f`, `\0`,
and the usual quote and backslash escapes that each literal form needs.

### Under The Hood: The Off-By-One Surprise

When you want only the visible letters of a string literal, do not pass the whole literal blindly as a slice. Carve out the visible part yourself:

```cyan
[]const char name = subslice("cyan", 0, 4);
```

That `4` matters. The literal storage is really five bytes long: `c`, `y`, `a`, `n`, `\0`.

The other easy mistake is trying to return a slice that points at storage that is about to die. This short program shows the problem directly:

```cyan
[]i64 bad() {
    i64[2] values = [1, 2];
    return values;
}

i64 main() {
    return 0;
}
```

The compiler rejects this with `view-returning function must depend on a borrow or slice parameter`. The deeper reason is that `values` is a local array. It stops existing when `bad()` returns. A slice pointing at it would be a view into dead memory. Cyan refuses to let that escape.

## 4. Modules And Builtin Imports

### Why Cyan Uses Explicit Import Paths

As soon as code grows past one file, a language has to answer two practical questions. How do you reach your own modules, and how do you reach the standard library? Some languages hide the second question behind magical global names. Cyan keeps both forms visible in the source.

### Mental Model

Treat an import path as an address. `import tutorial_support.stats as stats;` is a package-local address that resolves to another Cyan file. `import /std.slice as slice;` is an address into the builtin library shelf embedded in the compiler. The leading `/` is the sign that says, “this name does not come from my package directory.”

### The Code

These two tiny files are enough to show the local-module form.

`docs/snippets/modules_demo.cyan`:

```cyan
import tutorial_support.stats as stats;

i64 main() {
    return stats.meaning() - 42;
}
```

`docs/snippets/tutorial_support/stats.cyan`:

```cyan
export i64 meaning() {
    return 42;
}
```

### Walkthrough

The import line `import tutorial_support.stats as stats;` uses a dotted path because the module lives under `docs/snippets/tutorial_support/stats.cyan`. The alias `as stats` means the imported declarations are accessed through the name `stats`.

The exported function in the second file is ordinary top-level Cyan code. `export` is the key word that makes it visible to importing modules.

Back in `main`, the call `stats.meaning()` looks exactly like what it is: call the exported function through the namespace alias.

Builtin imports follow the same syntactic pattern, but start with `/`. You already saw `import /std.slice as slice;` earlier. The same shape works for `/std.println`, `/std.file`, `/std.ptr`, and the rest of the current builtin set.

### Under The Hood

This distinction matters because Cyan compiles the whole import closure as one package. Local imports come from files in your package root. Builtin imports come from the embedded stdlib sources under `stdlib/std/*.cyan`. The code looks uniform at the call site, but the leading `/` still tells you which world you are pulling from.

## 5. Borrows, Mutation, And Reborrowing

### Why Borrows Exist

C will happily hand you ten pointers to the same integer and trust you not to create a mess. Java and Python give you references everywhere, but they do not make aliasing rules visible in the source. Cyan tries to land between those worlds. It wants you to be able to say, “I am only reading through this reference,” or, “I am temporarily taking exclusive write access,” and then have the compiler check that story.

### Mental Model

A shared borrow, written `&T`, is a library reading card. You may inspect the book, but you may not scribble in it. A mutable borrow, written `&mut T`, is a temporary work order for the bookbinder. While the binder has the book, nobody else gets to pretend they are independently editing it. A **reborrow** is what happens when the binder briefly hands the book to another binder for one subtask and then takes it back.

### The Code

This runnable example is short enough to keep entirely in your head:

```cyan
i64 main() {
    i64 value = 0;
    {
        &mut i64 first = &value;
        {
            &mut i64 second = first;
            *second = 1;
        }
        *first = 2;
    }
    return value;
}
```

### Walkthrough

`i64 value = 0;` creates ordinary owned data. Nothing is borrowed yet. The interesting part starts with the first nested scope.

`&mut i64 first = &value;` creates a mutable borrow of `value`. The type on the left, `&mut i64`, matters as much as the expression on the right. This line creates an exclusive mutable loan.

Inside the inner block, `&mut i64 second = first;` creates a reborrow. This line is subtle and important. `second` is a shorter-lived mutable loan derived from `first`.

`*second = 1;` writes through the inner reborrow. The `*` says, “go through the borrow and touch the underlying value.” Because we are in a mutable borrow, that write is allowed.

Then the inner scope ends. At that exact moment, `second` disappears. Only after that does `*first = 2;` become legal again. This is the heart of reborrowing. Cyan lets you temporarily hand off exclusive access, but it tracks when that handoff ends.

When the outer block ends, `first` also disappears. `return value;` then reads the owned integer directly and returns `2`.

### Under The Hood: What Cyan Refuses

If you try to write through a shared borrow, Cyan stops you. A short example is enough:

```cyan
i64 main() {
    i64 value = 0;
    &i64 alias = &value;
    *alias = 3;
    return value;
}
```

The compiler reports `cannot write to a borrowed place`. This is exactly the library-card rule: `&i64` means read-only access.

The other classic mistake is letting a borrow outlive its source. This example shows it without any abstraction:

```cyan
i64 main() {
    i64 fallback = 0;
    &i64 alias = &fallback;
    {
        i64 inner = 1;
        alias = &inner;
    }
    return 0;
}
```

This fails with `borrow source does not live long enough`. The compiler is following the same story you would tell another human. `inner` lives only inside the inner block. Once that block ends, `alias` cannot still claim to point at `inner`.

## 6. Enums And `switch`: Borrow Versus Move

### Why Cyan Makes You Choose

In C, tagged unions are manual. You store a tag, store a payload, and pray every branch checks the tag before reading the payload. In garbage-collected languages, pattern matching usually does not force you to think about ownership. Cyan does. When you match on an enum, the compiler wants to know whether you are merely peeking at the payload, mutating it in place, or consuming the enum and taking ownership of what is inside.

### Mental Model

Imagine an enum value as a sealed suitcase with a label on the outside. `switch (&value)` means, “open the suitcase, look inside, but leave the suitcase on the table.” `switch (&mut value)` means, “open the suitcase and rearrange what is inside.” `switch (move value)` means, “hand the suitcase over completely; after this point, the old owner does not keep it.”

### The Code

This file is `docs/snippets/enums_and_switch.cyan`.

```cyan
enum Option<T> {
    None,
    Some(T),
};

i64 read_by_borrow(Option<i64> value) {
    switch (&value) {
        case None:
            return 0;
        case Some(payload):
            return *payload;
    }
}

i64 read_by_move(Option<i64> value) {
    switch (move value) {
        case None:
            return 0;
        case Some(payload):
            return payload;
    }
}

i64 main() {
    Option<i64> value = Some(9);
    return read_by_borrow(value) + read_by_move(Some(1)) - 10;
}
```

### Walkthrough

`enum Option<T> { None, Some(T), };` defines a generic tagged value with two cases. `None` carries nothing. `Some(T)` carries one payload of type `T`.

In `read_by_borrow`, focus on `switch (&value)`. The `&` is the whole story. This branch borrows `value`. The `Some(payload)` branch therefore binds `payload` as a borrow to the payload, and the line `return *payload;` dereferences it.

In `read_by_move`, the signature still receives `Option<i64> value`, but the match uses `switch (move value)`. This time the match consumes the enum. In the `Some(payload)` branch, `payload` is a plain `i64`, so the line is just `return payload;`.

The `main` function shows both styles side by side. `Option<i64> value = Some(9);` creates an owned enum value. `read_by_borrow(value)` borrows internally. `read_by_move(Some(1))` consumes the temporary enum it receives.

### Mutating Through A Match

There is a third mode:

```cyan
enum BoxedInt {
    Value(i64),
};

i64 set_and_read(BoxedInt value) {
    switch (&mut value) {
        case Value(payload):
            *payload = 41;
    }

    switch (&value) {
        case Value(payload):
            return *payload;
    }
}
```

The first `switch` borrows the enum mutably, so `payload` becomes a mutable borrow to the payload and `*payload = 41;` is legal. The second `switch` then reads the updated value through a shared borrow. This is a compact example of Cyan’s ownership choices staying visible in the syntax.

### Under The Hood: Exhaustiveness Means Something

This short failing example shows Cyan taking exhaustiveness seriously:

```cyan
enum Option {
    None,
    Some(i64),
};

i64 main() {
    Option value = None();
    switch (move value) {
        case None:
            return 0;
        case Some(payload):
            return payload;
        default:
            return 2;
    }
}
```

The compiler reports `default case is unreachable`. Once you have already handled every variant of the enum, `default` is dead code.

## 7. `depends(...)`: Zero-Copy Views With Provenance

### Why Cyan Needs A Feature Like This

This is the chapter where Cyan stops looking like “a small typed language” and starts looking like itself.

Suppose you are writing an HTTP parser in C. You read bytes into a buffer, then you want to return the method, path, headers, and body without copying them. The usual C answer is some combination of `char*`, lengths, comments, and discipline. The code may work, but the lifetime story stays in the programmer’s head. If somebody later returns a pointer into a dead local buffer, the compiler has very little to say.

Cyan wants zero-copy parsing too, but it wants the source to spell out where returned views came from. That is what `depends(...)` does. It is a provenance declaration. It tells the compiler, “this borrow or slice in the return value, or in this output parameter, is derived from that input.”

If you know Rust, the closest mental bridge is lifetime relations. The difference is that Cyan writes the relation as an explicit dependency path in the function signature instead of hiding it behind separate lifetime parameter syntax.

If you come from Java or Python, the mental gap is different. In those languages, a parser can cheaply return a small object whose fields refer to one backing string or byte array, and the garbage collector quietly keeps that backing storage alive. Cyan does not have that luxury. If a struct field is a slice, that field does **not** own the bytes. It is only a view. `depends(...)` is the line that tells the compiler which incoming storage keeps that view valid.

### The One-Sentence Rule

When you see `depends(...)`, read it as an answer to one question:

> “Which input already owns the storage that this returned borrow or slice points into?”

That is all. It is not about allocation. It is not about copying. It is not about who may mutate the bytes. It is only about provenance: where the view came from.

### Mental Model

Take one strip of paper and write `cyan` across it. Now cut two clear plastic windows and place one over `cy` and one over `an`. The paper strip is the source buffer. The windows are slices. `depends(...)` is the label on the windows that says, “both of these windows are views into that paper strip.”

For aggregate returns, Cyan tracks **leaf views** inside the top-level shape. If a returned struct contains two slices, the compiler asks where each one came from. `depends(return on text)` is shorthand for, “every borrow or slice leaf inside the returned value comes from `text`.” A more specific path such as `depends(return.left on x, return.right on y)` says, “the leaves come from different places, and I want to say exactly which.”

The same idea applies to output parameters. `depends(out on text)` means, “after this call, any borrow or slice fields stored inside `out` should be treated as views into `text`.”

Another way to say the same thing is this:

- the buffer or borrowed input is the **real owner**
- the returned struct is a **bag of windows**
- `depends(...)` is the **map from each window back to the real owner**

If that map is wrong, you eventually get the worst kind of systems bug: code that still compiles, still looks reasonable, but is quietly reading the wrong storage. Cyan insists that the map live in the signature so the compiler can check it instead of trusting comments.

### The Code

This standalone example is `docs/snippets/depends_and_views.cyan`.

```cyan
struct Split {
    []const char head;
    []const char tail;
};

struct View {
    []const char data;
};

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

void take_tail(&mut View out, []const char text, i64 mid)
    depends(out on text) {
    Split parts = split_at(text, mid);
    out.data = parts.tail;
}

i64 main() {
    []const char word = subslice("cyan", 0, 4);
    Split parts = split_at(word, 2);
    []const char empty = subslice("", 0, 0);
    View view = {empty};
    take_tail(&mut view, word, 2);

    if (parts.head[0] != 'c') {
        return 1;
    }
    if (parts.tail[1] != 'n') {
        return 2;
    }
    if (view.data[0] != 'a') {
        return 3;
    }
    return 0;
}
```

### Walkthrough

Start with the data shapes. `Split` has two slice fields, `head` and `tail`. `View` has one slice field, `data`. Nothing in either struct owns bytes. That is the first habit to internalize.

If you are used to Java classes or Python objects, that line deserves to be repeated:

- `Split` is **not** storing two copied strings
- `View` is **not** storing an independent buffer
- both structs are storing **references into some other storage**

That is why the dependency contract matters so much more here than it would in a garbage-collected language.

Now look at `Split split_at([]const char text, i64 mid) depends(return on text)`. The parameter `text` is itself a slice view. The function promises to return a `Split`, whose two slice fields both point into `text`. That promise is what `depends(return on text)` means.

Read the branches with that contract in mind:

- `if (mid <= 0)` returns an empty prefix plus the whole input
- `if (mid >= len(text))` returns the whole input plus an empty suffix
- the final branch returns two smaller windows cut out of `text`

In all three branches, the storage source stays the same. No new string is allocated. No bytes are copied. The function is only producing new views.

The empty-slice case is especially worth noticing. `subslice(text, 0, 0)` is still treated as derived from `text`. Cyan does not treat “empty” as magical provenance-free data. An empty view is still a view.

Now move to `void take_tail(&mut View out, []const char text, i64 mid) depends(out on text)`. This is the mutable-output form. `out` is a struct that contains a slice field. The dependency declaration tells the compiler how to reason about the fields written into that struct.

Inside `take_tail`, `Split parts = split_at(text, mid);` produces two views into `text`. Then `out.data = parts.tail;` stores one of those views into `out`.

This is the key idea behind the HTTP parser later in the tutorial. A parser often fills an already-allocated output object in place:

- `out.method` points into the request buffer
- `out.path` points into the request buffer
- `out.body` points into the request buffer

`depends(out on bytes)` is how the function says all of that in one line.

`main()` turns the abstract rule into something concrete. `[]const char word = subslice("cyan", 0, 4);` strips off the string literal terminator so the example focuses only on dependency tracking. `Split parts = split_at(word, 2);` produces `cy` and `an`. `View view = {empty};` creates a placeholder struct, and `take_tail(&mut view, word, 2);` rewrites the placeholder so that `view.data` becomes another view into `word`.

The final checks prove the returned slices are reading the same source bytes we expect:

- `parts.head[0]` is `c`
- `parts.tail[1]` is `n`
- `view.data[0]` is `a`

That is the practical payoff of `depends(...)`: the code gets to stay zero-copy, but the lifetime story is no longer hidden in the programmer’s head.

### A Java/Python Reading Of The Same Example

If the previous explanation still feels abstract, translate it into a more familiar object-oriented picture.

Imagine you had a Java class like this:

```java
class Split {
    ByteRange head;
    ByteRange tail;
}
```

and `ByteRange` were just `(buffer, start, length)`. Then `split_at(text, 2)` would return one `Split` whose two fields both point into the same original `buffer`.

That is effectively what Cyan is doing, except Cyan makes the relationship compiler-visible. `depends(return on text)` is the proof that both `head` and `tail` are byte ranges into `text`, not detached storage.

### When Cyan Can Infer The Dependency

Not every view-returning function needs an explicit `depends(...)`. The simplest case looks like this:

```cyan
&i64 first(&i64 x) {
    return x;
}

i64 read(&i64 x) {
    return *x;
}

i64 main() {
    i64 value = 6;
    return read(first(&value));
}
```

Why does this compile without `depends(return on x)`? Because there is only one plausible view source parameter: `x`. The compiler can infer the relation because there is no ambiguity. This is a convenience, not magic. As soon as more than one source could explain the returned view, you must be explicit.

That last sentence matters. Many readers see one inferred case and overgeneralize. Do not. The inference rule is intentionally narrow. It exists to keep trivial identity-style helpers pleasant. It is not an invitation to write parser signatures that hide provenance.

### When Cyan Cannot Infer The Dependency

The canonical failure looks like this:

```cyan
&i64 choose(&i64 x, &i64 y) {
    return x;
}
```

The compiler rejects it with `view-returning functions with multiple borrow or slice parameters require depends(return on <param>)`. Even though *you* can see that this particular body returns `x`, the signature leaves the relationship unstated while there are two possible sources in play. Cyan requires the contract to say it.

This is a good place to pause and ask what disaster Cyan is preventing.

Without that rule, a future edit like this would be frighteningly easy:

- version 1 returns `x`
- version 2 sometimes returns `y`
- callers still read the old signature and assume the old relation

`depends(...)` prevents the function contract from drifting away from the implementation.

### Under The Hood: Forgetting `depends(...)` On A Struct Return

The easiest hard failure to understand is an aggregate return. If a function returns a struct that contains slice fields, Cyan requires you to say where those fields came from.

This version deliberately omits the contract:

```cyan
struct Split {
    []const char head;
    []const char tail;
};

Split split_at([]const char text, i64 mid) {
    return {
        subslice(text, 0, mid),
        subslice(text, mid, len(text) - mid)
    };
}
```

The compiler rejects it with `functions returning aggregates with borrow or slice fields require an explicit depends clause`.

That message is the compiler telling you, “I see that you are returning a struct full of views, but the signature never said where those views come from.” The fix is exactly the version you already saw:

```cyan
Split split_at([]const char text, i64 mid)
    depends(return on text) {
    return {
        subslice(text, 0, mid),
        subslice(text, mid, len(text) - mid)
    };
}
```

### Precise Paths For Struct Fields

The moment a returned value contains several views, the dependency path becomes more precise. A minimal working example looks like this:

```cyan
struct PairRefs {
    &i64 left;
    &i64 right;
};

PairRefs pair_refs(&i64 x, &i64 y)
    depends(return.left on x, return.right on y) {
    return {x, y};
}

i64 read(&i64 value) {
    return *value;
}

i64 main() {
    i64 left = 3;
    i64 right = 7;
    PairRefs refs = pair_refs(&left, &right);
    if (read(refs.left) != 3) {
        return 1;
    }
    if (read(refs.right) != 7) {
        return 2;
    }
    return 0;
}
```

`return.left on x` and `return.right on y` are not verbosity for verbosity’s sake. They are the exact map the compiler needs. One field comes from `x`, the other from `y`.

This is where the feature starts paying rent in real parsers and protocol decoders. A returned struct often mixes several sources:

- a request buffer
- a precomputed keyword slice
- a fallback value
- a borrowed state object

Once those sources mix, the short form `depends(return on text)` is no longer enough. The paths tell you, field by field, what is attached to what.

There is also a useful shorthand-plus-override pattern:

```cyan
struct Pair {
    []const char left;
    []const char right;
};

struct Bundle {
    Pair pair;
    []const char tail;
};

Bundle build([]const char text, []const char special)
    depends(return on text, return.pair.right on special) {
    return {
        {subslice(text, 0, 1), special},
        subslice(text, 1, len(text) - 1)
    };
}
```

Read that declaration in two passes. First, `depends(return on text)` says every view leaf in the return value comes from `text`. Then `return.pair.right on special` overrides one specific leaf. That is an elegant pattern when most fields come from one source but a few come from somewhere else.

If you have written Rust before, this is roughly the point where a function signature would start accumulating more lifetime relations. Cyan uses explicit dependency paths instead. If you have **not** written Rust before, that is fine too. The simpler reading is enough:

- start with the default source
- override the exceptional field
- let the compiler verify the body matches the story

### Dependency Paths Can Reach Enum Payloads

This is where Cyan’s model becomes more expressive than many readers expect. Here is an example with views inside enum payloads:

```cyan
struct Token {
    i64 value;
};

enum Selection {
    Head(&Token),
    Tail([]Token),
};

Selection choose([]Token values, bool want_head)
    depends(return.Head on values, return.Tail on values) {
    if (want_head) {
        return Head(&values[0]);
    }
    return Tail(values);
}
```

`return.Head on values` means the `Head` payload, if that variant is chosen, borrows from `values`. `return.Tail on values` says the same for the slice payload of `Tail`. It follows the same core rule: each view leaf gets a provenance path.

This looks advanced, but the principle has not changed. Cyan is still asking the same question: “if this variant is chosen, where did the view inside it come from?”

### Output Parameters Need Provenance Too

The mutable-output version is just as important in real programs. This pattern is common:

```cyan
struct Pair {
    []const char left;
    []const char right;
};

void fill(&mut Pair out, []const char text, []const char special)
    depends(out on text, out.right on special) {
    out.left = subslice(text, 0, 1);
    out.right = special;
}
```

Here `out` is a struct that gets rewritten in place. Most of its view fields should be treated as derived from `text`, but one specific field, `out.right`, should be treated as derived from `special`. This is the mutable mirror of the return-value override pattern.

There is one nuance worth stating explicitly because it affects how you read later chapters.

Today Cyan is stricter about **aggregate return values** than about every possible mutable-output case. In particular, if you omit the mutable-parameter `depends(...)`, the assignment itself may compile, but the ambiguity has not gone away. It can resurface later when you try to use the rewritten view and the compiler can only say, “I no longer know what storage this points into.”

Why? Because the explicit line becomes essential the moment the function stops being trivial.

### Under The Hood: Forgetting `depends(...)` On A Mutable Output

This function rewrites a `View`, but never states where the new slice in `out.data` came from:

```cyan
struct View {
    []const char data;
};

void choose_tail(&mut View out,
                 []const char bytes,
                 []const char fallback,
                 bool use_fallback) {
    if (use_fallback) {
        out.data = fallback;
        return;
    }
    out.data = bytes;
}
```

That call may look harmless, especially if you only test it with short literals. But once later code tries to use `out.data`, Cyan can reject the use site with `cannot use a view value whose source is unknown after this call or control-flow path`.

In other words, the omission does not make the provenance problem disappear. It merely postpones where the compiler can point at it.

The fix is to make the relation explicit in the signature so the caller and the compiler both know what was written into `out`.

### Under The Hood: A Parser-Shaped Mutable-Output Mismatch

Here is the next level of bug: the function *does* declare a source, but the body stores a different one.

```cyan
struct View {
    []const char data;
};

void overwrite_wrong(&mut View v, []const char expected, []const char actual)
    depends(v.data on expected) {
    v.data = actual;
}
```

The compiler reports `mutable parameter borrow or slice does not match its declared depends source`.

This is the same bug that would bite an HTTP parser if it promised “all fields in `out` come from `bytes`” and then quietly stored a slice from some fallback buffer or scratch buffer instead.

### Under The Hood: Cyan Checks The Body Against The Promise

The most important thing to understand about `depends(...)` is that the compiler verifies that the body actually satisfies the declared relation.

The shortest proof looks like this:

```cyan
&i64 choose_left(&i64 x, &i64 y, bool cond) depends(return on x) {
    if (cond) {
        return x;
    }
    return y;
}
```

The compiler reports `returned borrow does not match depends(return on x)`. That message is earned. The declaration promised that the returned borrow always comes from `x`. The body sometimes returns `y`. Cyan notices the mismatch.

This variant makes the same point through control-flow merging:

```cyan
&i64 choose(&i64 a, &i64 b, bool cond) depends(return on a) {
    &i64 x = a;
    if (cond) {
        x = a;
    } else {
        x = b;
    }
    return x;
}
```

Even though the final `return x;` looks uniform, the compiler remembers that `x` may hold either source depending on the branch. That is why the error is still correct.

This is the core payoff of `depends(...)`. It gives the compiler enough structure to check zero-copy code that would otherwise rely on comments and luck.

### A Small Checklist Before You Leave This Chapter

When you write a function involving borrows or slices, ask these questions in order:

1. Does this function return a view, or store one into a mutable output object?
2. If yes, which input already owns the storage behind that view?
3. If there are several possible sources, have I written the path explicitly?
4. If I changed the body, does the declared dependency still match what the code actually returns or stores?

If you keep that checklist in your head, the later HTTP parser chapter becomes much less mysterious.

## 8. Interfaces, Erased Borrows, And Why `println` Works

### Why Cyan Has Interfaces At All

Sooner or later, you want one operation to work across unrelated types. In C, the usual answer is `void*` plus a function pointer and a pile of manual conventions. In Java, the answer is often a nominal interface hierarchy. Cyan goes for something leaner. An interface describes one callable operation. Types satisfy it by writing matching `impl` blocks. The result feels like ad-hoc polymorphism rather than class inheritance.

### Mental Model

Think of a Cyan interface as a **verb** or a **capability**. `measure` names one operation that some receiver types support. An interface alias such as `measure + fmt` names a bundle of capabilities. An interface value such as `&measure` is an erased borrow to “something that can be measured.” A slice such as `[]&measure` is a tray full of values that all support the same verb, even if their concrete types differ.

### The Code

This standalone file is `docs/snippets/interfaces.cyan`.

```cyan
interface<T> i64 measure(&T value);

struct Box {
    i64 value;
};

impl measure(&Box box) {
    return box.value;
}

i64 call_dyn(&measure value) {
    return measure(value);
}

i64 sum([]&measure values) {
    i64 i = 0;
    i64 total = 0;
    while (i < len(values)) {
        total = total + measure(values[i]);
        i++;
    }
    return total;
}

i64 main() {
    Box left = {4};
    Box right = {8};
    if (measure(left) != 4) {
        return 1;
    }
    if (call_dyn(&right) != 8) {
        return 2;
    }
    if (sum([&left, &right]) != 12) {
        return 3;
    }
    return 0;
}
```

### Walkthrough

`interface<T> i64 measure(&T value);` declares the verb. The receiver type parameter `T` appears in the first parameter as a borrow. That is Cyan saying, “measurement will be selected from the receiver type.”

`struct Box { i64 value; };` is just one concrete type that can implement the verb.

`impl measure(&Box box) { return box.value; }` connects the two. There is no inheritance declaration, no `implements` keyword, and no separate vtable spelling. The `impl` says that when the receiver is `&Box`, the `measure` operation should read `box.value`.

`i64 call_dyn(&measure value)` takes an erased interface borrow. The exact concrete type is gone from the signature, but the capability remains: this value supports `measure(...)`.

Inside `call_dyn`, the line `return measure(value);` looks almost too simple. That is a good sign. Cyan’s interface dispatch is designed so the call site reads like the direct form.

`i64 sum([]&measure values)` raises the abstraction one step. The parameter is a slice of erased interface borrows. The loop body does not care whether element `0` and element `1` came from the same concrete type. It only cares that `measure(values[i])` is valid for each one.

`main()` then exercises all three styles: a direct call on a concrete value, a dynamic call through `&measure`, and a loop over `[]&measure`.

### Composing Interfaces With `+` And `-`

Single verbs stay useful, but many APIs need more than one capability at once. Cyan handles that by letting you name interface expressions:

```cyan
interface<T> i64 left(&T value);
interface<T> i64 right(&T value);
interface both = left + right;

struct Pair {
    i64 lhs;
    i64 rhs;
};

impl left(&Pair value) {
    return value.lhs;
}

impl right(&Pair value) {
    return value.rhs;
}

i64 sum_pair(&both value) {
    return left(value) + right(value);
}
```

`interface both = left + right;` says that `both` is not a new verb. It is a name for “anything that supports `left` and `right`.” That is why `sum_pair(&both value)` can call `left(value)` and `right(value)` separately. The composed interface exposes the positive callable members it names.

Subtraction is the other half:

```cyan
interface<T> i64 read(&T value);
interface<T> i64 seek(&T value);
interface streaming = read - seek;
```

Read `streaming` as “readable, but not seekable.” A type that implements both `read` and `seek` does not satisfy `streaming`. Inside a function that takes `&streaming`, you can call `read(value)`. You cannot call `seek(value)` through that interface, because `seek` is an exclusion rule, not a provided capability.

This turns out to be especially useful at library boundaries. `/std.thread` uses the same mechanism to say that a task must implement `run_task`, must be shareable, and must not be marked `local`.

### The Same Mechanism Powers `/std.println`

The standard-library printing module uses the same pattern. The formatter lives in `/std.println`, and its public calls look like this:

```cyan
import /std.println;

i64 main() {
    []const char name = subslice("cyan", 0, 4);
    i64 answer = 42;
    println("{} {}", [name, answer]);
    return 0;
}
```

That style follows the type of `println`. `println` takes a slice of interface values internally. The current stdlib implementation builds formatting on top of interface dispatch, `fwrite`, and `fdopen`.

The detail that matters for everyday use is the local bindings. `name` and `answer` are given concrete storage first, then passed into `[name, answer]`. That is the idiomatic pattern when building interface-value slices. It gives the compiler real sources to borrow from.

### Implementing `fmt` For Your Own Type

The same interface is open to your own types. The current stdlib exposes `StringBuilder` together with a few small helper functions so an external module can append text and numbers while implementing `fmt`. Built-in `fmt` support covers strings, booleans, characters, signed integers, unsigned integers, and floating-point values.

An unqualified import keeps the example short:

```cyan
import /std.println;
```

This standalone example is `docs/snippets/custom_fmt.cyan`.

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
    println("point={}", [point]);
    return 0;
}
```

Read the `impl` body as a tiny serializer. `fmt_write_cstr(out, "Point(");` appends a fixed prefix. `fmt_write_i64(out, value.x);` and `fmt_write_i64(out, value.y);` append the numeric fields. `fmt_write_char(out, ')');` closes the shape. The `println` call at the bottom then treats `Point` exactly like the built-in printable types because the interface contract is now satisfied.

The mental model is simple: `println` owns the output builder, your `fmt` implementation contributes bytes to that builder, and the formatting loop takes care of the surrounding template.

### Under The Hood: Interface Dispatch Must Stay Unambiguous

This example shows a rule that protects this system from becoming fuzzy:

```cyan
interface<T> i64 measure(&T value);

struct Box<T> {
    T value;
};

impl measure<T>(&Box<T> box) {
    return 0;
}

impl measure(&Box<i64> box) {
    return box.value;
}
```

The compiler reports `duplicate impl for interface 'measure' on 'Box'`. That is Cyan refusing ambiguous dispatch. If two `impl` blocks could both claim the same receiver shape, the language makes you resolve the conflict instead of picking one silently.

### `unchecked` Interface Contracts

Some contracts depend on invariants that the compiler cannot prove from the signature alone. Cyan lets you mark that kind of contract directly on the interface:

```cyan
unchecked interface<T> void publish(&T value);
```

The matching implementations carry the same marker:

```cyan
unchecked impl publish(&ChannelWriter writer) {
    // body
}
```

The signature still goes through the ordinary interface checker. The `unchecked` marker says the deeper semantic promise behind the contract comes from the library author rather than from structural analysis.

## 9. `unchecked`, Raw Pointers, Null, And The C Boundary

### Why Cyan Does Not Try To Eliminate Raw Memory

A systems language needs C interop, allocation, and raw pointers for kernels, parsers, runtimes, and foreign-function boundaries. Cyan keeps those operations available and marks the dangerous zone explicitly with `unchecked`.

### Mental Model

Picture an airlock between two rooms. One room is safe Cyan: borrows, slices, checked provenance, and explicit aliasing rules. The other room is raw memory: pointer arithmetic, const-changing casts, C APIs, and operations that the compiler cannot fully validate. `unchecked { ... }` is the airlock. You cross on purpose. You do the dangerous work. Then you step back out.

### The Code

This standalone example is `docs/snippets/drop_and_unchecked.cyan`.

```cyan
extern {
    void* malloc(u64 n);
    void free(void* p);
}

struct IntBox {
    i64* ptr;
};

impl drop(&mut IntBox self) {
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

i64 main() {
    IntBox box = new_box(7);
    unchecked {
        return *box.ptr - 7;
    }
}
```

### Walkthrough

`extern { ... }` declares foreign functions. Here they are `malloc` and `free`. Cyan treats them as raw C-facing functions, so the parameter and return types are FFI-friendly scalars or raw pointers.

`struct IntBox { i64* ptr; };` stores a raw pointer. Borrows carry lifetime rules checked by the compiler. Raw pointers are lower-level and therefore need more explicit care.

`impl drop(&mut IntBox self)` defines a destructor. When an `IntBox` goes out of scope, this `drop` implementation runs automatically.

Inside `drop`, the line `free(self.ptr as void*);` sits inside `unchecked`. The cast from `i64*` to `void*` is a raw-pointer cast, and Cyan only allows that in the explicit escape hatch.

`IntBox new_box(i64 value)` performs the matching allocation path. Inside `unchecked`, `i64* ptr = malloc(sizeof(i64));` asks C for enough bytes to store one `i64`. Then `*ptr = value;` writes into the allocated memory through the raw pointer. Finally, `return {ptr};` packages the pointer into the owning struct.

In `main`, `IntBox box = new_box(7);` creates a box that owns one heap allocation. The final `unchecked` block reads through the raw pointer with `*box.ptr`. When `main` ends, `box` drops, which in turn calls `free`.

### Null Raw Pointers Without A New Keyword

Cyan does not have a built-in `null` keyword. Instead, the standard library exposes null raw pointers through `/std.ptr`:

```cyan
import /std.ptr as ptr;

i64 main() {
    i64* counter = ptr.null<i64>();
    void* raw = ptr.null<void>();
    if (counter == ptr.null<i64>() && raw == ptr.null<void>()) {
        return 0;
    }
    return 1;
}
```

The helper behind this is small:

```cyan
extern {
    //@lower constant=null
    void* null_raw();
}

export T* null<T>() {
    unchecked {
        return null_raw() as T*;
    }
}
```

The important boundary is that `null<T>()` is for **raw pointers**. Borrows and slices are never nullable. A borrow is supposed to point at a live source.

### Under The Hood: What Must Stay Inside `unchecked`

One forbidden operation looks like this:

```cyan
extern {
    i64* get_buffer();
}

i64 main() {
    i64* ptr = get_buffer();
    return ptr[0];
}
```

The compiler reports `pointer indexing is only allowed in unchecked blocks`. The same operation becomes legal inside an explicit `unchecked` block.

Another boundary looks like this:

```cyan
i64 main() {
    i64 value = 7;
    const i64* read_only = &value;
    i64* writable = read_only as i64*;
    return *writable;
}
```

This fails with `const cast is only allowed in unchecked blocks`. Weakening constness is a manual escape hatch.

There is also one subtle null-related pitfall. `ptr.null<T>()` has no value argument, so Cyan cannot infer `T`. This tiny example shows it:

```cyan
import /std.ptr as ptr;

i64 main() {
    ptr.null();
    return 0;
}
```

The compiler reports `could not infer type argument for 'T'`. The fix is to write the type argument explicitly, for example `ptr.null<i64>()` or `ptr.null<void>()`.

## 10. High-Level Concurrency With `parallel_do`

### Why This Chapter Stops At The High Level

The earlier version of this tutorial compressed high-level task execution, thread-boundary markers, raw shared pointers, and atomics into one chapter. That is too steep for most readers, especially if you are coming from Java or Python rather than from Rust or C++ concurrency.

So this chapter does less on purpose.

Read this chapter first if your goal is ordinary application code:

- run a few tasks in parallel
- wait for them to finish
- protect shared state with `Mutex<T>`
- share heap state with `Arc<T>`

Leave `send`, `share`, `local`, `shared T*`, and atomics for the next chapter. Those are the lower-level boundary tools.

### Why Cyan Starts Here

Cyan's current threading surface centers on one straightforward tool: `parallel_do([]&threaded_runnable tasks);`. It launches a batch of tasks, waits for all of them to finish, and then returns to the caller.

That “waits for all of them” part matters. `parallel_do(...)` is a fork-join primitive. It is not “fire and forget.” It is not a detached background thread. It is closer to “run these jobs concurrently, then continue when the batch is done.”

### Mental Model

Think of `parallel_do(...)` as a foreman handing job cards to workers and then standing at the exit until every card comes back stamped complete.

If you prefer a Java/Python analogy:

- `run_task` is close to Java's `Runnable.run()` or a Python object with a `run()`-style method
- `parallel_do([a, b, c])` is close to “submit these three jobs and immediately join on all of them”
- `Mutex<T>` is the same old idea: only one worker may mutate the protected data at a time
- `Arc<T>` is shared ownership of one heap object so several tasks can all hold onto it safely

You do not need to understand raw atomics to use this layer.

### The Code

This file is `docs/snippets/threading_parallel_do.cyan`.

```cyan
import /std.sync;
import /std.thread;

struct AddTask {
    &Mutex<i64> total;
    i64 value;
};

impl run_task(&AddTask task) {
    MutexGuard<i64> guard = lock(task.total);
    *guard.data = *guard.data + task.value;
}

i64 main() {
    Mutex<i64> total = mutex_new(0);
    AddTask left = {&total, 4};
    AddTask middle = {&total, 7};
    AddTask right = {&total, 9};

    parallel_do([left, middle, right]);

    MutexGuard<i64> guard = lock(&total);
    return *guard.data - 20;
}
```

Run it the same way as the earlier snippets:

```sh
cyan docs/snippets/threading_parallel_do.cyan --check
cyan docs/snippets/threading_parallel_do.cyan -o /tmp/threading_parallel_do.o
```

### Walkthrough

Start with the task shape:

```cyan
struct AddTask {
    &Mutex<i64> total;
    i64 value;
};
```

Each task carries two things:

- a borrow of one shared counter protected by a mutex
- one increment value that this task should add

This is a common concurrent pattern. Several workers all need to update one shared piece of state, so they all point at the same protected object instead of each owning a private copy.

Now look at the execution entry point:

```cyan
impl run_task(&AddTask task) {
    MutexGuard<i64> guard = lock(task.total);
    *guard.data = *guard.data + task.value;
}
```

If you have not seen Cyan interfaces yet, the short reading is simple: this is the code that runs when the task is executed by the thread system.

`lock(task.total)` returns a `MutexGuard<i64>`. While that guard is alive, `guard.data` behaves like a mutable borrow of the protected integer. When the guard goes out of scope, the lock is released.

That is deliberately similar to the corresponding concept in many other languages:

- Java: `synchronized` block or `Lock.lock()` / `unlock()`
- Python: `with lock:` around shared mutable state
- C++: `std::lock_guard`

The names differ. The concurrency idea is the same.

Now read `main()`:

```cyan
Mutex<i64> total = mutex_new(0);
AddTask left = {&total, 4};
AddTask middle = {&total, 7};
AddTask right = {&total, 9};

parallel_do([left, middle, right]);
```

The important detail is that the tasks borrow `&total`, and that works because `parallel_do(...)` is blocking. The caller does not continue until all tasks are done, so the borrowed `total` is still alive for the whole parallel phase.

This is the first big difference between this chapter and detached-thread APIs in other languages. Detached work often forces you to move or heap-allocate everything up front. `parallel_do(...)` is simpler because the lifetime stays tied to one blocking call.

The final lines:

```cyan
MutexGuard<i64> guard = lock(&total);
return *guard.data - 20;
```

are ordinary sequential code again. The batch has finished. The program locks the result, reads it, and returns.

### What `run_task` And `threaded_runnable` Mean

`import /std.thread;` gives you `parallel_do(...)` plus the small interface vocabulary that describes runnable tasks:

```cyan
interface<T> void run_task(&T task);
interface thread_shared = share - local;
interface thread_sendable = send - local;
interface threaded_runnable = run_task + thread_shared;
```

For this chapter, you only need one simplified reading:

- `run_task` means “this type knows how to execute itself as a task”
- `threaded_runnable` means “this type can safely cross the worker boundary used by `parallel_do(...)`”

The low-level marker words inside that definition, especially `share` and `local`, are the subject of the next chapter. For now, treat them as the thread-safety gate behind `parallel_do(...)`.

### Two Common High-Level Shapes

The mutex example covers one common pattern: several tasks borrowing one shared mutable value from the caller.

The next common pattern is shared heap state through `Arc<T>`:

```cyan
struct SharedState {
    Mutex<i64> total;
};

struct ArcTask {
    Arc<SharedState> state;
    i64 value;
};

impl run_task(&ArcTask task) {
    &SharedState state = arc_get(&task.state);
    MutexGuard<i64> guard = lock(&state.total);
    *guard.data = *guard.data + task.value;
}
```

`Arc<T>` is the “many tasks may all keep a handle to the same heap object” tool. If you have used Rust before, it is the same broad idea as `Arc`. If you have not, think of it as a reference-counted shared object handle.

Why use `Arc<T>` instead of a plain borrow?

- use a plain borrow when the caller obviously owns the shared state and the work is tightly scoped to one blocking `parallel_do(...)`
- use `Arc<T>` when the shared state itself should live on the heap and be passed around as one reusable object

In day-to-day code, `Mutex<T>` plus `Arc<T>` will cover most of what you need.

### Under The Hood: The Easy Wrong Turn

A beginner mistake is to think, “I already know what a pointer is, so I will just share a raw pointer between tasks.”

This task does exactly that:

```cyan
import /std.thread;

struct PtrTask {
    i64* ptr;
};

impl run_task(&PtrTask task) {}
```

and `parallel_do([task])` rejects it with `does not satisfy marker 'share'`.

That message is good news. Cyan is refusing to treat an ordinary raw pointer as automatically safe for cross-thread sharing.

The practical rule is:

- for ordinary code, reach for `Mutex<T>` or `Arc<T>` first
- do not jump to raw pointers just because they look familiar
- save `shared T*` and atomics for the lower-level chapter

### What To Keep In Your Head

If you only want the 80% everyday subset of Cyan concurrency, keep these rules:

1. `parallel_do(...)` runs a batch and waits for the batch to finish.
2. `run_task` is the task body.
3. Shared mutable state usually means `Mutex<T>`.
4. Shared heap ownership usually means `Arc<T>`.
5. If the next chapter feels too low-level on first read, skip it and come back later.

## 11. Thread Boundaries, Markers, And Atomics

### Why This Is A Separate Chapter

This is the layer that confused many readers in the old tutorial because it appeared too early and too densely.

You need this chapter when you are doing one of these things:

- wrapping a low-level handle
- deciding whether a type may cross thread boundaries
- exposing a concurrency primitive in a library
- doing lock-free coordination with atomics

If you are only trying to write application code with `parallel_do(...)`, `Mutex<T>`, and `Arc<T>`, you can skim this chapter once and come back later.

### Three Marker Words: `local`, `send`, And `share`

These three words are the thread-boundary vocabulary. If you know Rust, they are closely related to `!Send`, `Send`, and `Sync`. If you do not know Rust, read them in plain English.

`local`

- meaning: this value is tied to one specific thread
- typical examples: GUI objects, rendering contexts, event loops, objects whose API says “must stay on the creating thread”
- consequence: Cyan must stop you from handing it to worker threads

`send`

- meaning: ownership may move to another thread
- picture: mailing one physical book to somebody else; after mailing it, you do not still hold the same book locally
- consequence: safe for ownership transfer across a thread boundary

`share`

- meaning: several threads may access the same value safely
- picture: several people editing one shared document, but only because the system has real coordination around the document
- consequence: safe for shared access across a thread boundary

The most important beginner-level lesson is that these are not decorative labels. They exist to prevent concrete disasters:

- `local` prevents thread-affine resources from being used on the wrong thread
- `send` prevents ownership-moving APIs from receiving values that cannot survive a thread handoff
- `share` prevents shared-access APIs from receiving values that would race or break internally

### How `/std.thread` Uses Those Markers

The standard thread interfaces are built out of those markers:

```cyan
interface<T> void run_task(&T task);
interface thread_shared = share - local;
interface thread_sendable = send - local;
interface threaded_runnable = run_task + thread_shared;
```

Read them in plain language:

- `thread_shared = share - local` means “shareable across threads, but not thread-bound”
- `thread_sendable = send - local` means “movable across threads, but not thread-bound”
- `threaded_runnable = run_task + thread_shared` means “a runnable task whose value may be shared safely across the `parallel_do(...)` boundary”

That is why `parallel_do(...)` rejects certain task types. It is not arbitrary. The task must cross a concurrency boundary, so Cyan checks the markers that describe whether that crossing is valid.

### `local`: Thread-Bound Values

When a type is tied to one thread by meaning rather than by obvious field layout, mark it with `impl local(...)`:

```cyan
struct GLContext {
    i64 id;
};

impl local(GLContext);
```

The mental model is simple: this value is welded to the thread that owns it.

That is exactly the right rule for objects such as:

- an OpenGL or Vulkan context bound to one thread
- a GUI widget or window object tied to the UI thread
- any foreign-library handle whose documentation says “must be used from the creating thread”

### Under The Hood: `local` Blocking A Task

Here is a task that tries to carry a thread-bound graphics context through `parallel_do(...)`:

```cyan
import /std.sync;
import /std.thread;

struct GLContext {
    i64 id;
};

impl local(GLContext);

struct RenderTask {
    Arc<GLContext> ctx;
};

impl run_task(&RenderTask task) {}
```

The compiler rejects it with `matches excluded marker 'local'`.

That message is the concurrency equivalent of a seat belt locking. `parallel_do(...)` wants a `thread_shared` task. `thread_shared` means `share - local`. The task contains a `local` thing, so it fails the boundary on purpose.

### `send`: Ownership Transfer Across Threads

`send` is about handing ownership from one thread to another.

The easiest picture is still the physical book:

- before transfer, thread A owns the book
- after transfer, thread B owns the book
- there is still only one book

Most plain data shapes are naturally sendable. If a type is made of ordinary values whose ownership can move cleanly, there is usually nothing surprising going on.

The hard cases are opaque wrapper types:

```cyan
struct OpaqueTicket {
    void* raw;
};
```

From the outside, the compiler cannot tell whether `raw` is safe to move to another thread. That is why explicit marker implementations for nominal types use `unchecked`.

```cyan
unchecked impl send(OpaqueTicket);
```

This line means, “the library author takes responsibility for the claim that values of this type may be transferred across a thread boundary.”

If you try to write the marker without `unchecked`, Cyan rejects it. The corresponding diagnostics are `impl send(...) must be declared unchecked` and `impl share(...) must be declared unchecked`.

### `share`: Safe Shared Access Across Threads

`share` is the companion concept for shared access.

The simple intuition is:

- if several threads can all see the same value
- and that is still safe
- then the type must have the right internal synchronization story

Sometimes that story is structural and obvious:

- a `Mutex<T>` protects mutable access
- an `Arc<T>` shares ownership of a heap object
- immutable plain data can often be shared safely

Sometimes the story is hidden inside a nominal wrapper, and only the library author really knows the invariant. That is the other case for `unchecked impl share(...)`.

```cyan
struct OpaqueHandle {
    void* raw;
};

unchecked impl share(OpaqueHandle);
```

The line is a promise. Use it only when the promise is true.

### `shared T*`: A Raw Pointer Meant For Cross-Thread Coordination

Now to the piece that tends to look cryptic on first exposure.

An ordinary `T*` is just a raw pointer. It says nothing about thread-sharing intent.

A `shared T*` is different. It is a raw pointer explicitly marked as participating in cross-thread coordination.

That distinction is important because raw pointers are where the compiler stops being able to infer much for you. The `shared` qualifier is the extra bit of information that says, “this pointer is part of a concurrency story, not just generic pointer arithmetic.”

Here is the smallest possible example:

```cyan
i64 counter = 0;
shared i64* total;
unchecked {
    total = (&mut counter) as shared i64*;
}
atomic_fetch_add(total, 1, atomic_seq_cst());
```

Break that into three separate ideas:

1. `counter` is still an ordinary integer owner.
2. `total` is a raw pointer variable that is explicitly marked `shared`.
3. `atomic_fetch_add(...)` only accepts the `shared` version, because atomic operations are a cross-thread coordination primitive.

If you are coming from C or C++, the language is forcing you to say out loud what kind of pointer you intend to use at the thread boundary. That extra ceremony is the point.

### Under The Hood: Why Bare Raw Pointers Are Rejected

Passing a plain raw pointer where a shared pointer is required fails with the direct diagnostic `type mismatch: expected shared i64*, got i64*`.

Using a plain raw pointer directly with an atomic operation fails with `atomic operations require a shared pointer operand`.

This small failing program shows the second case:

```cyan
i64 main() {
    i64 value = 0;
    i64* ptr = &mut value;
    atomic_fetch_add(ptr, 1, atomic_seq_cst());
    return 0;
}
```

The rule is simple once you stop fighting it:

- ordinary `T*` is just a raw pointer
- `shared T*` is the raw pointer form reserved for cross-thread coordination
- atomics only work on the shared form

### When To Actually Use Atomics

Use atomics when the shared state is truly small and low-level:

- counters
- flags
- pointer-sized coordination cells

Do not default to atomics just because they sound faster. In application code, `Mutex<T>` is usually easier to reason about and easier to extend when the shared state stops being “just one integer.”

That is why this tutorial separated the topics:

- chapter 10: everyday concurrency
- chapter 11: boundary markers and low-level coordination

### The Working Set

The pieces that work together today are:

- `/std.thread` with `run_task`, `thread_shared`, `thread_sendable`, `threaded_runnable`, and `parallel_do(...)`
- `/std.sync` with `Mutex<T>`, `MutexGuard<T>`, `Arc<T>`, `arc_get(...)`, and `lock(...)`
- `shared T*` for raw cross-thread state
- atomic operations and memory-order constants
- `impl local(...)`, `unchecked impl send(...)`, and `unchecked impl share(...)` for nominal thread properties

That is enough to cover both the high-level “run these tasks and wait” style and the lower-level “I am defining a thread boundary in a library” style.

## 12. Practical Walkthrough: A Zero-Copy HTTP Request Parser

### Why This Example Matters

Toy examples teach surface syntax. Real examples teach the language. The repository's zero-copy HTTP request example reads one request into one buffer, then builds a structured result that points back into that buffer without copying the parsed fields.

This chapter matters because it combines the tutorial's two hardest ideas in a practical way:

- slices are views, not owners
- `depends(...)` turns those views into a checked contract

### Mental Model

The stack array is the house. The parsed request object is a bundle of windows cut into that house. The parser never manufactures owned strings for the method, path, headers, or body. It only computes where each piece begins and how long it is.

If you prefer a higher-level analogy, think of one `byte[]` plus several tiny `(start, length)` objects that all point back into it. Cyan keeps that design explicit instead of hiding it behind heap allocation.

This is the concrete request used by the companion C fixture:

```text
POST /submit?lang=safe&mode=zero HTTP/1.1\r\n
Host: example.test\r\n
User-Agent: cyan/0\r\n
Content-Type: text/plain\r\n
Content-Length: 5\r\n
\r\n
hello
```

The parser points slices at the right byte ranges of the original request.

### The Code

The full file is long, so start with the three pieces that matter most.

First, the parsed data is made of ordinary structs containing slices:

```cyan
struct HttpHeader {
    []const char name;
    []const char value;
};

struct RequestTarget {
    []const char raw;
    []const char path;
    []const char query;
};

struct HttpRequest {
    RequestLine line;
    HeaderBag headers;
    []const char body;
    i64 content_length_value;
};
```

Second, the parser carves views out of the request bytes:

```cyan
HttpHeader parse_header_line([]const char bytes, i64 line_start, i64 line_end)
    depends(return on bytes) {
    i64 colon = find_byte_in_range(bytes, line_start, line_end, ':');
    []const char name = subslice(bytes, line_start, colon - line_start);
    i64 value_start = colon + 1;
    if (value_start < line_end && bytes[value_start] == ' ') {
        value_start++;
    }
    []const char value = subslice(bytes, value_start, line_end - value_start);
    return {name, value};
}
```

Third, the entry point fills one stack buffer and parses from that one owner:

```cyan
char[256] buffer;
i64 byte_count;
unchecked {
    byte_count = mock_http_read(&mut buffer[0], len(buffer));
}
if (byte_count <= 0) {
    return 1;
}

[]const char bytes = subslice(buffer, 0, byte_count);
HttpRequest parsed = blank_request(bytes);
if (!parse_http_request(bytes, &mut parsed)) {
    return 2;
}
```

### Walkthrough

The struct declarations are intentionally ordinary. Cyan’s trick is not that it invented a magical parser object. The trick is that fields like `[]const char name;` and `[]const char body;` are plain slice views. Once you understand those field types, the rest of the parser becomes a controlled exercise in computing offsets.

Look at `parse_header_line(...) depends(return on bytes)`. The function receives the whole request slice plus two indices describing one header line. `find_byte_in_range` locates the colon. `subslice(bytes, line_start, colon - line_start)` creates a slice for the header name. The code then skips one optional space and builds another slice for the header value. The final `return {name, value};` returns a struct of views, not copied strings.

Notice how directly this connects back to chapter 7:

- `bytes` is the real source buffer
- `name` and `value` are two windows into that buffer
- `depends(return on bytes)` is the signature-level proof that both windows come from there

Now zoom out to `parse_http_request([]const char bytes, &mut HttpRequest out) depends(out on bytes)`. This signature says that the function will rewrite `out` so that its internal slice fields all point into `bytes`. That is the whole parser contract in one line.

The body first resets `out` with `*out = blank_request(bytes);`. This is more important than it looks. Instead of leaving fields uninitialized or using null sentinels, the parser fills the output struct with empty views that are already known to derive from `bytes`. That gives the function a clean, valid starting state.

Next the parser finds the request line boundaries. `find_crlf(bytes, 0)` locates the end of the first line. `find_byte_in_range` then finds the spaces that separate method, target, and HTTP version. `build_request_line(...)` packages those three slices into a structured `RequestLine`.

After that, the parser walks header lines with a `cursor`. Each pass finds the next `\r\n`. If the parser sees an empty line, it has reached the boundary between headers and body. At that point, `[]const char body = subslice(bytes, cursor, len(bytes) - cursor);` creates the body view, the code assembles `HeaderBag`, and `*out = {line, headers, body, content_length_value};` stores the final structured result.

The `main()` function tells the memory story clearly:

- `char[256] buffer;` owns the bytes
- `mock_http_read(&mut buffer[0], len(buffer));` asks foreign code to fill the array
- `[]const char bytes = subslice(buffer, 0, byte_count);` turns the valid prefix into a slice view
- `parse_http_request(bytes, &mut parsed)` builds a network of smaller views that all still point back into `buffer`

That is zero-copy parsing in one sentence: one owner, many views.

### Why `depends(out on bytes)` Is Still Worth Writing

Even when a mutable-output helper looks simple, omitting the dependency contract does not make the provenance problem disappear. It can simply move the failure later.

If a function writes one of several possible views into `out`, the call itself may type-check, but later code that tries to use the rewritten field can hit the diagnostic `cannot use a view value whose source is unknown after this call or control-flow path`.

That is why `depends(out on bytes)` is worth writing even before the compiler absolutely forces you to. It buys you three things:

1. the signature states the parser's memory contract in one line
2. future readers do not have to reverse-engineer where all of `out`'s slice fields came from
3. the compiler has a precise story to check when the parser evolves and starts handling more than one possible source

### Under The Hood: Two Parser-Shaped Dependency Mistakes

The first mistake is omission. Here the function stores either `bytes` or `fallback` into `out.body`, but the signature says nothing:

```cyan
struct BodyOnly {
    []const char body;
};

void choose_body(&mut BodyOnly out,
                 []const char bytes,
                 []const char fallback,
                 bool use_fallback) {
    if (use_fallback) {
        out.body = fallback;
        return;
    }
    out.body = bytes;
}
```

A later use of `out.body` can fail with `cannot use a view value whose source is unknown after this call or control-flow path`.

The second mistake is a wrong promise. Here the function explicitly says `out` depends on `bytes`, but the body stores `fallback`:

```cyan
struct BodyOnly {
    []const char body;
};

void set_body(&mut BodyOnly out, []const char bytes, []const char fallback)
    depends(out on bytes) {
    out.body = fallback;
}
```

That fails with `mutable parameter borrow or slice does not match its declared depends source`.

Together, these two failures explain why `depends(...)` matters in parser code. One prevents provenance from going unknown. The other prevents a wrong provenance story from silently becoming part of your API contract.

### Where To Study The Full Breakdown

The companion document [Zero-Copy HTTP Walkthrough](http-zero-copy-walkthrough.md) spends much more time on this file. It follows the real request byte by byte, explains why the parser builds empty views first, and shows how `depends(out on bytes)` keeps the provenance story intact from start to finish.

The repository also contains `examples/http_parse/main.cyan`, which runs the same basic design against a real loopback socket and prints the parsed fields with `/std.println`.

## 13. Reading Cyan’s Error Messages

The fastest way to get productive in Cyan is to learn how the compiler is thinking. The failing cases are good at exposing that thinking because each one is small and deliberate.

When Cyan says `view-returning functions with multiple borrow or slice parameters require depends(return on <param>)`, it is telling you that provenance is ambiguous and must be stated.

When Cyan says `view-returning function must depend on a borrow or slice parameter`, it is warning that you are trying to return a view into storage with no safe incoming owner to attach it to.

When Cyan says `functions returning aggregates with borrow or slice fields require an explicit depends clause`, it is telling you that a struct or enum full of views cannot leave the function without a provenance contract.

When Cyan says `returned borrow does not match depends(return on x)`, it is pointing at a mismatch between the promise in the signature and the values actually returned in the body.

When Cyan says `mutable parameter borrow or slice does not match its declared depends source`, it is pointing at the mutable-output version of the same problem.

When Cyan says `cannot use a view value whose source is unknown after this call or control-flow path`, it is telling you that a function or branch produced a view whose provenance was never pinned down tightly enough for later use.

When Cyan says `matches excluded marker 'local'`, it is usually because a thread-bound value tried to cross a thread boundary that explicitly excludes `local`.

When Cyan says `does not satisfy marker 'share'`, it is usually because some value, often a raw pointer or wrapper type, is being passed into a shared-thread boundary without proving it is safe for shared access.

When Cyan says `atomic operations require a shared pointer operand`, it is telling you that atomics only work on `shared T*`, not ordinary raw pointers.

When Cyan says `borrow source is not live`, `cannot assign through shared borrow`, or `cannot borrow ... as mutable more than once`, it is describing an aliasing or lifetime conflict. Read those messages literally before you assume there is a deeper trick.

When Cyan says `pointer indexing is only allowed in unchecked blocks` or `const cast is only allowed in unchecked blocks`, it is enforcing the airlock boundary between ordinary Cyan and raw memory operations.

When Cyan says `could not infer type argument for 'T'`, it usually means exactly what it says: the call site did not provide enough evidence. The fix is often to bind a value to a typed local or to spell the type argument explicitly.

## 14. Where To Go Next

If you want one file that touches many language features in a compact space, read `examples/all_features.cyan`. It shows slices, `depends(...)`, interfaces, `switch`, `unchecked`, and `/std.println` in one program.
