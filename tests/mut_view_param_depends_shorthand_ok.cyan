struct Pair {
    []const char left;
    []const char right;
};

void fill(&mut Pair out, []const char text, []const char special)
    depends(out on text, out.right on special) {
    out.left = subslice(text, 0, 1);
    out.right = special;
}

int main() {
    Pair pair = {subslice("", 0, 0), subslice("", 0, 0)};
    fill(&mut pair, subslice("abcd", 0, 4), subslice("ZZ", 0, 2));
    if (pair.left[0] != 'a') {
        return 1;
    }
    if (pair.right[0] != 'Z') {
        return 2;
    }
    return 0;
}
