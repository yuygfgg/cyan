struct Token {
    int value;
};

impl drop(&mut Token token) {}

int read(&Token token) {
    return token.value;
}

[]Token tokens(&Token[2] values) {
    return values;
}

&Token first([]Token values) {
    return &values[0];
}

int main() {
    Token[2] values = [{4}, {8}];
    []Token view = tokens(&values);
    if (read(view[0]) != 4) {
        return 1;
    }
    if (read(first(view)) != 4) {
        return 1;
    }
    return 0;
}
