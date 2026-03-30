struct Holder {
    &int view;
};

int main() {
    int x = 1;
    int y = 2;
    Holder h = {&x};
    &Holder alias = &h;
    h.view = &y;
    return 0;
}
