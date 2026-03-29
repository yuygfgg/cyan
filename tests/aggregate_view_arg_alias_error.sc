struct Holder {
    &mut int view;
};

void write_both(Holder holder, &mut int value) {
    *holder.view = 1;
    *value = 2;
}

int main() {
    int x = 0;
    Holder holder = {&mut x};
    write_both(holder, &mut x);
    return 0;
}
