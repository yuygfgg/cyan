int read(&int value) {
    return *value;
}

int main() {
    int left = 1;
    int right = 2;
    &int selected = &left;
    if (left < right) {
        selected = &left;
    } else {
        selected = &right;
    }
    return read(selected) - 1;
}
