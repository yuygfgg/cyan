int write_twice(&mut int first, &mut int second) {
    *first = 1;
    *second = 2;
    return 0;
}

int main() {
    int value = 0;
    return write_twice(&value, &value);
}
