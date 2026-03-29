struct Pair {
    int left;
    int right;
};

int main() {
    Pair pair = {1, 2};
    &mut Pair ref = &mut pair;
    ref.left = 5;
    ref.right++;
    return pair.left + pair.right - 8;
}
