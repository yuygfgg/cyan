interface<T> int measure(&T value);

struct Box {
    int value;
};

impl measure(&Box value) {
    return value.value;
}

int main() {
    []&measure views = [];
    {
        Box box = {5};
        views = [box];
    }
    return measure(views[0]);
}
