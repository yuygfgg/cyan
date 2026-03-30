int main() {
    int x = 0;
    &mut int p = &mut x;
    {
        &int s = p;
        drop(p);
    }
    &mut int again = p;
    return 0;
}
