struct Pair {
    int x;
    int y;
};

int sum(&Pair pair) {
    return pair.x + pair.y;
}

int main() {
    Pair pair = {1, 2};
    return sum(&pair);
}
