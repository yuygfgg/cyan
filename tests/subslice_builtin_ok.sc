[]char middle(&char[12] text) {
    return subslice(text, 1, 3);
}

int main() {
    char[12] text = "hello world";
    []char whole = subslice(text, 0, 11);
    []char word = subslice(whole, 6, 5);
    []const char ell = middle(&text);

    if (len(whole) != 11) {
        return 1;
    }
    if (len(word) != 5) {
        return 2;
    }
    if (word[0] != 'w') {
        return 3;
    }
    if (word[4] != 'd') {
        return 4;
    }
    if (ell[0] != 'e') {
        return 5;
    }
    if (ell[2] != 'l') {
        return 6;
    }
    return 0;
}
