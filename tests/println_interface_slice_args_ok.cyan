interface<T> void fmt(&T value, &mut int out);

struct Box {
    int value;
};

impl fmt(&Box value, &mut int out) {
    *out = *out + value.value;
}

void render_into(&mut int out, const char* format_text, []&fmt args) {
    int i = 0;
    while (i < len(args)) {
        fmt(args[i], out);
        i++;
    }
}

void println(const char* format_text, []&fmt args) {
    int out = 0;
    render_into(&mut out, format_text, args);
}

int main() {
    Box left = {4};
    Box right = {8};
    int out = 0;
    render_into(&mut out, "{}", [&left, &right]);
    if (out != 12) {
        return 1;
    }
    println("{}", [&left, &right]);
    return 0;
}
