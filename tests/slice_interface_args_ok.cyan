interface<T> int measure(&T value);

struct Box {
    int value;
};

impl measure(&Box box) {
    return box.value;
}

int sum([]&measure values) {
    int i = 0;
    int total = 0;
    while (i < len(values)) {
        total = total + measure(values[i]);
        i++;
    }
    return total;
}

int main() {
    Box left = {4};
    Box right = {8};
    []&measure values = [&left, &right];
    if (sum(values) != 12) {
        return 1;
    }
    if (sum([&left, &right]) != 12) {
        return 1;
    }
    return 0;
}
