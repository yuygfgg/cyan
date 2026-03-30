int main() {
    int x = 1;
    int y = 2;
    int pick = 0;
    &mut int pa = &mut x;
    &mut int pb = &mut y;
    &int s = pa;
    if (pick == 0) {
        s = pa;
    } else {
        s = pb;
    }
    return *s - 1;
}
