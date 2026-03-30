void store(&mut int out, int value) {
    *out = value;
}

interface<T> void fmt(&T value, &mut int out);

struct Box {
    int value;
};

impl fmt(&Box value, &mut int out) {
    store(&mut out, value.value);
}

int main() {
    Box box = {7};
    int out = 0;
    fmt(box, &mut out);
    return out - 7;
}
