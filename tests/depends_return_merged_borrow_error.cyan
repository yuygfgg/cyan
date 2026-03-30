&int choose(&int a, &int b, bool cond) depends(return on a) {
    &int x = a;
    if (cond) {
        x = a;
    } else {
        x = b;
    }
    return x;
}

int main() {
    int a = 1;
    int b = 2;
    return *choose(&a, &b, false);
}
