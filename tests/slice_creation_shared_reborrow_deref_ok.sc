int head([]int s) {
    return s[0];
}

int main() {
    int[4] arr = [1, 2, 3, 4];
    &mut int[4] whole = &mut arr;
    []int view = *whole;
    return view[1] + head(*whole);
}
