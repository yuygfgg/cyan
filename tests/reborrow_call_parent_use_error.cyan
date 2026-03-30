&mut int id_mut(&mut int value) {
    return value;
}

int main() {
    int value = 0;
    &mut int first = &mut value;
    &mut int second = id_mut(first);
    *first = 2;
    return value;
}
