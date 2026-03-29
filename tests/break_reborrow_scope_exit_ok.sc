struct Box {
    int x;
};

int main() {
    Box b = {0};
    &mut Box p = &mut b;
    while (true) {
        &mut Box q = p;
        break;
    }
    p.x = 1;
    return b.x - 1;
}
