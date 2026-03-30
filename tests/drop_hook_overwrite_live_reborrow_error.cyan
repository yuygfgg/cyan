struct Holder {
    &mut int view;
};

impl drop(&mut Holder self) {
    *self.view = 1;
}

int main() {
    int x = 0;
    int y = 0;
    Holder h = {&mut x};
    &mut int child = &mut *h.view;
    h = {&mut y};
    *child = 2;
    return x;
}
