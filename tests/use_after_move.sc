struct Box {
    int value;
};

int take(Box b) {
    return b.value;
}

int main() {
    Box b;
    Box other = b;
    return b.value;
}
