struct Box<T> {
    T value;
};

int read_box<T>(&Box<T> box) {
    return box.value;
}

int main() {
    Box<int> box = {7};
    &mut Box<int> alias = &mut box;
    return read_box(alias) - 7;
}
