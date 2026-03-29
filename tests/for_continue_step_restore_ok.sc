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

int main() {
    int sum = 0;

    for (Token token = make(1); sum < 3; token = make(sum + 2)) {
        sum = sum + consume(token);
        continue;
    }

    if (sum == 4) {
        return 0;
    }
    return 1;
}
