struct Token {
    int value;
};

impl drop(&mut Token token) {
    record_drop();
}

bool again([]Token values, int i) {
    return i < 3;
}

extern {
    int dropped_count();
    void record_drop();
}

int run() {
    int i = 0;
    while (again([{1}], i)) {
        i++;
    }
    return 0;
}

int main() {
    run();
    return dropped_count();
}
