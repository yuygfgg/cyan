int main() {
    int x = 10;
    int y = 20;
    []&int tail = [];
    {
        []&int views = [&x, &y];
        tail = subslice(views, 1, 1);
    }
    y = 99;
    return *tail[0] - 99;
}
