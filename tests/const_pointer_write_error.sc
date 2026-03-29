int main() {
    int value = 1;
    const int* ptr = &value;
    unchecked {
        *ptr = 2;
    }
    return value;
}
