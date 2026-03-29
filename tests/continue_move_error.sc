struct Token {
    int value;
};

impl drop(&mut Token token) {}

Token make(int value) {
    Token token = {value};
    return token;
}

int consume(Token token) {
    return token.value;
}

int bad() {
    Token token = make(1);

    while (true) {
        consume(token);
        continue;
    }

    return 0;
}
