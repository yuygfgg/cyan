int main() {
    int[1] arr = [0];
    &mut int[1] whole = &mut arr;
    {
        []int s = whole;
        drop(whole);
    }
    &mut int[1] again = whole;
    return 0;
}
