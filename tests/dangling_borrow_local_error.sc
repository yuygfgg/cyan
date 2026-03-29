int main() {
    int fallback = 0;
    &int alias = &fallback;
    {
        int inner = 1;
        alias = &inner;
    }
    return 0;
}
