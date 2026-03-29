struct Holder {
    &mut int view;
};

int main() {
    int x = 0;
    int y = 0;
    Holder holder = {&mut y};
    &int shared = &x;
    holder = {&mut x};
    return 0;
}
