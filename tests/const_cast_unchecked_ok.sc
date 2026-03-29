int main() {
    int value = 7;
    const int* read_only = &value;
    unchecked {
        int* writable = read_only as int*;
        *writable = 9;
    }
    return value;
}
