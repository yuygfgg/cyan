struct Token {
    int value;
};

impl drop(&mut Token token) {}

int read(&Token token) {
    return token.value;
}

int main() {
    []Token values = [{4}, {8}];
    if (read(values[0]) != 4) {
        return 1;
    }
    if (read(values[1]) != 8) {
        return 1;
    }
    return 0;
}
