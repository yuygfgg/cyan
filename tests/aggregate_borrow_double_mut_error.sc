struct Holder {
    &mut int view;
};

int main() {
    int x = 0;
    Holder first = {&mut x};
    Holder second = {&mut x};
    return 0;
}
