extern {
    void* malloc(size_t n);
    void free(void* p);
    void emit_str(const char* text);
}

struct StringBuilder {
    char* ptr;
    size_t len;
    size_t capacity;
};

impl drop(&mut StringBuilder self) {
    unchecked {
        free(self.ptr);
    }
}

StringBuilder builder_new(size_t capacity) {
    unchecked {
        char* memory = malloc(capacity);
        memory[0] = '\0';
        return {memory, 0, capacity};
    }
}

void builder_reserve(&mut StringBuilder out, size_t needed) {
    if (needed <= out.capacity) {
        return;
    }

    size_t next_capacity = out.capacity;
    while (next_capacity < needed) {
        next_capacity = next_capacity * 2;
    }

    unchecked {
        char* next = malloc(next_capacity);
        size_t i = 0;
        while (i < out.len) {
            next[i] = out.ptr[i];
            i = i + 1;
        }
        next[out.len] = '\0';
        free(out.ptr);
        out.ptr = next;
        out.capacity = next_capacity;
    }
}

void builder_push_char(&mut StringBuilder out, char value) {
    size_t needed = out.len + 2;
    builder_reserve(out, needed);
    unchecked {
        out.ptr[out.len] = value;
        out.len = out.len + 1;
        out.ptr[out.len] = '\0';
    }
}

size_t cstring_len(const char* text) {
    unchecked {
        size_t len = 0;
        const char* cursor = text;
        while (*cursor != '\0') {
            len = len + 1;
            cursor = cursor + 1;
        }
        return len;
    }
}

void builder_append_cstring(&mut StringBuilder out, const char* text) {
    size_t text_len = cstring_len(text);
    size_t needed = out.len + text_len + 1;
    builder_reserve(out, needed);

    unchecked {
        size_t i = 0;
        while (i < text_len) {
            out.ptr[out.len + i] = text[i];
            i = i + 1;
        }
        out.len = out.len + text_len;
        out.ptr[out.len] = '\0';
    }
}

void builder_append_slice(&mut StringBuilder out, []const char text) {
    size_t needed = out.len + len(text) + 1;
    builder_reserve(out, needed);

    int i = 0;
    while (i < len(text)) {
        unchecked {
            out.ptr[out.len + i] = text[i];
        }
        i++;
    }

    unchecked {
        out.len = out.len + len(text);
        out.ptr[out.len] = '\0';
    }
}

void builder_push_digit(&mut StringBuilder out, int digit) {
    int ascii = ('0' as int) + digit;
    builder_push_char(out, ascii as char);
}

void builder_append_positive_int(&mut StringBuilder out, int value) {
    if (value >= 10) {
        builder_append_positive_int(out, value / 10);
    }
    builder_push_digit(out, value % 10);
}

void builder_append_negative_digits(&mut StringBuilder out, int value) {
    if (value <= -10) {
        builder_append_negative_digits(out, value / 10);
    }
    builder_push_digit(out, 0 - (value % 10));
}

void builder_append_int(&mut StringBuilder out, int value) {
    if (value < 0) {
        builder_push_char(out, '-');
        builder_append_negative_digits(out, value);
        return;
    }
    builder_append_positive_int(out, value);
}

export interface<T> void fmt(&T value, &mut StringBuilder out);

impl fmt(&[]const char value, &mut StringBuilder out) {
    builder_append_slice(out, *value);
}

impl fmt(&int value, &mut StringBuilder out) {
    builder_append_int(out, *value);
}

void render_into(&mut StringBuilder out, const char* format_text, []&fmt args) {
    int next_arg = 0;

    unchecked {
        const char* cursor = format_text;
        while (*cursor != '\0') {
            if (cursor[0] == '{' && cursor[1] == '}') {
                if (next_arg < len(args)) {
                    fmt(args[next_arg], out);
                    next_arg++;
                } else {
                    builder_append_cstring(out, "<missing>");
                }
                cursor = cursor + 2;
                continue;
            }

            if (cursor[0] == '{' && cursor[1] == '{') {
                builder_push_char(out, '{');
                cursor = cursor + 2;
                continue;
            }

            if (cursor[0] == '}' && cursor[1] == '}') {
                builder_push_char(out, '}');
                cursor = cursor + 2;
                continue;
            }

            builder_push_char(out, *cursor);
            cursor = cursor + 1;
        }
    }

    if (next_arg < len(args)) {
        builder_append_cstring(out, " <extra args>");
    }
}

export void print(const char* format_text, []&fmt args) {
    StringBuilder out = builder_new(64);
    render_into(&mut out, format_text, args);
    emit_str(out.ptr);
}

export void println(const char* format_text, []&fmt args) {
    StringBuilder out = builder_new(64);
    render_into(&mut out, format_text, args);
    builder_push_char(&mut out, '\n');
    emit_str(out.ptr);
}

export void print0(const char* format_text) {
    []&fmt args = [];
    print(format_text, args);
}

export void println0(const char* format_text) {
    []&fmt args = [];
    println(format_text, args);
}
