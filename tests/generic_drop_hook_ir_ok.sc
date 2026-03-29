struct Box<T> {
    T value;
};

impl drop<T>(&mut Box<T> value) {
    return;
}

int main() {
    Box<int> value = {1};
    return 0;
}
