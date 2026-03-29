&int first(&int x) {
    return x;
}

int read(&int x) {
    return *x;
}

int main() {
    int value = 6;
    return read(first(&value));
}
