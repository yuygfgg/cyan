int add(int lhs, int rhs) {
    return lhs + rhs;
}

int main() {
    int value = add(2, 3);
    if (value > 4) {
        return value;
    } else {
        return 0;
    }
}
