struct Pair {
    &int first;
    &int second;
};

int read_first(Pair pair) {
    return *pair.first;
}

int main() {
    int a = 4;
    int b = 5;
    int c = 7;
    int d = 8;
    Pair pair = {&a, &b};
    if (a < c) {
        pair = {&a, &b};
    } else {
        pair = {&c, &d};
    }
    return read_first(pair) - 4;
}
