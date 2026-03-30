int fill(int* out) {
    unchecked {
        *out = 11;
    }
    return 0;
}

int main() {
    int value;
    unchecked {
        fill(&mut value);
    }
    return value - 11;
}
