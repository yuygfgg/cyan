struct Box {
    int value;
};

int main() {
    Box b;
    if (true) {
        Box other = b;
    } else {
        b = b;
    }
    return 0;
}
