int main() {
    int a = 1;
    int b = 2;
    int i = 0;
    &int x = &a;
    while (i < 2) {
        {
            &mut int m = &mut b;
            *m = *m;
        }
        if (i == 0) {
            x = &b;
        }
        i = i + 1;
    }
    return *x;
}
