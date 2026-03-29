int main() {
    int value = 7;
    const int* read_only = &value;
    int* writable = read_only as int*;
    return *writable;
}
