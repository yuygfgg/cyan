void set_one(&mut int value) {
    *value = 1;
}

void forward(&mut int value) {
    set_one(&mut value);
}

int main() {
    int value = 0;
    forward(&mut value);
    return value - 1;
}
