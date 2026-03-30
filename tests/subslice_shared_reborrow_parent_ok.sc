int main() {
    int[4] arr = [1, 2, 3, 4];
    &mut int[4] whole = &mut arr;
    []int view = whole;
    []int head = subslice(view, 0, 2);
    return head[0] + head[1];
}
