interface<T> int measure(&T value);

struct Box {
    int value;
};

impl measure(&Box box) {
    return box.value;
}

int call_dyn(&measure value) {
    return measure(value);
}

int main() {
    Box box = {11};
    return call_dyn(&box);
}
