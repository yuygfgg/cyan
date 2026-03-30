import http_parse.println;

// Use extern to import C functions.
// None is needed in this example though.
// extern {
//     void* malloc(size_t n);
//     void free(void* p);
// }

// `export` makes a declaration visible to importing modules. This enum is also
// generic, so `Option<T>` demonstrates parameterized user-defined types.
export enum Option<T> {
    None,
    Some(T)
};

// Structs can freely mix ordinary fields with slice or borrow view fields.
struct BufferView {
    []const char bytes;
    bool active;
};

struct Pair {
    int left;
    int right;
};

struct Split {
    []const char head;
    []const char tail;
};

struct HighlightedSplit {
    Split parts;
    []const char whole;
};

// Interfaces declare ad-hoc polymorphic operations selected from the receiver
// type's matching `impl`.
export interface<T> int compute(&T value);

impl compute(&int value) {
    return *value;
}

impl compute(&Pair value) {
    return value.left + value.right;
}

// `impl drop(...)` registers a destructor that runs automatically on scope
// exit or explicit `drop(...)`.
impl drop(&mut BufferView self) {
    self.active = false;
}

// `depends(return on text)` says the returned slice is a zero-copy view into
// the parameter named on the right-hand side.
[]const char skip_prefix([]const char text, int count) depends(return on text) {
    if (count >= len(text)) {
        return subslice(text, 0, 0);
    }
    return subslice(text, count, len(text) - count);
}

// The shorthand also works recursively for aggregate returns: every
// borrow/slice leaf inside `return` is treated as depending on `text`.
Split split_at([]const char text, int mid)
    depends(return on text) {
    if (mid <= 0) {
        return {subslice(text, 0, 0), text};
    }
    if (mid >= len(text)) {
        return {text, subslice(text, 0, 0)};
    }
    return {subslice(text, 0, mid), subslice(text, mid, len(text) - mid)};
}

// More specific target paths override broader ones. Here every returned leaf
// depends on `text` except `return.parts.tail`, which depends on `focus`.
HighlightedSplit highlight_split([]const char text, []const char focus, int mid)
    depends(return on text, return.parts.tail on focus) {
    if (mid <= 0) {
        return {{subslice(text, 0, 0), focus}, text};
    }
    if (mid >= len(text)) {
        return {{text, focus}, text};
    }
    return {{subslice(text, 0, mid), focus}, text};
}

// `depends(out on text)` is the mutable-parameter shorthand: after the call,
// every borrow/slice leaf stored into `out` is treated as borrowing from
// `text` unless a more specific `out... on ...` entry overrides it.
void reset_view(&mut BufferView out, []const char text, int start)
    depends(out on text) {
    out.bytes = skip_prefix(text, start);
    out.active = true;
}

// `depends(return.Some on slice)` shows that dependency paths can walk into an
// enum payload, here the `Some(T)` payload of `Option<T>`.
Option<&T> first_element<T>([]T slice) depends(return.Some on slice) {
    if (len(slice) > 0) {
        return Some(&slice[0]);
    }
    return None();
}

// Borrow-returning functions use `depends(return on pair)` to tie the returned
// `&mut` to the source mutable borrow parameter.
&mut int first_mut(&mut Pair pair) depends(return on pair) {
    return &mut pair.left;
}

// `while` is the basic looping construct, and slice indexing reads through the
// view created by `subslice(...)`.
int sum_slice([]int values) {
    int i = 0;
    int total = 0;
    while (i < len(values)) {
        total = total + values[i];
        i++;
    }
    return total;
}

export int main() {
    // `subslice(...)` creates a zero-copy slice view over an array or string
    // literal without copying the underlying bytes.
    []const char greeting = subslice("safe-c demo", 0, 11);
    Split pieces = split_at(greeting, 6);
    HighlightedSplit highlighted =
        highlight_split(greeting, subslice("demo", 0, 4), 5);

    BufferView view = {pieces.tail, false};
    reset_view(&mut view, pieces.head, 2);

    int[4] numbers = [10, 20, 30, 40];
    []int tail = subslice(numbers, 1, len(numbers) - 1);
    int total = sum_slice(tail);

    Option<&int> first = first_element(tail);
    int picked = -1;

    // `switch (&value)` pattern-matches an enum by shared borrow, so the
    // original enum remains available after the switch.
    switch (&first) {
        case Some(value):
            picked = **value;
        case None:
            picked = 0;
    }

    Pair pair = {picked, total};
    int combined = compute(&pair);

    {
        &mut Pair pair_mut = &pair;
        &mut int left = first_mut(pair_mut);
        *left = *left + 5;
    }

    int adjusted = pair.left;

    // Raw pointers are available, but dereference and pointer mutation require
    // an explicit `unchecked` block.
    unchecked {
        int* ptr = &mut adjusted;
        *ptr = *ptr + 1;
    }

    // `println(..., [..])` passes a slice of interface values; each argument is
    // coerced to the exported `fmt` interface implemented by `println.sc`.
    println("head={} focus={} view={} total={} sum={} adjusted={}",
            [pieces.head, highlighted.parts.tail, view.bytes, total, combined,
             adjusted]);

    return 0;
}
