int main() {
    int[1] arr = [0];
    &mut int[1] whole = &mut arr;
    whole[0] = 1;
    drop(whole);
    return arr[0] - 1;
}
