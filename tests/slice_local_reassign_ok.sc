int main() {
    int[1] a = [1];
    int[1] b = [2];
    []int s = a;
    s = b;
    return s[0];
}
