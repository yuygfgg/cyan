int take_mut(&mut int value) {
    *value = 3;
    return *value;
}

int main() {
    int value = 0;
    &mut int first = &value;
    &int second = &*first;
    return take_mut(first);
}
