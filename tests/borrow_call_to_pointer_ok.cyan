&int first(&int value) {
    return value;
}

int main() {
    int value = 0;
    int* ptr = &*first(&value);
    unchecked {
        *ptr = 12;
    }
    return value;
}
