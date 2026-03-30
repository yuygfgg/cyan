int main() {
    int x = 10;
    int y = 20;
    &int elem = &x;
    {
        []&int views = [&x, &y];
        elem = views[1];
    }
    y = 99;
    return *elem - 99;
}
