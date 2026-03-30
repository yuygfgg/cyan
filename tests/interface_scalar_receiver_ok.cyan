interface<T> int measure(&T value);

impl measure(&int value) {
    return *value;
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

int call_dyn(&measure value) {
    return measure(value);
}

int main() {
    int left = 4;
    int right = 8;
    if (measure(right) != 8) {
        return 1;
    }
    if (call_dyn(left) != 4) {
        return 2;
    }
    if (sum([left, right]) != 12) {
        return 3;
    }
    return 0;
}
