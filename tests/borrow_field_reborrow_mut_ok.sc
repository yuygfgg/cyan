struct Holder {
    &mut int view;
};

int main() {
    int x = 0;
    {
        Holder holder = {&mut x};
        &mut int again = &mut *holder.view;
        *again = 7;
    }
    return x - 7;
}
