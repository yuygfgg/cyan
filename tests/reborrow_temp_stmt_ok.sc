int take(&mut int value) {
    *value = 5;
    return *value;
}

int main() {
    int value = 0;
    &mut int borrow = &value;
    take(&mut *borrow);
    *borrow = 2;
    return value;
}
