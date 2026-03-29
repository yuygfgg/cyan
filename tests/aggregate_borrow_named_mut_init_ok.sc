struct Holder {
    &mut int view;
};

int main() {
    int x = 0;
    &mut int parent = &mut x;
    {
        Holder holder = {parent};
        *holder.view = 7;
    }
    *parent = 8;
    return x - 8;
}
