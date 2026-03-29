interface<T> int measure(&T value);

struct Box<T> {
    T value;
};

impl measure<T>(&Box<T> box) {
    return 0;
}

impl measure(&Box<int> box) {
    return box.value;
}

int main() {
    Box<int> box = {1};
    return measure(box);
}
