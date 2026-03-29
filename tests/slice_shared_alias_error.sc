int main() {
    int x = 0;
    int y = 0;
    []&int views = [&x, &y];
    y = 1;
    return *views[1];
}
