int main() {
    int[2] arr = [1, 2];
    []int base = arr;
    []int s = arr;
    s = arr;
    return base[0] + s[1] - 3;
}
