int main() {
    int[1] arr = [5];
    &mut int[1] whole = &mut arr;
    []int s = whole;
    int y = s[0];
    return y;
}
